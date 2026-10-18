// song library: find mp3s, tidy titles, decode+analyse+map on demand, cache maps.
// ids: id = hash(file stem + size) (12 hex), so renames of the folder keep ids but edits to the file change it.
// cache: <cacheDir>/<id>.v<kMapperVersion>.s<seed>.pulse (mapper::write format). miss => decode + analyse + generate
//   (~2-7 s on console) then write. bump kMapperVersion (library.h) whenever mapper/analysis output changes.
// !: the console title sandbox refuses opendir() on most paths (EPERM, seen on /mnt/*, /data, /download0); fopen of files
//   works. so scanDir falls back to readIndex(): <dir>/index.txt lines "relative/path.mp3[<TAB>size]" (tools/make-index.py).
//   /app0/assets/songs (the app folder) is the supported location; listing there is expected but unverified.
#include "library.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "analysis.h"
#include "platform.h"
#include "tags.h"

#define PULSE_TITLE "PPSA99993"

namespace library {

namespace {

// case-insensitive '.mp3' suffix test.
bool endsWithMp3(const std::string &n) {
  if (n.size() < 5) return false;
  const char *e = n.c_str() + n.size() - 4;
  return (e[0] == '.') && (e[1] | 32) == 'm' && (e[2] | 32) == 'p' && e[3] == '3';
}

// stable 48-bit FNV-style id from name stem and byte size; also the key for scores, seeds and cache files.
std::string hexId(const std::string &stem, long size) {
  unsigned long long h = 1469598103934665603ull;
  for (unsigned char c : stem) h = (h ^ c) * 1099511628211ull;
  h = (h ^ (unsigned long long)size) * 1099511628211ull;
  char buf[24];
  std::snprintf(buf, sizeof buf, "%012llx", h & 0xffffffffffffull);
  return buf;
}

// Stereo 48 kHz to mono at the analysis rate, averaging the source samples each output sample covers.
// decoded 48 kHz stereo int16 -> mono float at analysis::kSampleRate by box-averaging source samples.
std::vector<float> toAnalysisRate(const audio::Track &t) {
  const double ratio = (double)audio::kRate / analysis::kSampleRate;
  const size_t n = (size_t)(t.frames() / ratio);
  std::vector<float> out(n);
  for (size_t i = 0; i < n; i++) {
    const size_t a = (size_t)(i * ratio), b = std::max(a + 1, (size_t)((i + 1) * ratio));
    float sum = 0;
    for (size_t k = a; k < b && k < t.frames(); k++) sum += (t.pcm[2 * k] + t.pcm[2 * k + 1]) * (0.5f / 32768.0f);
    out[i] = sum / (b - a);
  }
  return out;
}

}  // namespace

namespace {

// adds one song (id, tidied title from ID3 tags or file name via tags::display); skips duplicates by id.
void addFile(const std::string &path, const std::string &name, long size, std::vector<Entry> &list) {
  Entry en;
  en.path = path;
  const std::string stem = name.substr(0, name.size() - 4);
  en.id = hexId(stem, size);
  tags::Info t;
  tags::read(path.c_str(), t);
  en.title = tags::display(t, stem);
  const bool dup = std::any_of(list.begin(), list.end(), [&](const Entry &o) { return o.id == en.id; });
  if (!dup) list.push_back(en);
}

// A title's sandbox may refuse to list a folder yet open known files: `index.txt` there holds one
// "relative/path.mp3" per line, optionally followed by a tab and the size in bytes. Tolerant of hand-made lists (see below).
// reads <dir>/index.txt (see header). size optional: measured with fopen/fseek when absent. false if no index file.
bool readIndex(const std::string &dir, std::vector<Entry> &list) {
  FILE *f = std::fopen((dir + "/index.txt").c_str(), "r");
  if (!f) return false;
  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l.erase(0, 3);
    const size_t tab = l.rfind('\t');
    std::string rel = tab == std::string::npos ? l : l.substr(0, tab);
    // lists made by hand or by Windows tools: backslashes, "./", and full paths such as "E:\\PPSA99993\\assets\\songs\\Artist\\a.mp3"
    for (char &c : rel)
      if (c == '\\') c = '/';
    if (rel.compare(0, 2, "./") == 0) rel.erase(0, 2);
    long size = tab == std::string::npos ? 0 : std::atol(l.c_str() + tab + 1);
    FILE *probe = std::fopen((dir + "/" + rel).c_str(), "rb");
    if (!probe) {
      std::string low = rel;
      for (char &c : low) c = (char)std::tolower((unsigned char)c);
      const size_t at = low.find("/songs/");
      if (at != std::string::npos) rel.erase(0, at + 7);
      else if (low.compare(0, 6, "songs/") == 0) rel.erase(0, 6);
      probe = std::fopen((dir + "/" + rel).c_str(), "rb");
    }
    const size_t slash = rel.rfind('/');
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    if (!probe) continue;
    if (size <= 0) {
      std::fseek(probe, 0, SEEK_END);
      size = std::ftell(probe);
    }
    std::fclose(probe);
    if (endsWithMp3(name)) addFile(dir + "/" + rel, name, size, list);
  }
  std::fclose(f);
  return true;
}

struct DirItem {
  std::string name;
  bool dir;
};

// lists one folder. normal path: opendir/readdir. console: the title sandbox refuses opendir on every path tried so far, so on
// failure (probe=true: once per root folder) other routes are tried and every outcome is logged as errno numbers:
// raw open(O_DIRECTORY) + getdents, plain open + getdents, and the same on the sandbox's real mount path for /app0 and /download0.
// returns false if nothing could list it. d_type tells folders from files; DT_UNKNOWN falls back to stat.
bool listDir(const std::string &dir, std::vector<DirItem> &out, bool probe) {
  out.clear();
  auto kind = [&](const std::string &name, unsigned char type) {
    if (type == DT_DIR) return true;
    if (type != DT_UNKNOWN) return false;
    struct stat st;
    return stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  };
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') out.push_back({e->d_name, kind(e->d_name, e->d_type)});
    closedir(d);
    return true;
  }
  const int first = errno;
#ifdef PLATFORM_PS5
  std::string alt = dir;
  for (const char *root : {"/app0", "/download0"})
    if (dir.compare(0, std::strlen(root), root) == 0) alt = std::string("/mnt/sandbox/" PULSE_TITLE "_000") + dir;
  struct Try {
    const char *label;
    const std::string &path;
    int flags;
  };
  const Try tries[] = {{"open(dir)", dir, O_RDONLY | O_DIRECTORY}, {"open", dir, O_RDONLY}, {"alt(dir)", alt, O_RDONLY | O_DIRECTORY}, {"alt", alt, O_RDONLY}};
  std::string report = "opendir " + std::to_string(first);
  for (const Try &t : tries) {
    if (t.path == dir && t.label[0] == 'a') continue;
    const int fd = open(t.path.c_str(), t.flags);
    if (fd < 0) {
      report += std::string(", ") + t.label + " " + std::to_string(errno);
      continue;
    }
    char buf[8192];
    long total = 0;
    for (int n; (n = getdents(fd, buf, sizeof buf)) > 0;) {
      for (int off = 0; off < n;) {
        const dirent *e = (const dirent *)(buf + off);
        if (e->d_name[0] != '.' && e->d_reclen > 0) out.push_back({e->d_name, kind(e->d_name, e->d_type)}), total++;
        off += e->d_reclen > 0 ? e->d_reclen : n;
      }
    }
    close(fd);
    if (probe) platform::log("library: %s: listed through %s (%ld entries)", dir.c_str(), t.label, total);
    return true;
  }
  if (probe) platform::log("library: %s: cannot list (%s)", dir.c_str(), report.c_str());
#else
  (void)probe;
#endif
  errno = first;
  return false;
}

