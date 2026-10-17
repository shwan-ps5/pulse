// pulse-view song.mp3 [--seed N] [--diff easy|medium|hard]: analyses a song, generates its maps and
// plays it with the boxes flying at you, to judge the patterns. Desktop only.
//   Space pause   Left/Right seek 5 s (shift: 20 s)   1/2/3 easy/medium/hard   N/B next/previous seed
//   +/- note speed   C hit clicks on/off   P autoplay bot / mouse   Esc quit
// With autoplay off the mouse is the right (blue) saber; hold a mouse button to move the left (red) one.
#include <SDL_keycode.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <memory>

#include "analysis.h"
#include "bot.h"
#include "game.h"
#include "gl.h"
#include "mapper.h"
#include "platform.h"
#include "render.h"
#include "vmath.h"

namespace {

constexpr int kRate = 48000;

// POSIX single-quote escaping for a path passed to ffmpeg via popen.
std::string shellQuote(const std::string &s) {
  std::string o = "'";
  for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return o + "'";
}

// decodes any audio file to float PCM through an ffmpeg pipe (desktop dev tool: not the console's decoder).
bool decode(const std::string &path, int channels, int rate, std::vector<float> &out) {
  const std::string cmd = "ffmpeg -v error -i " + shellQuote(path) + " -vn -ac " + std::to_string(channels) + " -ar " + std::to_string(rate) + " -f f32le -";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) return false;
  float buf[8192];
  size_t n;
  while ((n = fread(buf, sizeof(float), 8192, p)) > 0) out.insert(out.end(), buf, buf + n);
  return pclose(p) == 0 && !out.empty();
}

// ---- the song and its playback (the audio thread reads these) ----
std::vector<float> pcm;  // interleaved stereo at kRate
std::atomic<long> playPos{0};
std::atomic<bool> playing{true}, clicks{true};
std::mutex mapLock;
std::vector<mapper::Note> clickNotes;
size_t nextClick = 0;
struct Voice {
  long start;
  float freq;
};
std::vector<Voice> voices;

// audio callback: plays the decoded song at playPos plus hit clicks (same idea as game/main.cpp fill).
void fill(float *out, int frames) {
  std::unique_lock<std::mutex> lock(mapLock, std::try_to_lock);
  std::memset(out, 0, sizeof(float) * 2 * frames);
  if (!lock.owns_lock() || !playing) return;
  const long pos = playPos;
  for (int i = 0; i < frames; i++) {
    const long p = pos + i;
    if (p >= 0 && (size_t)p * 2 + 1 < pcm.size()) out[2 * i] = pcm[2 * p] * 0.8f, out[2 * i + 1] = pcm[2 * p + 1] * 0.8f;
  }
  if (clicks) {
    while (nextClick < clickNotes.size() && clickNotes[nextClick].time * kRate < pos + frames) {
      voices.push_back({(long)(clickNotes[nextClick].time * kRate), clickNotes[nextClick].color ? 1300.0f : 1900.0f});
      nextClick++;
    }
    for (size_t v = 0; v < voices.size();) {
      const Voice &c = voices[v];
      bool alive = false;
      for (int i = 0; i < frames; i++) {
        const long age = pos + i - c.start;
        if (age < 0 || age > 600) {
          alive = alive || age < 0;
          continue;
        }
        alive = true;
        const float s = 0.25f * std::sin(6.2831853f * c.freq * age / kRate) * std::exp(-age / 120.0f);
        out[2 * i] += s, out[2 * i + 1] += s;
      }
      if (!alive) voices.erase(voices.begin() + v);
      else v++;
    }
  }
  playPos = pos + frames;
}

// forget clicks scheduled after time t (after a seek).
void resetClicks(double t) {
  std::lock_guard<std::mutex> lock(mapLock);
  voices.clear();
  nextClick = std::lower_bound(clickNotes.begin(), clickNotes.end(), t, [](const mapper::Note &n, double v) { return n.time < v; }) - clickNotes.begin();
}

