// Checks for everything around the rules engine: tempo detection on synthetic tracks, ID3 reading and title tidying,
// the scores file, MP3 decoding, and the song library (scan, index.txt forms, map cache). `make test` runs it after game_test.
// The demo song (ps5/demo) is the one real input: a 128 BPM, 120.5 s synthesised house track.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "analysis.h"
#include "library.h"
#include "mapper.h"
#include "mp3.h"
#include "scores.h"
#include "tags.h"

#define DEMO "ps5/demo/Pulse Demo - Neon Run.mp3"

// library.cpp logs through the platform layer; the test has no window, so the messages are dropped.
namespace platform {
void log(const char *, ...) {}
}  // namespace platform

static int failures;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      failures++;                                      \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

static std::string tmp;  // scratch folder, made once

static std::string path(const std::string &name) { return tmp + "/" + name; }

static void writeFile(const std::string &p, const std::string &bytes) {
  for (size_t at = p.find('/', 1); at != std::string::npos; at = p.find('/', at + 1)) mkdir(p.substr(0, at).c_str(), 0777);
  std::ofstream(p, std::ios::binary) << bytes;
}

static std::string readFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// kick on every beat and a snare-ish noise burst on beats 2 and 4, at the analysis rate. no off-beat hats: they would make a
// double-time reading as valid as the true one.
static std::vector<float> clickTrack(double bpm, double seconds) {
  const int rate = analysis::kSampleRate;
  std::vector<float> x((size_t)(seconds * rate), 0.0f);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> noise(-1, 1);
  const double beat = 60.0 / bpm;
  auto add = [&](double at, int kind) {
    const size_t s = (size_t)(at * rate);
    const int len = kind == 0 ? rate / 10 : rate / 25;
    for (int i = 0; i < len && s + i < x.size(); i++) {
      const float env = std::exp(-i / (len / 5.0f));
      const float v = kind == 0 ? std::sin(2 * 3.14159265f * 60 * i / rate) : noise(rng);
      x[s + i] += 0.8f * env * v;
    }
  };
  int n = 0;
  for (double t = 0; t < seconds - 0.2; t += beat, n++) {
    add(t, 0);
    if (n % 2) add(t, 1);
  }
  return x;
}

// 1. tempo: the beat grid on a known track.
static void testTempo() {
  for (double bpm : {100.0, 120.0, 140.0}) {
    const analysis::Result r = analysis::analyse(clickTrack(bpm, 40));
    CHECK(std::fabs(r.bpm - bpm) < 1.5, "click track at %.0f BPM analysed as %.2f", bpm, r.bpm);
    CHECK(r.beats.size() > 40 * bpm / 60 * 0.8, "%.0f BPM: only %zu beats found", bpm, r.beats.size());
    CHECK(r.beatConfidence > 0.05, "%.0f BPM: confidence %.2f", bpm, r.beatConfidence);
    double worst = 0;
    for (size_t i = 1; i < r.beats.size(); i++) worst = std::max(worst, std::fabs((r.beats[i] - r.beats[i - 1]) - 60.0 / bpm));
    CHECK(worst < 0.05, "%.0f BPM: beats wander by %.0f ms", bpm, worst * 1000);
    CHECK(r.bars.size() >= 5, "%.0f BPM: %zu bars", bpm, r.bars.size());
    for (const auto &b : r.bars) CHECK(b.end > b.start && b.intensity >= 0 && b.intensity <= 1.001f, "bad bar %.2f-%.2f intensity %.2f", b.start, b.end, (double)b.intensity);
  }
  const analysis::Result silence = analysis::analyse(std::vector<float>(analysis::kSampleRate * 20, 0.0f));
  CHECK(silence.peaks.empty(), "silence has %zu onsets", silence.peaks.size());
  CHECK(std::string(analysis::name(analysis::Section::Drop)) != "", "section names exist");
}