// recursive (depth<=4) listing of mp3s under dir. a folder that cannot be listed at depth 0 falls back to readIndex(dir).
void scanDir(const std::string &dir, int depth, std::vector<Entry> &list) {
  std::vector<DirItem> items;
  if (!listDir(dir, items, depth == 0)) {
    if (depth == 0 && readIndex(dir, list)) platform::log("library: %s: read index.txt instead", dir.c_str());
    return;
  }
  for (const DirItem &it : items) {
    const std::string path = dir + "/" + it.name;
    if (it.dir) {
      if (depth < 4) scanDir(path, depth + 1, list);
      continue;
    }
    if (!endsWithMp3(it.name)) continue;
    long size = 0;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
      size = (long)st.st_size;
    } else if (FILE *m = std::fopen(path.c_str(), "rb")) {
      std::fseek(m, 0, SEEK_END);
      size = std::ftell(m);
      std::fclose(m);
    }
    addFile(path, it.name, size, list);
  }
}

}  // namespace

bool scanIndex(const std::string &dir, std::vector<Entry> &list) { return readIndex(dir, list); }

// all dirs scanned in order, de-duplicated by id, sorted case-insensitively by title. logs songs found per dir. T: main thread.
std::vector<Entry> scan(const std::vector<std::string> &dirs) {
  std::vector<Entry> list;
  for (const std::string &dir : dirs) {
    const size_t before = list.size();
    scanDir(dir, 0, list);
    if (list.size() > before) platform::log("library: %zu songs from %s", list.size() - before, dir.c_str());
  }
  auto lower = [](std::string x) {
    for (char &c : x) c = (char)std::tolower((unsigned char)c);
    return x;
  };
  std::sort(list.begin(), list.end(), [&](const Entry &a, const Entry &b) { return lower(a.title) < lower(b.title); });
  return list;
}

