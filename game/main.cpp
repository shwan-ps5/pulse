// Pulse: the game. Title, song select, play and results screens, with the PS VR2 headset and the Sense
// controllers as sabers on the console; on a PC it opens a window and the bot or the mouse plays.
//   desktop: game [--songs DIR]... [--diff easy|medium|hard|extreme] [--vr] [--bot] [--mouse]
//            game song.mp3 map.pulse [...]   plays that song straight away
//            --screen title|select|results --shot SECONDS file.ppm   writes one frame; --video out.mp4 renders a run
// Menus: stick or d-pad to move, trigger or Cross to choose, Circle to go back, Options pauses in a song.
// DualSense in a song: Circle toggles the autoplay bot, Square cycles the controller axis the blade lies along.
// R3 or a stick click recentres.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bot.h"
#include "game.h"
#include "gl.h"
#include "library.h"
#include "mapper.h"
#include "mp3.h"
#include "platform.h"
#include "render.h"
#include "scores.h"
#include "tags.h"
#include "vmath.h"
#include "vr.h"
#include "vr_eyes.h"

#ifdef PLATFORM_PS5
#define STORAGE "/download0/"
#else
#define STORAGE "build/"
#endif

namespace {

// ---- the song ----
audio::Track track;
#ifdef PLATFORM_PS5
// The runtime's app heap defaults to 128 MiB; it falls back to smaller sizes if the console cannot map this much.
extern "C" const size_t ps5_opengl_heap_size = 1024u * 1024u * 1024u;
extern "C" size_t ps5_opengl_heap_live_bytes(void);
#endif
std::mutex trackLock;  // held while the track is swapped for another song
std::atomic<long> playPos{0};
std::atomic<bool> playing{true};
std::mutex voiceLock;
struct Voice {
  long start;
  float freq;
};
std::vector<Voice> voices;

void fill(float *out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  if (!playing) return;
  std::unique_lock<std::mutex> tl(trackLock, std::try_to_lock);
  if (!tl.owns_lock()) return;
  const long pos = playPos;
  const size_t n = track.frames();
  for (int i = 0; i < frames; i++) {
    const long p = pos + i;
    if (p >= 0 && (size_t)p < n) out[2 * i] = track.pcm[2 * p] / 32768.0f * 0.8f, out[2 * i + 1] = track.pcm[2 * p + 1] / 32768.0f * 0.8f;
  }
  std::unique_lock<std::mutex> lock(voiceLock, std::try_to_lock);
  if (lock.owns_lock()) {
    for (size_t v = 0; v < voices.size();) {
      const Voice &c = voices[v];
      bool alive = false;
      for (int i = 0; i < frames; i++) {
        const long age = pos + i - c.start;
        if (age < 0 || age > 700) {
          alive = alive || age < 0;
          continue;
        }
        alive = true;
        const float s = 0.22f * std::sin(6.2831853f * c.freq * age / audio::kRate) * std::exp(-age / 140.0f);
        out[2 * i] += s, out[2 * i + 1] += s;
      }
      if (!alive) voices.erase(voices.begin() + v);
      else v++;
    }
  }
  playPos = pos + frames;
}

void click(double now, int colour) {
  std::lock_guard<std::mutex> lock(voiceLock);
  voices.push_back({(long)(now * audio::kRate), colour == 1 ? 1300.0f : 1900.0f});
  if (voices.size() > 64) voices.erase(voices.begin());
}

// ---- state ----
enum class Screen { Title, Select, Options, Help, Playing, Results };
Screen screen = Screen::Title;
mapper::Song song;
bool songLoaded = false;
int diff = 1;
std::unique_ptr<game::Session> session;
std::unique_ptr<game::Bot> bot;
std::vector<render::Fx> effects;
game::SaberPose sabers[2] = {{{-0.25f, 1.0f, -0.35f}, {-0.25f, 1.0f, -1.35f}}, {{0.25f, 1.0f, -0.35f}, {0.25f, 1.0f, -1.35f}}};
bool autoplay = false, paused = false, quitRequested = false;
std::vector<library::Entry> songs;
std::string songId, songTitle, statusLine;
int titleSel = 0, songSel = 0, pauseSel = 0, resultSel = 0, optionSel = 0, helpPage = 0;
std::string optionNote;
bool userCentred = false;
int paletteId = 0;
bool paletteSwap = false;
#ifdef PLATFORM_PS5
int bgMode = 0;  // off until tried on the headset
#else
int bgMode = 1;
#endif
const char *kBgNames[3] = {"OFF", "FLAT", "3D"};
std::map<std::string, int> seeds;  // the pattern a song is played with, by song id; 1 unless re-rolled
library::Info info;
bool haveInfo = false;
std::string infoKey, artFor;
struct Result {
  long score = 0;
  float accuracy = 0;
  int combo = 0, hits = 0, misses = 0;
  bool best = false;
} result;
render::Panel panel;
float senseX = 0, senseY = 0;
uint32_t sense = 0;  // the Sense controllers' buttons this frame, both hands
int bladeAxis;
constexpr double kLeadIn = 3.0;
const char *kDiffNames[4] = {"easy", "medium", "hard", "extreme"};
const char *kDiffLabels[4] = {"EASY", "MEDIUM", "HARD", "EXTREME"};
const float kAxes[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
const char *kAxisNames[6] = {"-z", "+z", "+y", "-y", "+x", "-x"};

int seedOf(const std::string &id) {
  const auto it = seeds.find(id);
  return it == seeds.end() ? 1 : it->second;
}

// The key a song's scores are filed under: another pattern of the same song is its own table.
std::string scoreKey(const std::string &id) {
  const int sd = seedOf(id);
  return sd == 1 ? id : id + "~" + std::to_string(sd);
}

void loadSettings() {
  if (FILE *f = std::fopen(STORAGE "settings.txt", "r")) {
    int p = 0, sw = 0, bg = bgMode;
    const int got = std::fscanf(f, "%d %d %d", &p, &sw, &bg);
    if (got >= 2 && p >= 0 && p < render::paletteCount()) paletteId = p, paletteSwap = sw != 0;
    if (got == 3 && bg >= 0 && bg < 3) bgMode = bg;
    std::fclose(f);
  }
  if (FILE *f = std::fopen(STORAGE "seeds.txt", "r")) {
    char id[32];
    int sd;
    while (std::fscanf(f, "%31s %d", id, &sd) == 2) seeds[id] = sd;
    std::fclose(f);
  }
  render::setPalette(paletteId, paletteSwap);
}

void saveSettings() {
  render::setPalette(paletteId, paletteSwap);
  if (FILE *f = std::fopen(STORAGE "settings.txt", "w")) {
    std::fprintf(f, "%d %d %d\n", paletteId, paletteSwap ? 1 : 0, bgMode);
    std::fclose(f);
  }
}

void saveSeeds() {
  if (FILE *f = std::fopen(STORAGE "seeds.txt", "w")) {
    for (const auto &kv : seeds)
      if (kv.second != 1) std::fprintf(f, "%s %d\n", kv.first.c_str(), kv.second);
    std::fclose(f);
  }
}

std::string clock(double seconds) {
  char b[16];
  std::snprintf(b, sizeof b, "%d:%02d", (int)seconds / 60, (int)seconds % 60);
  return b;
}

std::string clip(const std::string &s, size_t n) {
  if (s.size() <= n) return s;
  size_t k = n - 2;
  while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
  return s.substr(0, k) + "..";
}

void startSong() {
  const auto &notes = song.maps[diff].notes;
  game::Config cfg;
  cfg.touchCuts = diff == 0;
  session = std::make_unique<game::Session>(notes, cfg);
  bot = std::make_unique<game::Bot>(notes);
  effects.clear();
  playPos = (long)(-kLeadIn * audio::kRate);
  session->restart(-kLeadIn);
  paused = false;
  playing = true;
  platform::log("game: %s, %zu notes", kDiffNames[diff], notes.size());
}

void takeSense(const vr::Hand &h) {
  sense |= h.buttons;
  if (std::fabs(h.stick[0]) > std::fabs(senseX)) senseX = h.stick[0];
  if (std::fabs(h.stick[1]) > std::fabs(senseY)) senseY = h.stick[1];
}

// What the player asked for this frame, from either hand or the DualSense: moves and choices as presses.
struct Nav {
  bool up = false, down = false, left = false, right = false, select = false, back = false, options = false, reroll = false;
};

Nav readNav() {
  static bool pu, pd, pl, pr, ps, pb, po, pt;
  const platform::Pad pad = platform::pad();
  const float x = std::fabs(senseX) > std::fabs(pad.lx) ? senseX : pad.lx, y = std::fabs(senseY) > std::fabs(pad.ly) ? senseY : pad.ly;
  const uint32_t b = pad.buttons | sense;
  const bool u = (b & platform::BTN_UP) || y < -0.6f, d = (b & platform::BTN_DOWN) || y > 0.6f, l = (b & platform::BTN_LEFT) || x < -0.6f,
             r = (b & platform::BTN_RIGHT) || x > 0.6f, s = (b & (platform::BTN_CROSS | platform::BTN_L2 | platform::BTN_R2)) || pad.l2 > 0.5f || pad.r2 > 0.5f,
             k = (b & (platform::BTN_CIRCLE | platform::BTN_CREATE)) != 0, o = (b & platform::BTN_OPTIONS) != 0, t = (b & platform::BTN_TRIANGLE) != 0;
  Nav n;
  n.up = u && !pu, n.down = d && !pd, n.left = l && !pl, n.right = r && !pr, n.select = s && !ps, n.back = k && !pb, n.options = o && !po, n.reroll = t && !pt;
  pu = u, pd = d, pl = l, pr = r, ps = s, pb = k, po = o, pt = t;
  return n;
}

// Moves a menu cursor.
void step(int &sel, int count, const Nav &n) {
  if (n.up) sel = (sel + count - 1) % count;
  if (n.down) sel = (sel + 1) % count;
}

void loadBladeAxis() {
  if (FILE *f = std::fopen(STORAGE "blade-axis.txt", "r")) {
    int a = 0;
    if (std::fscanf(f, "%d", &a) == 1 && a >= 0 && a < 6) bladeAxis = a;
    std::fclose(f);
  }
}

void saveBladeAxis() {
  if (FILE *f = std::fopen(STORAGE "blade-axis.txt", "w")) {
    std::fprintf(f, "%d\n", bladeAxis);
    std::fclose(f);
  }
}

// ---- tracker space to the scene: about the recentre point, level, the head's rest height at 1.6 m ----
void toScene(const float p[3], float out[3]) {
  float op[3], yaw[4];
  vr::origin(op, yaw);
  const float inv[4] = {0, -yaw[1], 0, yaw[3]};
  const float t[3] = {p[0] - op[0], p[1] - op[1], p[2] - op[2]};
  rotate(inv, t, out);
  out[1] += 1.6f;
}

void quatToScene(const float q[4], float out[4]) {
  float op[3], yaw[4];
  vr::origin(op, yaw);
  const float inv[4] = {0, -yaw[1], 0, yaw[3]};
  qmul(inv, q, out);
}

// The saber a controller makes: the hilt at the controller, the blade along its chosen axis.
game::SaberPose saberFrom(const vr::Hand &h) {
  float pos[3], q[4];
  toScene(h.pos, pos);
  quatToScene(h.quat, q);
  float d[3];
  rotate(q, kAxes[bladeAxis], d);
  return {{pos[0], pos[1], pos[2]}, {pos[0] + d[0], pos[1] + d[1], pos[2] + d[2]}};
}


// ---- the screens ----
void buildTitle() {
  panel.rows.clear();
  panel.add("PULSE", 0.05f, 0);
  panel.add("RHYTHM SABERS", 0.014f, 3);
  panel.add(" ", 0.02f);
  const char *items[4] = {"PLAY", "HOW TO PLAY", "OPTIONS", "QUIT"};
  for (int i = 0; i < 4; i++) panel.add(std::string(i == titleSel ? "> " : "  ") + items[i], 0.022f, i == titleSel ? 2 : 1);
}

constexpr int kHelpPages = 2;

void buildHelp() {
  panel.rows.clear();
  panel.add(helpPage == 0 ? "HOW TO PLAY" : "SCORING AND CONTROLS", 0.026f, 0);
  panel.add(" ", 0.008f);
  auto head = [](const char *t) { panel.add(t, 0.015f, 4); };
  auto line = [](const char *t) { panel.add(t, 0.0115f, 1); };
  if (helpPage == 0) {
    panel.add("CUT EACH BLOCK WITH THE SABER OF ITS COLOUR", 0.0115f, 3);
    panel.add(" ", 0.006f);
    head("EASY: DOTS");
    line("CUT A DOT IN ANY DIRECTION, OR JUST TOUCH IT");
    head("MEDIUM: LINES");
    line("A LINE SHOWS THE WAY TO CUT, NOT WHICH END:");
    line("CUT ALONG IT, UP OR DOWN, LEFT OR RIGHT, FOR");
    line("FULL POINTS, OR ACROSS IT FOR HALF POINTS.");
    line("HIT IT AND YOUR COMBO STAYS.");
    head("HARD: ARROWS");
    line("CUT THE WAY THE ARROW POINTS. THE WRONG WAY");
    line("SCORES NOTHING AND BREAKS YOUR COMBO.");
    head("EXTREME");
    line("HARD WITH MORE BLOCKS, PAIRS AND FASTER RUNS");
  } else {
    line("A GOOD CUT SCORES UP TO 115 TIMES YOUR MULTIPLIER");
    line("FAST, WIDE SWINGS AND CUTTING THE MIDDLE SCORE MORE");
    line("MULTIPLIER: X2 AT 2 IN A ROW, X4 AT 6, X8 AT 14");
    line("A MISS OR A BAD CUT RESETS IT");
    panel.add(" ", 0.008f);
    head("CONTROLS");
    line("R3 CENTRES THE MENU     OPTIONS BUTTON PAUSES");
    line("TRIANGLE ON SONG SELECT PICKS A NEW PATTERN");
    line("OPTIONS: COLOUR-BLIND PALETTES, BACKGROUND, RESCAN");
  }
  panel.add(" ", 0.008f);
  panel.add(std::string("LEFT RIGHT PAGE ") + std::to_string(helpPage + 1) + "/" + std::to_string(kHelpPages) + "     BACK TO RETURN", 0.009f, 3);
}

void buildOptions() {
  panel.rows.clear();
  panel.preview = true;
  panel.add("OPTIONS", 0.03f, 0);
  panel.add(" ", 0.012f);
  const std::string rows[5] = {std::string("COLOURS   < ") + render::paletteName(paletteId) + " >", std::string("BACKGROUND   < ") + kBgNames[bgMode] + " >",
                               std::string("SWAP HANDS   ") + (paletteSwap ? "ON" : "OFF"), "RESCAN MUSIC", "BACK"};
  for (int i = 0; i < 5; i++) panel.add(std::string(i == optionSel ? "> " : "  ") + rows[i], 0.02f, i == optionSel ? 2 : 1);
  panel.add(" ", 0.012f);
  if (optionNote.empty()) panel.add("LEFT HAND BLOCK ON THE LEFT, RIGHT HAND ON THE RIGHT", 0.009f, 3);
  else panel.add(optionNote, 0.012f, 4);
}

void buildSelect() {
  panel.rows.clear();
  panel.add("SELECT SONG", 0.026f, 0);
  if (songs.empty()) {
    panel.add(" ", 0.012f);
    panel.add("NO SONGS FOUND", 0.018f, 5);
    panel.add("COPY MP3 FILES TO THE APP'S assets/songs FOLDER", 0.011f, 3);
    panel.add("THEN OPTIONS, RESCAN MUSIC", 0.011f, 3);
    return;
  }
  const int n = (int)songs.size();
  if (artFor != songs[songSel].id) {
    artFor = songs[songSel].id;
    std::vector<uint8_t> rgb;
    if (tags::cover(songs[songSel].path.c_str(), rgb)) render::setArt(rgb.data());
    else render::setArt(nullptr);
  }
  panel.addArt(0.8f);
  for (int k = -2; k <= 2; k++) {
    const int i = songSel + k;
    if (i < 0 || i >= n) {
      panel.add(" ", 0.014f);
      continue;
    }
    panel.add((k == 0 ? "> " : "  ") + clip(songs[i].title, 40), k == 0 ? 0.02f : 0.014f, k == 0 ? 2 : std::abs(k) == 2 ? 3 : 1);
  }
  panel.add(" ", 0.012f);
  panel.add(std::string(diff > 0 ? "< " : "  ") + kDiffLabels[diff] + (diff < 3 ? " >" : "  "), 0.022f, 4);
  const library::Entry &cur = songs[songSel];
  const int sd = seedOf(cur.id);
  if (infoKey != cur.id + std::to_string(sd)) {
    infoKey = cur.id + std::to_string(sd);
    haveInfo = library::cachedInfo(cur, STORAGE "cache", sd, info);
  }
  char line[96];
  if (haveInfo) {
    std::snprintf(line, sizeof line, "%.0f BPM   %s   %d NOTES   %.1f PER SECOND", info.bpm, clock(info.duration).c_str(), info.notes[diff], info.duration > 0 ? info.notes[diff] / info.duration : 0.0);
    panel.add(line, 0.012f, 1);
  } else {
    panel.add("BPM AND LENGTH SHOWN ONCE ANALYSED", 0.012f, 3);
  }
  std::snprintf(line, sizeof line, "PATTERN %d", sd);
  panel.add(line, 0.011f, 3);
  const scores::Best b = scores::get(scoreKey(cur.id), diff);
  if (b.plays) std::snprintf(line, sizeof line, "BEST %ld   %s   %.1f%%   COMBO %d", b.score, scores::rank(b.accuracy), (double)b.accuracy, b.combo);
  else std::snprintf(line, sizeof line, "NO SCORE YET");
  panel.add(line, 0.014f, 1);
  if (!statusLine.empty()) panel.add(statusLine, 0.012f, 5);
  panel.add(" ", 0.01f);
  panel.add("UP DOWN SONG   LEFT RIGHT LEVEL", 0.009f, 3);
  panel.add("TRIANGLE NEW PATTERN   TRIGGER PLAY", 0.009f, 3);
}

void buildPause() {
  panel.rows.clear();
  panel.add("PAUSED", 0.03f, 0);
  const char *items[4] = {"RESUME", "RESTART", "SONG SELECT", "QUIT"};
  for (int i = 0; i < 4; i++) panel.add(std::string(i == pauseSel ? "> " : "  ") + items[i], 0.018f, i == pauseSel ? 2 : 1);
}

void buildResults() {
  panel.rows.clear();
  panel.add("SONG COMPLETE", 0.03f, 0);
  panel.add(clip(songTitle, 38) + "   " + kDiffNames[diff], 0.012f, 3);
  panel.add(" ", 0.012f);
  panel.add(scores::rank(result.accuracy), 0.07f, result.accuracy >= 80 ? 4 : result.accuracy >= 50 ? 0 : 5);
  char line[96];
  std::snprintf(line, sizeof line, "SCORE %ld", result.score);
  panel.add(line, 0.022f, 2);
  std::snprintf(line, sizeof line, "ACCURACY %.1f%%   MAX COMBO %d", (double)result.accuracy, result.combo);
  panel.add(line, 0.014f, 1);
  std::snprintf(line, sizeof line, "HIT %d   MISSED %d", result.hits, result.misses);
  panel.add(line, 0.014f, 1);
  if (result.best) panel.add("NEW BEST", 0.02f, 4);
  panel.add(" ", 0.012f);
  const char *items[3] = {"PLAY AGAIN", "NEW PATTERN", "SONG SELECT"};
  for (int i = 0; i < 3; i++) panel.add(std::string(i == resultSel ? "> " : "  ") + items[i], 0.018f, i == resultSel ? 2 : 1);
}

bool loadSongEntry(const library::Entry &e) {
  // The console's heap is 256 MiB and fixed: let go of the last song before decoding the next.
  playing = false;
  session.reset();
  bot.reset();
  effects.clear();
  songLoaded = false;
  {
    std::lock_guard<std::mutex> lock(trackLock);
    std::vector<int16_t>().swap(track.pcm);
  }
  song = mapper::Song();
  platform::log("library: loading %s", clip(e.title, 60).c_str());
  audio::Track t;
  mapper::Song s;
  if (!library::load(e, STORAGE "cache", seedOf(e.id), t, s)) return false;
#ifdef PLATFORM_PS5
  platform::log("game: heap in use %zu MiB", ps5_opengl_heap_live_bytes() >> 20);
#endif
  playing = false;
  {
    std::lock_guard<std::mutex> lock(trackLock);
    track = std::move(t);
  }
  song = std::move(s);
  songLoaded = true;
  songId = scoreKey(e.id), songTitle = e.title;
  return true;
}

bool havePanel = false;
void drawFlat(double now);
void drawEyes(const vr::Frame &f, double now);
bool vrActive = false;

bool playSong(const library::Entry &e) {
  panel.rows.clear();
  panel.add("LOADING", 0.026f, 0);
  panel.add(clip(e.title, 38), 0.014f, 1);
  panel.add("ANALYSING THE SONG, A MOMENT", 0.01f, 3);
  havePanel = true;
  if (vrActive) {
    vr::Frame f;
    if (vr::begin(f)) {
      drawEyes(f, platform::seconds());
      glFinish();
      vr_eyes::fill(f.buffer);
      vr::submit();
    }
  } else {
    drawFlat(0);
    platform::swap();
  }
  if (!loadSongEntry(e)) return false;
  screen = Screen::Playing;
  startSong();
  return true;
}

void finishRun() {
  const game::Stats &st = session->stats();
  result.score = st.score, result.combo = st.maxCombo, result.hits = st.hits, result.misses = st.misses + st.badCuts;
  result.accuracy = session->maxScore() ? (float)(100.0 * st.score / session->maxScore()) : 0.0f;
  result.best = scores::record(songId, diff, result.score, result.accuracy, result.combo);
  playing = false;
  resultSel = 0;
  screen = Screen::Results;
  platform::log("game: finished %s %s: %ld, %.1f%%, rank %s%s", songTitle.c_str(), kDiffNames[diff], result.score, (double)result.accuracy, scores::rank(result.accuracy),
                result.best ? " (new best)" : "");
}

// ---- drawing ----
void refreshPanel() {
  havePanel = true;
  panel.preview = false;
  switch (screen) {
    case Screen::Title: buildTitle(); break;
    case Screen::Select: buildSelect(); break;
    case Screen::Options: buildOptions(); break;
    case Screen::Help: buildHelp(); break;
    case Screen::Results: buildResults(); break;
    case Screen::Playing:
      havePanel = paused;
      if (paused) buildPause();
      break;
  }
}

render::Scene sceneAt(double now) {
  render::Scene s;
  const bool inSong = screen == Screen::Playing || screen == Screen::Results;
  s.now = now, s.diff = diff, s.sabers = sabers, s.effects = &effects, s.clock = platform::seconds();
  if (inSong && songLoaded) s.song = &song, s.downbeatPhase = song.downbeatPhase;
  if (screen == Screen::Playing) s.session = session.get();
  s.panel = havePanel ? &panel : nullptr;
  return s;
}

void drawFlat(double now) {
  render::Scene sc = sceneAt(now);
  sc.maxZ = 1e9f;
  sc.bg = bgMode ? 2 : 0, sc.bg3d = bgMode == 2, sc.hdr = true;
  // A menu fills the window: move in on it and leave the sabers out of the way of the text.
  if (havePanel) sc.sabers = nullptr;
  float bg[3];
  render::background(sc, bg);
  const int w = platform::width(), h = platform::height();
  render::postBegin(w, h);
  glClearColor(bg[0], bg[1], bg[2], 1);
  glEnable(GL_DEPTH_TEST);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  const float a = (float)w / h, tan[4] = {0.7f * a, 0.7f * a, 0.7f, 0.7f};
  float ez = 1.9f;
  if (havePanel) {
    float pw, ph;
    render::panelSize(panel, pw, ph);
    ez = -1.6f + std::max(pw * 0.46f, ph * 0.82f);
  }
  const float eye[3] = {0, havePanel ? 1.5f : 1.7f, ez}, q[4] = {0, 0, 0, 1};
  render::draw(frustum(tan, 0.05f, 120.0f) * eyeView(eye, q), sc, bg);
  render::postEnd();
}

void drawEyes(const vr::Frame &f, double now) {
  const int eye = vr_eyes::eye();
  render::Scene sc = sceneAt(now);
  sc.bg = bgMode ? 1 : 0, sc.bg3d = bgMode == 2;
  float bg[3];
  render::background(sc, bg);
  glBindFramebuffer(GL_FRAMEBUFFER, vr_eyes::framebuffer());
  glEnable(GL_DEPTH_TEST);
  glViewport(0, 0, eye * 2, eye);
  glClearColor(bg[0], bg[1], bg[2], 1);
  glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  for (int e = 0; e < 2; e++) {
    float p[3], q[4];
    toScene(f.eye[e].pos, p);
    quatToScene(f.eye[e].quat, q);
    glViewport(e * eye, 0, eye, eye);
    render::draw(frustum(f.eye[e].tan, 0.05f, 150.0f) * eyeView(p, q), sc, bg);
    static const double started = platform::seconds();
    if (!userCentred && screen != Screen::Playing && platform::seconds() - started < 40) {
      float p0[3], p1[3], hq[4];
      toScene(f.eye[0].pos, p0);
      toScene(f.eye[1].pos, p1);
      quatToScene(f.eye[0].quat, hq);
      const float mid[3] = {(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2};
      render::hint(frustum(f.eye[e].tan, 0.05f, 150.0f) * eyeView(p, q), mid, hq, "PRESS R3 TO CENTRE THE MENU", "LOOK STRAIGHT AHEAD FIRST");
    }
  }
}

bool writePpm(const char *path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
  std::fclose(f);
  return true;
}

void report(double now) {
  const game::Stats &st = session->stats();
  platform::log("game: %.1f s  score %ld  combo %d (best %d) x%d  energy %.0f%%  hit %d  miss %d  bad %d  %.1f%% of max", now, st.score, st.combo, st.maxCombo,
                st.multiplier, st.energy * 100, st.hits, st.misses, st.badCuts, session->maxScore() ? 100.0 * st.score / session->maxScore() : 0.0);
}


}  // namespace

int main(int argc, char **argv) {
  bool wantVr = false, mouse = false, forceBot = false;
  const char *mp3Path = nullptr, *mapPath = nullptr, *shotPath = nullptr, *videoPath = nullptr, *screenName = nullptr;
  double shotAt = 0;
  int shotPalette = -1;
  std::vector<std::string> dirs;
#ifdef PLATFORM_PS5
  wantVr = true;
  dirs = {"/mnt/usb0/songs", "/mnt/usb1/songs", "/mnt/usb2/songs", "/mnt/usb3/songs", "/mnt/ext0/songs", "/mnt/ext1/songs", "/data/songs", "/download0/songs", "/app0/assets/songs"};
  (void)argc, (void)argv;
#else
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--diff") && i + 1 < argc) diff = argv[++i][0] == 'e' ? (argv[i][1] == 'x' ? 3 : 0) : argv[i][0] == 'h' ? 2 : 1;
    else if (!std::strcmp(argv[i], "--songs") && i + 1 < argc) dirs.push_back(argv[++i]);
    else if (!std::strcmp(argv[i], "--screen") && i + 1 < argc) screenName = argv[++i];
    else if (!std::strcmp(argv[i], "--palette") && i + 1 < argc) shotPalette = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--vr")) wantVr = true;
    else if (!std::strcmp(argv[i], "--mouse")) mouse = true;
    else if (!std::strcmp(argv[i], "--bot")) forceBot = true;
    else if (!std::strcmp(argv[i], "--video") && i + 1 < argc) videoPath = argv[++i];
    else if (!std::strcmp(argv[i], "--shot") && i + 2 < argc) shotAt = std::atof(argv[i + 1]), shotPath = argv[i + 2], i += 2;
    else if (!mp3Path) mp3Path = argv[i];
    else mapPath = argv[i];
  }
  if (dirs.empty()) {
    dirs.push_back("songs");
    if (const char *home = std::getenv("HOME")) dirs.push_back(std::string(home) + "/Music");
  }
#endif
  if (!platform::init("Pulse", 1280, 720, false)) return 1;
  render::init();
  loadBladeAxis();
  loadSettings();
  if (shotPalette >= 0) paletteId = shotPalette, render::setPalette(paletteId, paletteSwap);
  scores::init(shotPath || videoPath ? STORAGE "scores-test.txt" : STORAGE "scores.txt");
  songs = library::scan(dirs);
  platform::log("game: %zu songs found", songs.size());

  bool vrOn = false;
  if (wantVr && vr::init(platform::user()) && vr_eyes::init()) {
    static unsigned depth;
    const int e = vr_eyes::eye();
    glCreateRenderbuffers(1, &depth);
    glNamedRenderbufferStorage(depth, GL_DEPTH_COMPONENT32F, e * 2, e);
    glNamedFramebufferRenderbuffer(vr_eyes::framebuffer(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    vrOn = vr::start(vr_eyes::targets());
    vrActive = vrOn;
  }
  platform::log("game: headset %s, blade along %s", vrOn ? "on" : vr::status(), kAxisNames[bladeAxis]);
  platform::notify(vrOn ? "Pulse: stick to move, trigger to choose" : "Pulse: no headset, TV view");
  autoplay = forceBot || (!vrOn && !mouse);

  // A song named on the command line plays straight away, with its map as given: for tests and videos.
  if (mp3Path && mapPath) {
    if (!mapper::read(mapPath, song) || song.maps.size() < 4 || !audio::decodeMp3(mp3Path, track)) {
      platform::log("game: cannot load %s / %s (failed)", mp3Path, mapPath);
      return 1;
    }
    songLoaded = true, songId = "direct", songTitle = mp3Path;
    screen = Screen::Playing;
    startSong();
  } else if (screenName && !std::strcmp(screenName, "select")) {
    screen = Screen::Select;
  } else if (screenName && !std::strncmp(screenName, "help", 4)) {
    screen = Screen::Help, helpPage = screenName[4] == '2' ? 1 : 0;
  } else if (screenName && !std::strcmp(screenName, "options")) {
    screen = Screen::Options;
  } else if (screenName && !std::strcmp(screenName, "results")) {
    if (songs.empty() || !loadSongEntry(songs[0])) return 1;
    screen = Screen::Playing;
    startSong();
  }

#ifndef PLATFORM_PS5
  if (videoPath) {
    if (screen != Screen::Playing) return 2;
    platform::poll();
    autoplay = true;
    const int w = platform::width(), h = platform::height();
    char cmd[2048];
    std::snprintf(cmd, sizeof cmd,
                  "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 60 -i - -itsoffset %g -i '%s' -vf 'vflip,crop=trunc(iw/2)*2:trunc(ih/2)*2' -c:v libx264 -preset fast -crf 18 "
                  "-pix_fmt yuv420p -c:a aac -b:a 192k -shortest '%s'",
                  w, h, kLeadIn, mp3Path ? mp3Path : songs[0].path.c_str(), videoPath);
    FILE *ff = popen(cmd, "w");
    if (!ff) return 1;
    std::vector<unsigned char> px((size_t)w * h * 3);
    const double end = track.frames() / (double)audio::kRate;
    double sim = -kLeadIn;
    for (double t = -kLeadIn; t < end; t += 1.0 / 60) {
      for (; sim <= t; sim += 1.0 / 120) {
        for (int c = 0; c < 2; c++) sabers[c] = bot->pose(c, sim);
        session->update(sim, sabers);
        for (const game::Event &e : session->takeEvents())
          if (e.kind == game::Event::Hit) effects.push_back({e.pos, e.color, sim});
      }
      refreshPanel();
      drawFlat(t);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
      std::fwrite(px.data(), 1, px.size(), ff);
      if ((int)(t * 60) % 600 == 0) report(t);
    }
    report(end);
    return pclose(ff) == 0 ? 0 : 1;
  }
#endif
  if (shotPath) {
    // Play on the bot up to the moment (or through to the results) and write one frame, to look at it without a screen.
    platform::poll();
    autoplay = true;
    double at = shotAt;
    if (screen == Screen::Playing) {
      const bool toEnd = screenName && !std::strcmp(screenName, "results");
      const double stop = toEnd ? track.frames() / (double)audio::kRate + 1.5 : shotAt;
      for (double t = std::max(-kLeadIn, toEnd ? -kLeadIn : shotAt - 8); t <= stop; t += 1.0 / 90) {
        for (int c = 0; c < 2; c++) sabers[c] = bot->pose(c, t);
        session->update(t, sabers);
        for (const game::Event &e : session->takeEvents())
          if (e.kind == game::Event::Hit) effects.push_back({e.pos, e.color, t});
      }
      if (toEnd) finishRun(), at = stop;
      else report(shotAt);
    }
    refreshPanel();
    drawFlat(at);
    return writePpm(shotPath, platform::width(), platform::height()) ? 0 : 1;
  }
  if (!platform::startAudio(fill, audio::kRate)) platform::log("game: no audio");

  uint32_t last = 0;
  double lastNow = -1e9, nextReport = 0;
  while (platform::poll() && !quitRequested) {
    const double now = playPos / (double)audio::kRate;
    vr::Frame f;
    const bool frame = vrOn && vr::begin(f);
    const bool inSong = screen == Screen::Playing;
    // The bot's hands (the simulated headset follows them, so the hand-to-saber path is the one tested).
    if (inSong && autoplay)
      for (int c = 0; c < 2; c++) {
        sabers[c] = bot->pose(c, now);
#ifndef PLATFORM_PS5
        const float pos[3] = {sabers[c].base.x, sabers[c].base.y, sabers[c].base.z}, q[4] = {0, 0, 0, 1};
        vr::simulateHand(c, pos, q);
#endif
      }
    sense = 0, senseX = senseY = 0;
    vr::Hand hands[2];
    bool tracked[2] = {false, false};
    if (frame)
      for (int c = 0; c < 2; c++) {
        vr::hand(c, hands[c]);
        takeSense(hands[c]);
        tracked[c] = hands[c].tracked;
      }
    for (int c = 0; c < 2; c++) {
      if (!frame || !tracked[c]) continue;
#ifdef PLATFORM_PS5
      if (!(inSong && autoplay)) sabers[c] = saberFrom(hands[c]);
#else
      sabers[c] = saberFrom(hands[c]);
#endif
    }
    if (mouse && !frame) {
      float mx, my;
      int buttons;
      if (platform::mouse(mx, my, buttons)) {
        const float aspect = (float)platform::width() / platform::height();
        const float x = mx * 0.7f * aspect * 2.25f, y = 1.7f + my * 0.7f * 2.25f;
        sabers[buttons ? 0 : 1] = {{x, y, -0.35f}, {x, y, -1.35f}};
      }
    }

    const Nav nav = readNav();
    const uint32_t padButtons = platform::pad().buttons, held = padButtons | sense;
    const uint32_t edge = held & ~last;
    last = held;
    if (edge & (platform::BTN_R3 | platform::BTN_L3)) vr::recenter(), userCentred = true;

    switch (screen) {
      case Screen::Title:
        step(titleSel, 4, nav);
        if (nav.select) {
          if (titleSel == 0) screen = Screen::Select;
          else if (titleSel == 1) screen = Screen::Help, helpPage = 0;
          else if (titleSel == 2) screen = Screen::Options, optionSel = 0, optionNote.clear();
          else quitRequested = true;
        }
        break;
      case Screen::Help:
        if (nav.left) helpPage = std::max(0, helpPage - 1);
        if (nav.right || nav.select) helpPage = std::min(kHelpPages - 1, helpPage + 1);
        if (nav.back || (nav.select && helpPage == kHelpPages - 1 && !nav.right)) screen = Screen::Title;
        break;
      case Screen::Options: {
        step(optionSel, 5, nav);
        const int dir = (nav.right ? 1 : 0) - (nav.left ? 1 : 0);
        if (optionSel == 0 && (dir || nav.select)) {
          paletteId = (paletteId + (dir ? dir : 1) + render::paletteCount()) % render::paletteCount();
          saveSettings();
        } else if (optionSel == 1 && (dir || nav.select)) {
          bgMode = (bgMode + (dir ? dir : 1) + 3) % 3;
          saveSettings();
        } else if (optionSel == 2 && (dir || nav.select)) {
          paletteSwap = !paletteSwap;
          saveSettings();
        } else if (optionSel == 3 && nav.select) {
          const std::string keep = songs.empty() ? "" : songs[songSel].id;
          songs = library::scan(dirs);
          songSel = 0;
          for (size_t i = 0; i < songs.size(); i++)
            if (songs[i].id == keep) songSel = (int)i;
          artFor.clear();
          optionNote = songs.empty() ? "NO SONGS FOUND" : "FOUND " + std::to_string(songs.size()) + " SONGS";
          platform::log("game: rescan, %zu songs", songs.size());
        } else if (optionSel == 4 && nav.select) {
          screen = Screen::Title;
        }
        if (nav.back) screen = Screen::Title;
        break;
      }
      case Screen::Select:
        if (nav.back) screen = Screen::Title;
        if (songs.empty()) break;
        step(songSel, (int)songs.size(), nav);
        if (nav.left) diff = std::max(0, diff - 1);
        if (nav.right) diff = std::min(3, diff + 1);
        if (nav.reroll) {
          int &sd = seeds[songs[songSel].id];
          sd = sd < 1 ? 2 : sd % 99 + 1;
          saveSeeds();
        }
        if (nav.select) {
          statusLine.clear();
          if (!playSong(songs[songSel])) statusLine = "COULD NOT READ THAT SONG";
        }
        break;
      case Screen::Playing: {
        if (nav.options) paused = !paused, pauseSel = 0, playing = !paused;
        if (paused) {
          step(pauseSel, 4, nav);
          if (nav.back) paused = false, playing = true;
          if (nav.select) {
            if (pauseSel == 0) paused = false, playing = true;
            else if (pauseSel == 1) startSong();
            else if (pauseSel == 2) paused = false, playing = false, screen = Screen::Select;
            else quitRequested = true;
          }
          break;
        }
        if (padButtons & ~last & platform::BTN_CIRCLE) autoplay = !autoplay, platform::log("game: autoplay %s", autoplay ? "on" : "off");
        if (edge & platform::BTN_SQUARE) {
          bladeAxis = (bladeAxis + 1) % 6;
          saveBladeAxis();
          platform::log("game: blade along controller %s", kAxisNames[bladeAxis]);
        }
        if (now != lastNow) {
          session->update(now, sabers);
          for (const game::Event &e : session->takeEvents()) {
            if (e.kind == game::Event::Hit) effects.push_back({e.pos, e.color, now}), click(now, e.color), vr::haptic(e.color, 0.6f);
            else if (e.kind == game::Event::BadCut) effects.push_back({e.pos, 2, now}), vr::haptic(e.color, 0.3f);
          }
          if (effects.size() > 200) effects.erase(effects.begin(), effects.begin() + 100);
        }
        if (platform::seconds() >= nextReport) {
          nextReport = platform::seconds() + 5;
          if (now > 0) report(now);
        }
        if (now > song.duration + 1) finishRun();
        break;
      }
      case Screen::Results:
        step(resultSel, 3, nav);
        if (nav.back) screen = Screen::Select;
        if (nav.select) {
          if (resultSel == 0) screen = Screen::Playing, startSong();
          else if (resultSel == 1) {
            int &sd = seeds[songs[songSel].id];
            sd = sd < 1 ? 2 : sd % 99 + 1;
            saveSeeds();
            if (!playSong(songs[songSel])) screen = Screen::Select, statusLine = "COULD NOT READ THAT SONG";
          } else screen = Screen::Select;
        }
        break;
    }
    lastNow = now;

    refreshPanel();
    if (frame) {
      drawEyes(f, now);
      glFinish();
      vr_eyes::fill(f.buffer);
      vr::submit();
    }
#ifdef PLATFORM_PS5
    if (vrOn) {
      vr::waitSlot();
      continue;
    }
#endif
    drawFlat(now);
    char title[200];
    if (screen == Screen::Playing) {
      const game::Stats &st = session->stats();
      std::snprintf(title, sizeof title, "Pulse | %s | %s | score %ld  combo %d x%d  miss %d bad %d | %s", clip(songTitle, 30).c_str(), kDiffNames[diff], st.score, st.combo,
                    st.multiplier, st.misses, st.badCuts, autoplay ? "AUTOPLAY" : "playing");
    } else {
      std::snprintf(title, sizeof title, "Pulse");
    }
    platform::setTitle(title);
    platform::swap();
  }
  if (vrOn) vr::stop();
  platform::shutdown();
  return 0;
}