// ---- the app ----
analysis::Result result;
mapper::Song song;
std::string title;
unsigned seed = 1;
int diff = 1;
float speed = 14;  // metres per second
constexpr float kEyeY = 1.7f;

std::unique_ptr<game::Session> session;
std::unique_ptr<game::Bot> bot;
bool autoplay = true;
game::SaberPose sabers[2] = {{{-0.25f, 1.0f, -0.35f}, {-0.25f, 1.0f, -1.35f}}, {{0.25f, 1.0f, -0.35f}, {0.25f, 1.0f, -1.35f}}};

std::vector<render::Fx> effects;

// new game::Session for the current map/seed, restarted at `now`.
void newSession(double now) {
  session = std::make_unique<game::Session>(song.maps[diff].notes);
  session->restart(now);
  bot = std::make_unique<game::Bot>(song.maps[diff].notes);
  effects.clear();
}

// re-run mapper::generate with the current seed (N/B keys) and restart.
void regenerate() {
  std::lock_guard<std::mutex> lock(mapLock);
  song = mapper::generate(result, seed);
  clickNotes = song.maps[diff].notes;
  std::sort(clickNotes.begin(), clickNotes.end(), [](const mapper::Note &a, const mapper::Note &b) { return a.time < b.time; });
  nextClick = 0;
  voices.clear();
}

// one frame of the viewer (flat camera, no bloom).
void drawFrame(double now, int w, int h) {
  render::Scene sc;
  sc.now = now, sc.song = &song, sc.diff = diff, sc.session = session.get(), sc.sabers = sabers, sc.effects = &effects, sc.speed = speed;
  sc.downbeatPhase = result.downbeatPhase, sc.maxZ = 1e9f;
  float bg[3];
  render::background(sc, bg);
  glViewport(0, 0, w, h);
  glClearColor(bg[0], bg[1], bg[2], 1);
  glEnable(GL_DEPTH_TEST);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  const float a = (float)w / h, tan[4] = {0.7f * a, 0.7f * a, 0.7f, 0.7f};
  const float eye[3] = {0, kEyeY, 1.9f}, q[4] = {0, 0, 0, 1};
  render::draw(frustum(tan, 0.05f, 120.0f) * eyeView(eye, q), sc, bg);
}

}  // namespace