// map cache file for (song, seed, hands) at the current kMapperVersion: <id>.v3.s1.pulse, one-handed maps <id>.v3.s1.h1.pulse.
std::string cachePath(const Entry &e, const std::string &cacheDir, int seed, int hands) {
  char buf[56];
  if (hands) std::snprintf(buf, sizeof buf, ".v%d.s%d.h%d.pulse", kMapperVersion, seed, hands);
  else std::snprintf(buf, sizeof buf, ".v%d.s%d.pulse", kMapperVersion, seed);
  return cacheDir + "/" + e.id + buf;
}

// bpm/duration/note counts from the cache if present (no decode); false if never analysed. for the select screen.
bool cachedInfo(const Entry &e, const std::string &cacheDir, int seed, int hands, Info &out) {
  mapper::Song s;
  if (!mapper::read(cachePath(e, cacheDir, seed, hands).c_str(), s) || s.maps.size() < 4) return false;
  out.bpm = s.bpm, out.duration = s.duration;
  for (int d = 0; d < 4; d++) out.notes[d] = (int)s.maps[d].notes.size();
  return true;
}

// decode mp3 -> track (48 kHz stereo int16); song = cached map or analyse+generate+write cache. false if undecodable or no beat
// found (<8 beats or <4 bars). blocking: UI shows a loading frame first. memory: caller must have freed the previous song
// (console heap is finite; see main.cpp loadSongEntry).
bool load(const Entry &e, const std::string &cacheDir, int seed, int hands, audio::Track &track, mapper::Song &song) {
  if (!audio::decodeMp3(e.path.c_str(), track)) {
    platform::log("library: cannot decode %s (failed)", e.path.c_str());
    return false;
  }
  platform::log("library: decoded %.1f s", track.frames() / (double)audio::kRate);
  const std::string cached = cachePath(e, cacheDir, seed, hands);
  if (mapper::read(cached.c_str(), song) && song.maps.size() >= 4) return true;
  platform::log("library: analysing");
  const analysis::Result r = analysis::analyse(toAnalysisRate(track));
  if (r.beats.size() < 8 || r.bars.size() < 4) {
    platform::log("library: no beat found in %s (failed)", e.path.c_str());
    return false;
  }
  platform::log("library: analysed %.1f BPM, %zu beats", r.bpm, r.beats.size());
  song = mapper::generate(r, (unsigned)seed, hands);
  mkdir(cacheDir.c_str(), 0777);
  platform::log("library: mapped");
  mapper::write(song, cached.c_str());
  platform::log("library: analysed %s: %.1f BPM, %zu/%zu/%zu/%zu notes", e.title.c_str(), r.bpm, song.maps[0].notes.size(), song.maps[1].notes.size(),
                song.maps[2].notes.size(), song.maps[3].notes.size());
  return true;
}

}  // namespace library
