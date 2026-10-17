// song library: find mp3s, tidy titles, decode+analyse+map on demand, cache maps.
// ids: id = hash(file stem + size) (12 hex), so renames of the folder keep ids but edits to the file change it.
// cache: <cacheDir>/<id>.v<kMapperVersion>.s<seed>.pulse (mapper::write format). miss => decode + analyse + generate
//   (~2-7 s on console) then write. bump kMapperVersion (library.h) whenever mapper/analysis output changes.
// !: the console title sandbox refuses opendir() on most paths (EPERM, seen on /mnt/*, /data, /download0); fopen of files
//   works. so scanDir falls back to readIndex(): <dir>/index.txt lines "relative/path.mp3[<TAB>size]" (tools/make-index.py).
//   /app0/assets/songs (the app folder) is the supported location; listing there is expected but unverified.
#include "library.h"

#include <dirent.h>
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
// "relative/path.mp3" per line, optionally followed by a tab and the size in bytes.
// reads <dir>/index.txt (see header). size optional: measured with fopen/fseek when absent. false if no index file.
bool readIndex(const std::string &dir, std::vector<Entry> &list) {
  FILE *f = std::fopen((dir + "/index.txt").c_str(), "r");
  if (!f) return false;
  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    const size_t tab = l.rfind('\t');
    const std::string rel = tab == std::string::npos ? l : l.substr(0, tab);
    const size_t slash = rel.rfind('/');
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    long size = tab == std::string::npos ? 0 : std::atol(l.c_str() + tab + 1);
    if (size <= 0) {
      FILE *m = std::fopen((dir + "/" + rel).c_str(), "rb");
      if (!m) continue;
      std::fseek(m, 0, SEEK_END);
      size = std::ftell(m);
      std::fclose(m);
    }
    if (endsWithMp3(name)) addFile(dir + "/" + rel, name, size, list);
  }
  std::fclose(f);
  return true;
}

// recursive (depth<=4) listing of mp3s under dir. opendir failure at depth 0 => try readIndex(dir). logs the cause.
void scanDir(const std::string &dir, int depth, std::vector<Entry> &list) {
  DIR *d = opendir(dir.c_str());
  if (!d) {
    const int err = errno;
    if (depth == 0 && readIndex(dir, list)) platform::log("library: %s: cannot list (errno %d), read index.txt", dir.c_str(), err);
    else if (depth == 0 && err != ENOENT) platform::log("library: %s: cannot list (errno %d)", dir.c_str(), err);
    return;
  }
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name[0] == '.') continue;
    const std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < 4) scanDir(path, depth + 1, list);
      continue;
    }
    if (!endsWithMp3(name)) continue;
    addFile(path, name, (long)st.st_size, list);
  }
  closedir(d);
}

}  // namespace

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

// map cache file for (song, seed) at the current kMapperVersion.
std::string cachePath(const Entry &e, const std::string &cacheDir, int seed) {
  char buf[48];
  std::snprintf(buf, sizeof buf, ".v%d.s%d.pulse", kMapperVersion, seed);
  return cacheDir + "/" + e.id + buf;
}

// bpm/duration/note counts from the cache if present (no decode); false if never analysed. for the select screen.
bool cachedInfo(const Entry &e, const std::string &cacheDir, int seed, Info &out) {
  mapper::Song s;
  if (!mapper::read(cachePath(e, cacheDir, seed).c_str(), s) || s.maps.size() < 4) return false;
  out.bpm = s.bpm, out.duration = s.duration;
  for (int d = 0; d < 4; d++) out.notes[d] = (int)s.maps[d].notes.size();
  return true;
}

// decode mp3 -> track (48 kHz stereo int16); song = cached map or analyse+generate+write cache. false if undecodable or no beat
// found (<8 beats or <4 bars). blocking: UI shows a loading frame first. memory: caller must have freed the previous song
// (console heap is finite; see main.cpp loadSongEntry).
bool load(const Entry &e, const std::string &cacheDir, int seed, audio::Track &track, mapper::Song &song) {
  if (!audio::decodeMp3(e.path.c_str(), track)) {
    platform::log("library: cannot decode %s (failed)", e.path.c_str());
    return false;
  }
  platform::log("library: decoded %.1f s", track.frames() / (double)audio::kRate);
  const std::string cached = cachePath(e, cacheDir, seed);
  if (mapper::read(cached.c_str(), song) && song.maps.size() >= 4) return true;
  platform::log("library: analysing");
  const analysis::Result r = analysis::analyse(toAnalysisRate(track));
  if (r.beats.size() < 8 || r.bars.size() < 4) {
    platform::log("library: no beat found in %s (failed)", e.path.c_str());
    return false;
  }
  platform::log("library: analysed %.1f BPM, %zu beats", r.bpm, r.beats.size());
  song = mapper::generate(r, (unsigned)seed);
  mkdir(cacheDir.c_str(), 0777);
  platform::log("library: mapped");
  mapper::write(song, cached.c_str());
  platform::log("library: analysed %s: %.1f BPM, %zu/%zu/%zu/%zu notes", e.title.c_str(), r.bpm, song.maps[0].notes.size(), song.maps[1].notes.size(),
                song.maps[2].notes.size(), song.maps[3].notes.size());
  return true;
}

}  // namespace library