// dev tool entry: analyse -> generate -> play with a bot or mouse sabers to judge patterns. not part of the shipped game.
int main(int argc, char **argv) {
  std::string in, shotPath;
  double shotAt = 0;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shot") && i + 2 < argc) shotAt = std::atof(argv[i + 1]), shotPath = argv[i + 2], i += 2;
    else if (!std::strcmp(argv[i], "--diff") && i + 1 < argc) {
      const char *d = argv[++i];
      diff = d[0] == 'e' ? (d[1] == 'x' ? 3 : 0) : d[0] == 'h' ? 2 : 1;
    } else in = argv[i];
  }
  if (in.empty()) {
    std::fprintf(stderr, "usage: pulse-view song.mp3 [--seed N] [--diff easy|medium|hard]\n");
    return 2;
  }
  std::vector<float> mono;
  if (!decode(in, 2, kRate, pcm) || !decode(in, 1, analysis::kSampleRate, mono)) {
    std::fprintf(stderr, "could not decode %s (needs ffmpeg)\n", in.c_str());
    return 1;
  }
  std::fprintf(stderr, "analysing...\n");
  result = analysis::analyse(mono);
  if (result.beats.size() < 8 || result.bars.size() < 4) {
    std::fprintf(stderr, "no beat found\n");
    return 1;
  }
  regenerate();
  newSession(0);
  title = in.substr(in.find_last_of('/') + 1);

  if (!platform::init("Pulse view", 1600, 900, false)) return 1;
  render::init();
  playPos = (long)(-1.5 * kRate);  // a moment before the song, so the first boxes are seen coming
  if (!shotPath.empty()) {
    // One frame to a PPM and out, for looking at the view without a screen to watch.
    platform::poll();
    const int w = platform::width(), h = platform::height();
    for (double t = std::max(0.0, shotAt - 8); t <= shotAt; t += 1.0 / 90) {  // play up to the moment
      for (int c = 0; c < 2; c++) sabers[c] = bot->pose(c, t);
      session->update(t, sabers);
      for (const game::Event &e : session->takeEvents())
        if (e.kind == game::Event::Hit) effects.push_back({e.pos, e.color, t});
    }
    drawFrame(shotAt, w, h);
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    FILE *f = std::fopen(shotPath.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    std::fclose(f);
    return 0;
  }
  if (!platform::startAudio(fill, kRate)) std::fprintf(stderr, "no audio\n");

  const char *names[4] = {"easy", "medium", "hard", "extreme"};
  double nextTitle = 0;
  while (platform::poll()) {
    for (int k; (k = platform::nextKey()) != 0;) {
      const bool shift = k & 0x20000000;
      k &= ~0x20000000;
      double t = playPos / (double)kRate;
      bool seek = false;
      if (k == ' ') playing = !playing;
      else if (k == SDLK_LEFT) t -= shift ? 20 : 5, seek = true;
      else if (k == SDLK_RIGHT) t += shift ? 20 : 5, seek = true;
      else if (k >= '1' && k <= '4') diff = k - '1', regenerate(), seek = true;
      else if (k == 'n') seed++, regenerate(), seek = true;
      else if (k == 'b' && seed > 0) seed--, regenerate(), seek = true;
      else if (k == '=' || k == '+') speed = std::min(30.0f, speed + 2);
      else if (k == '-') speed = std::max(6.0f, speed - 2);
      else if (k == 'c') clicks = !clicks;
      else if (k == 'p') autoplay = !autoplay;
      if (seek) {
        t = std::clamp(t, -1.5, song.duration);
        playPos = (long)(t * kRate);
        resetClicks(t);
        newSession(t);
      }
    }
    const double now = playPos / (double)kRate;
    {
      // Where the sabers are this frame.
      static double lastNow = -1e9;
      if (autoplay) {
        for (int c = 0; c < 2; c++) sabers[c] = bot->pose(c, now);
      } else {
        float mx, my;
        int buttons;
        if (platform::mouse(mx, my, buttons)) {
          const float aspect = (float)platform::width() / platform::height();
          const float x = mx * 0.7f * aspect * 2.25f, y = kEyeY + my * 0.7f * 2.25f;
          sabers[buttons ? 0 : 1] = {{x, y, -0.35f}, {x, y, -1.35f}};
        }
      }
      if (playing && now != lastNow) {
        session->update(now, sabers);
        for (const game::Event &e : session->takeEvents()) {
          if (e.kind == game::Event::Hit) effects.push_back({e.pos, e.color, now});
          else if (e.kind == game::Event::BadCut) effects.push_back({e.pos, 2, now});
        }
        if (effects.size() > 200) effects.erase(effects.begin(), effects.begin() + 100);
      }
      lastNow = now;
    }
    if (platform::seconds() >= nextTitle) {
      nextTitle = platform::seconds() + 0.2;
      const analysis::Bar *bar = nullptr;
      for (const auto &b : song.bars)
        if (now >= b.start && now < b.end) bar = &b;
      char buf[320];
      const game::Stats &st = session->stats();
      std::snprintf(buf, sizeof buf, "%s | %s %zu notes seed %u | %.0f BPM %4.1f/%.0f s %s | score %ld  combo %d x%d  energy %.0f%%  acc %.1f%%  miss %d bad %d | %s %s", title.c_str(),
                    names[diff], song.maps[diff].notes.size(), seed, song.bpm, now, song.duration, bar ? analysis::name(bar->section) : "-", st.score, st.combo,
                    st.multiplier, st.energy * 100, st.maxScoreSoFar ? 100.0 * st.score / st.maxScoreSoFar : 100.0, st.misses, st.badCuts,
                    autoplay ? "AUTOPLAY" : "mouse", playing ? "" : "paused");
      platform::setTitle(buf);
    }
    drawFrame(now, platform::width(), platform::height());
    platform::swap();
  }
  platform::shutdown();
  return 0;
}