// big-endian-ish helpers for building ID3 tags
static std::string syncsafe(size_t n) { return {(char)((n >> 21) & 127), (char)((n >> 14) & 127), (char)((n >> 7) & 127), (char)(n & 127)}; }
static std::string be(size_t n) { return {(char)(n >> 24), (char)(n >> 16), (char)(n >> 8), (char)n}; }

// ID3v2.3 tag with frames (id, text) in ISO-8859-1, followed by one MPEG-less byte so the file is not empty.
static std::string id3v23(const std::vector<std::pair<std::string, std::string>> &frames) {
  std::string body;
  for (const auto &f : frames) body += f.first + be(f.second.size() + 1) + std::string("\0\0", 2) + std::string(1, '\0') + f.second;
  return "ID3" + std::string("\3\0\0", 3) + syncsafe(body.size()) + body;
}

// ID3v2.2 (3-letter ids, 6-byte frame headers).
static std::string id3v22(const std::string &artist, const std::string &title) {
  auto frame = [](const std::string &id, const std::string &text) {
    const size_t n = text.size() + 1;
    return id + std::string{(char)(n >> 16), (char)(n >> 8), (char)n} + std::string(1, '\0') + text;
  };
  const std::string body = frame("TP1", artist) + frame("TT2", title);
  return "ID3" + std::string("\2\0\0", 3) + syncsafe(body.size()) + body;
}

