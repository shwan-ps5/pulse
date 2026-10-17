// pulse-map song.mp3 [-o dir] [--seed N]: analyses a song and writes map.pulse (easy/medium/hard) and a
// debug.svg of what the analysis heard. Decoding goes through ffmpeg here; the game will decode on
// its own later.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "analysis.h"
#include "mapper.h"

namespace {

// ffmpeg pipe -> mono float at analysis::kSampleRate (desktop only).
bool decode(const std::string &path, std::vector<float> &out) {
  std::string cmd = "ffmpeg -v error -i '";
  for (char c : path) cmd += c == '\'' ? std::string("'\\''") : std::string(1, c);
  cmd += "' -vn -ac 1 -ar " + std::to_string(analysis::kSampleRate) + " -f f32le -";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) return false;
  float buf[4096];
  size_t n;
  while ((n = fread(buf, sizeof(float), 4096, p)) > 0) out.insert(out.end(), buf, buf + n);
  return pclose(p) == 0 && !out.empty();
}

const char *kSectionColour[] = {"#3a4a6a", "#2a2f3a", "#b8860b", "#c0392b", "#1e6b52", "#4a3a6a"};

// debug plot of onset curve, beats, sections and notes per level (open in a browser).
void svg(const char *path, const analysis::Result &r, const mapper::Song &s) {
  FILE *f = std::fopen(path, "w");
  if (!f) return;
  const double W = 1800, rowH = 90, T = r.duration, x0 = 40;
  auto X = [&](double t) { return x0 + (W - x0) * t / T; };
  std::fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='640' style='background:#111;font:11px sans-serif'>\n", (int)W + 20);
  for (const auto &b : r.bars)
    std::fprintf(f, "<rect x='%.1f' y='20' width='%.1f' height='580' fill='%s' opacity='0.55'/>\n", X(b.start), X(b.end) - X(b.start),
                 kSectionColour[(int)b.section]);
  std::fprintf(f, "<text x='%.0f' y='14' fill='#ccc'>%.1f BPM (confidence %.2f), %zu beats, bars coloured: intro/steady/build(amber)/drop(red)/break(green)/outro</text>\n",
               x0, r.bpm, r.beatConfidence, r.beats.size());
  // Onset curve.
  std::fprintf(f, "<polyline fill='none' stroke='#6cf' stroke-width='1' points='");
  float top = 1e-6f;
  for (float v : r.onset) top = std::max(top, v);
  for (size_t i = 0; i < r.onset.size(); i += 2)
    std::fprintf(f, "%.1f,%.1f ", X(i / analysis::kFps), 20 + rowH - rowH * r.onset[i] / top * 0.9);
  std::fprintf(f, "'/>\n");
  // Intensity per bar.
  std::fprintf(f, "<polyline fill='none' stroke='#fff' stroke-width='2' points='");
  for (const auto &b : r.bars) std::fprintf(f, "%.1f,%.1f %.1f,%.1f ", X(b.start), 20 + 2 * rowH - rowH * b.intensity, X(b.end), 20 + 2 * rowH - rowH * b.intensity);
  std::fprintf(f, "'/>\n");
  for (const auto &b : r.bars)
    std::fprintf(f, "<text x='%.1f' y='%.0f' fill='#ddd'>%s</text>\n", X(b.start) + 2, 20 + 2 * rowH + 12 + (int)(&b - &r.bars[0]) % 3 * 11, analysis::name(b.section));
  for (double b : r.beats) std::fprintf(f, "<line x1='%.1f' x2='%.1f' y1='20' y2='34' stroke='#fff'/>\n", X(b), X(b));
  for (size_t d = 0; d < s.maps.size(); d++) {
    const double y = 20 + 2 * rowH + 50 + d * 130;
    std::fprintf(f, "<text x='2' y='%.0f' fill='#ccc'>%s %zu</text>\n", y - 4, mapper::name(s.maps[d].difficulty), s.maps[d].notes.size());
    for (const auto &n : s.maps[d].notes)
      std::fprintf(f, "<circle cx='%.1f' cy='%.1f' r='2.2' fill='%s'/>\n", X(n.time), y + (2 - n.row) * 14 + 8, n.color ? "#4a8cff" : "#ff4a5a");
  }
  std::fprintf(f, "</svg>\n");
  std::fclose(f);
}

}  // namespace

// pulse-map song.mp3 [-o dir] [--seed N]: analyse + generate, print sections and per-level counts + validate() problems, write
// .pulse files and the svg. CI-style check that the mapper still produces valid maps.
int main(int argc, char **argv) {
  std::string in, out = ".";
  unsigned seed = 1;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::atoi(argv[++i]);
    else in = argv[i];
  }
  if (in.empty()) {
    std::fprintf(stderr, "usage: pulse-map song.mp3 [-o dir] [--seed N]\n");
    return 2;
  }
  std::vector<float> pcm;
  if (!decode(in, pcm)) {
    std::fprintf(stderr, "could not decode %s (needs ffmpeg)\n", in.c_str());
    return 1;
  }
  const analysis::Result r = analysis::analyse(pcm);
  if (r.beats.size() < 8 || r.bars.size() < 4) {
    std::fprintf(stderr, "no beat found\n");
    return 1;
  }
  const mapper::Song song = mapper::generate(r, seed);
  std::string cmd = "mkdir -p '" + out + "'";
  if (std::system(cmd.c_str()) != 0) return 1;
  mapper::write(song, (out + "/map.pulse").c_str());
  svg((out + "/debug.svg").c_str(), r, song);

  std::printf("%.1f s, %.2f BPM (confidence %.2f), %zu beats, %zu bars, downbeat phase %d\n", r.duration, r.bpm, r.beatConfidence,
              r.beats.size(), r.bars.size(), r.downbeatPhase);
  for (size_t i = 0; i < r.bars.size();) {
    size_t j = i;
    while (j + 1 < r.bars.size() && r.bars[j + 1].section == r.bars[i].section) j++;
    std::printf("  %6.1f - %6.1f s  %-7s %zu bars\n", r.bars[i].start, r.bars[j].end, analysis::name(r.bars[i].section), j - i + 1);
    i = j + 1;
  }
  for (const auto &m : song.maps) {
    const auto bad = mapper::validate(m);
    std::printf("%-6s %4zu notes (%.2f/s), %zu problems\n", mapper::name(m.difficulty), m.notes.size(), m.notes.size() / r.duration, bad.size());
    for (size_t i = 0; i < bad.size() && i < 5; i++) std::printf("    %s\n", bad[i].c_str());
  }
  return 0;
}