// 16x8 PNG, left half red and right half blue: the cover is cropped to the middle 8x8, so a quarter of it red and a quarter blue.
static const unsigned char kPng[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x7f, 0x14, 0xe8, 0xc0, 0x00, 0x00, 0x00, 0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc, 0xcf, 0x80, 0x1d, 0x30, 0x32, 0x60, 0x97, 0x61, 0x62, 0x20, 0x11, 0x8c, 0x6a, 0x20, 0x06, 0x00, 0x00, 0x00, 0x70, 0x02, 0x0f, 0xa1, 0xae, 0x69, 0x65, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

// APIC frame (v2.3): encoding, mime, picture type, description, data.
static std::string apic(const std::string &mime) {
  return std::string(1, '\0') + mime + std::string(1, '\0') + std::string(1, '\3') + "front" + std::string(1, '\0') + std::string((const char *)kPng, sizeof kPng);
}

// 2. tags: reading, and how names are tidied for the list.
static void testTags() {
  tags::Info t;
  writeFile(path("v23.mp3"), id3v23({{"TPE1", "Some Artist"}, {"TIT2", "Some Title"}}));
  CHECK(tags::read(path("v23.mp3").c_str(), t) && t.artist == "Some Artist" && t.title == "Some Title", "v2.3: '%s' / '%s'", t.artist.c_str(), t.title.c_str());
  t = {};
  writeFile(path("v22.mp3"), id3v22("Old Artist", "Old Title"));
  CHECK(tags::read(path("v22.mp3").c_str(), t) && t.artist == "Old Artist" && t.title == "Old Title", "v2.2: '%s' / '%s'", t.artist.c_str(), t.title.c_str());
  t = {};
  writeFile(path("noid3.mp3"), "just some bytes");
  CHECK(!tags::read(path("noid3.mp3").c_str(), t), "a file without a tag must report none");
  CHECK(!tags::read(path("missing.mp3").c_str(), t), "a missing file must report none");
  t = {};
  writeFile(path("curly.mp3"), id3v23({{"TIT2", "Don\xB4t Stop"}}));  // Latin-1 acute accent in place of an apostrophe
  tags::read(path("curly.mp3").c_str(), t);
  CHECK(t.title == "Don't Stop", "acute accent should become an apostrophe: '%s'", t.title.c_str());
  std::vector<uint8_t> pic;
  CHECK(!tags::cover(path("v23.mp3").c_str(), pic), "no picture in a tag without one");
  {
    const std::string body = std::string("APIC") + be(apic("image/png").size()) + std::string("\0\0", 2) + apic("image/png");
    writeFile(path("art.mp3"), "ID3" + std::string("\3\0\0", 3) + syncsafe(body.size()) + body);
    CHECK(tags::cover(path("art.mp3").c_str(), pic) && pic.size() == (size_t)tags::kArtSize * tags::kArtSize * 3, "cover decodes to %dx%d RGB", tags::kArtSize, tags::kArtSize);
    if (pic.size() == (size_t)tags::kArtSize * tags::kArtSize * 3) {
      const int mid = tags::kArtSize / 2;
      const uint8_t *l = &pic[((size_t)mid * tags::kArtSize + 10) * 3], *r = &pic[((size_t)mid * tags::kArtSize + tags::kArtSize - 10) * 3];
      CHECK(l[0] > 200 && l[2] < 50, "left of the cover is red (%d,%d,%d)", l[0], l[1], l[2]);
      CHECK(r[2] > 200 && r[0] < 50, "right of the cover is blue (%d,%d,%d)", r[0], r[1], r[2]);
    }
    const std::string bad = std::string("APIC") + be(20) + std::string("\0\0", 2) + std::string(20, 'x');
    writeFile(path("badart.mp3"), "ID3" + std::string("\3\0\0", 3) + syncsafe(bad.size()) + bad);
    pic.clear();
    CHECK(!tags::cover(path("badart.mp3").c_str(), pic), "a picture frame that is not an image yields none");
  }

  auto show = [](const char *artist, const char *title, const char *stem) {
    tags::Info i;
    i.artist = artist;
    i.title = title;
    return tags::display(i, stem);
  };
  CHECK(show("Daft Punk", "Around The World", "x") == "Daft Punk - Around The World", "artist + title");
  CHECK(show("Daft Punk", "Daft Punk - Around The World", "x") == "Daft Punk - Around The World", "repeated artist prefix stripped: '%s'", show("Daft Punk", "Daft Punk - Around The World", "x").c_str());
  CHECK(show("", "", "01 - Artist - Song") == "Artist - Song", "track number dropped: '%s'", show("", "", "01 - Artist - Song").c_str());
  CHECK(show("", "", "Artist - Song (1)") == "Artist - Song", "copy mark dropped: '%s'", show("", "", "Artist - Song (1)").c_str());
  CHECK(show("", "", "Artist - Song www.example.com") .find("www.") == std::string::npos, "web address dropped: '%s'", show("", "", "Artist - Song www.example.com").c_str());
  CHECK(show("Only Artist", "", "Some File") == "Only Artist - Some File", "artist + file name: '%s'", show("Only Artist", "", "Some File").c_str());
  CHECK(show("", "", "Plain Name") == "Plain Name", "plain file name: '%s'", show("", "", "Plain Name").c_str());
  CHECK(show("", "", "Beyonc\xC3\xA9 - Halo") == "Beyonce - Halo", "accents folded: '%s'", show("", "", "Beyonc\xC3\xA9 - Halo").c_str());
  CHECK(show("", "", "\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE - \xD0\x97\xD0\xB2\xD0\xB5\xD0\xB7\xD0\xB4\xD0\xB0") == "\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE - \xD0\x97\xD0\xB2\xD0\xB5\xD0\xB7\xD0\xB4\xD0\xB0",
        "Cyrillic kept as it is");
  CHECK(show("", "", "Don\xE2\x80\x99t Stop - Me") == "Don't Stop - Me", "curly apostrophe: '%s'", show("", "", "Don\xE2\x80\x99t Stop - Me").c_str());
}

// 3. scores file: round trip, max-keeping, old five-field lines, junk.
static void testScores() {
  const std::string f = path("scores.txt");
  scores::init(f);
  CHECK(scores::get("abc", 2).plays == 0, "unplayed song has no plays");
  CHECK(scores::record("abc", 2, 1000, 40, 55, 30), "first run is a best");
  CHECK(!scores::record("abc", 2, 900, 50, 45, 10), "lower score is not a best");
  const scores::Best b = scores::get("abc", 2);
  CHECK(b.score == 1000 && b.plays == 2 && b.combo == 30, "kept best score/combo: %ld %d %d", b.score, b.plays, b.combo);
  CHECK(b.accuracy == 50 && b.rating == 55, "each field keeps its own max: acc %.1f rating %.1f", (double)b.accuracy, (double)b.rating);
  CHECK(scores::get("abc", 3).plays == 0, "difficulties are separate");
  CHECK(scores::record("abc~2^L", 1, 500, 20, 20, 5), "one-handed ids are separate keys");
  scores::init(f);  // reload from disk
  const scores::Best c = scores::get("abc", 2);
  CHECK(c.score == 1000 && c.plays == 2 && c.rating == 55, "survives reload: %ld %d %.1f", c.score, c.plays, (double)c.rating);
  CHECK(scores::get("abc~2^L", 1).score == 500, "one-handed key survives reload");
  writeFile(f, "old:1 777 33.5 12 3\nnot a score line\nnew:0 5 1.0 1 1 9.5\n");
  scores::init(f);
  CHECK(scores::get("old", 1).score == 777 && scores::get("old", 1).rating == 33.5f, "five-field line: rating defaults to accuracy (%.1f)", (double)scores::get("old", 1).rating);
  CHECK(scores::get("new", 0).rating == 9.5f, "six-field line");
  scores::init(path("does-not-exist.txt"));
  CHECK(scores::get("old", 1).plays == 0, "missing file gives an empty table");
  CHECK(std::fabs(scores::rating(50, 0, 0) - 50.0f * 50 / 60) < 1e-3, "no notes: accuracy half only");
}

// 4. MP3 decode of the demo song.
static void testMp3() {
  audio::Track tr;
  CHECK(audio::decodeMp3(DEMO, tr), "demo song should decode");
  const double secs = tr.frames() / (double)audio::kRate;
  CHECK(std::fabs(secs - 120.5) < 0.5, "demo song is 120.5 s, decoded %.2f s", secs);
  int peak = 0;
  for (int16_t s : tr.pcm) peak = std::max(peak, std::abs((int)s));
  CHECK(peak > 3000 && peak <= 32768, "peak level %d", peak);
  CHECK(!audio::decodeMp3(path("noid3.mp3").c_str(), tr) || tr.frames() == 0, "garbage is not audio");
  CHECK(!audio::decodeMp3(path("missing.mp3").c_str(), tr), "missing file");
  audio::Track demo;
  audio::decodeMp3(DEMO, demo);
  const analysis::Result r = analysis::analyse([&] {
    const audio::Track &tr = demo;
    std::vector<float> m(tr.frames() * analysis::kSampleRate / audio::kRate);
    for (size_t i = 0; i < m.size(); i++) m[i] = tr.pcm[2 * (i * audio::kRate / analysis::kSampleRate)] / 32768.0f;
    return m;
  }());
  CHECK(std::fabs(r.bpm - 128) < 1.5, "demo song is 128 BPM, analysed as %.2f", r.bpm);
}

// 5. library: scanning, index.txt forms, cache.
static void testLibrary() {
  const std::string demo = readFile(DEMO);
  writeFile(path("songs/Artist/a song.mp3"), demo);
  writeFile(path("songs/b song.MP3"), demo.substr(0, demo.size() - 1000));
  writeFile(path("songs/notes.txt"), "not music");
  auto found = library::scan({path("songs")});
  CHECK(found.size() == 2, "two mp3s expected (one in a sub-folder, one upper-case), got %zu", found.size());
  CHECK(found.size() == 2 && found[0].id != found[1].id && found[0].id.size() == 12, "ids are 12 hex and distinct");
  CHECK(library::scan({path("songs"), path("songs")}).size() == 2, "the same folder twice must not duplicate songs");
  const auto again = library::scan({path("songs")});
  CHECK(again.size() == 2 && again[0].id == found[0].id, "ids are stable between scans");

  // index.txt forms (the sandbox fallback): every sloppy spelling must find the one file, with or without a size
  const long size = (long)demo.size();
  std::string wantId;
  for (const auto &e : found)
    if (e.path.find("Artist/a song.mp3") != std::string::npos) wantId = e.id;
  CHECK(!wantId.empty(), "folder scan lists Artist/a song.mp3");
  const std::string sized = "\t" + std::to_string(size);
  writeFile(path("idx/Artist/a song.mp3"), demo);
  const struct { const char *line; bool sizeGiven; } forms[] = {{"Artist/a song.mp3", true}, {"Artist/a song.mp3", false}, {"Artist\\a song.mp3", false},
                                                                  {"./Artist/a song.mp3", false}, {"E:\\PPSA99993\\assets\\songs\\Artist\\a song.mp3", false},
                                                                  {"songs/Artist/a song.mp3", false}};
  for (const auto &form : forms) {
    writeFile(path("idx/index.txt"), std::string("\xEF\xBB\xBF") + form.line + (form.sizeGiven ? sized : "") + "\r\n");  // BOM + CRLF as Notepad writes
    std::vector<library::Entry> via;
    CHECK(library::scanIndex(path("idx"), via) && via.size() == 1, "index line '%s': %zu songs", form.line, via.size());
    CHECK(via.size() == 1 && via[0].id == wantId, "index line '%s' gives another id than the folder scan", form.line);
  }
  writeFile(path("idx/index.txt"), "Artist/gone.mp3\n\nArtist/a song.txt\n");
  std::vector<library::Entry> none;
  CHECK(library::scanIndex(path("idx"), none) && none.empty(), "missing files and non-mp3 lines are skipped");
  CHECK(!library::scanIndex(path("songs"), none), "no index.txt reports false");
  CHECK(library::scan({path("idx")}).size() == 1, "folder scan of the idx folder");

  // map cache: first load analyses and writes, then the cache answers without decoding
  const std::string cache = path("cache");
  library::Info info;
  CHECK(!library::cachedInfo(found[0], cache, 1, 0, info), "nothing cached yet");
  audio::Track track;
  mapper::Song song;
  CHECK(library::load(found[0], cache, 1, 0, track, song), "load should analyse and map the demo song");
  CHECK(song.maps.size() == 4, "four maps, got %zu", song.maps.size());
  for (const auto &m : song.maps) CHECK(mapper::validate(m).empty(), "map %s is playable", mapper::name(m.difficulty));
  CHECK(library::cachedInfo(found[0], cache, 1, 0, info), "cached after load");
  CHECK(std::fabs(info.bpm - 128) < 1.5 && info.notes[3] > info.notes[0], "cache info: %.1f BPM, notes %d..%d", info.bpm, info.notes[0], info.notes[3]);
  CHECK(!library::cachedInfo(found[0], cache, 2, 0, info), "another seed is not cached");
  CHECK(!library::cachedInfo(found[0], cache, 1, 1, info), "left-only maps are not cached");
  mapper::Song again2;
  CHECK(library::load(found[0], cache, 1, 1, track, again2), "left-only load");
  CHECK(library::cachedInfo(found[0], cache, 1, 1, info), "left-only cached after load");
  CHECK(!again2.maps.empty() && again2.maps[0].notes.size() > 5, "left-only map has notes");
  for (const auto &n : again2.maps[0].notes) CHECK(n.color == 0, "left-only map has a right-hand note");
}

int main() {
  char dir[] = "/tmp/pulse-support-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  tmp = dir;
  testTempo();
  testTags();
  testScores();
  testMp3();
  testLibrary();
  const std::string cleanup = "rm -rf " + tmp;
  if (std::system(cleanup.c_str())) std::printf("could not remove %s\n", tmp.c_str());
  std::printf(failures ? "%d FAILED\n" : "support: all passed\n", failures);
  return failures ? 1 : 0;
}
