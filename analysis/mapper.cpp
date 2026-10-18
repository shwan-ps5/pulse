// analysis::Result -> 4 note maps (Easy..Extreme) + per-beat visual cues; .pulse text file read/write; map validator.
// deterministic: (Result, seed) -> identical Song. seed per song stored in STORAGE seeds.txt (game/main.cpp), Triangle rerolls.
// build() stages: (1) slot grid = each beat split p.subdivision ways, onset peaks snapped to nearest slot (score=strength,
//   bass/mid/high at that frame); (2) per bar pick top-scoring slots, count = notesPerBeat*4*section factor (intro .5,
//   break .15, outro .4, build ramps to 1+buildBoost, drop 1.25, else .7+.6*intensity), spaced >= minGap beats;
//   (3) place notes: alternate hands, alternate down/up swing per hand, row from bass/high ratio, lane near hand's side,
//   `fast` (<0.45 s since hand's last) limits movement to 1 lane/row, `blocked()` avoids the other hand's follow-through path,
//   sideways swings (p.sideways) leave lastDown unchanged; drops open with a two-hand pair; Extreme adds same-time pairs;
//   (4) styled(): remap dir codes by level. invariants (checked by validate()): time-ordered, hand gap >= 0.19 s,
//   <= 1 lane per 0.1 s, no two notes in one cell at one time.
//   (2b) gap fill: a gap over 4 s (Easy; 3.5/3/2.5 s above) between chosen slots gets a beat-aligned block about every second.
// !: any change to stage logic or Profile values changes output => bump library::kMapperVersion (invalidates caches).
#include "mapper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mapper {

using analysis::Section;

const char *name(Difficulty d) {
  return d == Difficulty::Easy ? "easy" : d == Difficulty::Medium ? "medium" : d == Difficulty::Hard ? "hard" : "extreme";
}

namespace {

// xorshift32. seed mix in generate(): seed*2654435761 + difficulty*40503 + 1. uniform() in [0,1).
struct Rng {
  uint32_t s;
  uint32_t next() {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
  }
  float uniform() { return (next() & 0xffffff) / (float)0x1000000; }
  int pick(int n) { return (int)(uniform() * n) % n; }
};

// per-level tuning. notesPerBeat in a steady bar; minGap beats between notes; subdivision slots/beat; buildBoost extra density
// at end of a build; sideways = chance a swing is left/right (not up/down).
struct Profile {
  float notesPerBeat;  // in a steady bar
  double minGap;       // beats between notes
  int subdivision;     // grid slots per beat
  float buildBoost;    // extra density at the end of a build
  float sideways;      // chance a swing goes left or right instead of up or down
};

// Easy .55/beat (dots), Medium 1.0 (lines), Hard 1.7 (arrows), Extreme 2.5 (+pairs). sideways 0/.3/.3/.35.
Profile profile(Difficulty d) {
  switch (d) {
    case Difficulty::Easy: return {0.55f, 0.9, 1, 0.5f, 0.0f};
    case Difficulty::Medium: return {1.0f, 0.45, 2, 0.7f, 0.3f};
    case Difficulty::Hard: return {1.7f, 0.24, 4, 0.9f, 0.3f};
    default: return {2.5f, 0.2, 4, 1.0f, 0.35f};
  }
}

// a candidate note time on the beat grid. beatIndex indexes Result::beats; sub is the slot within the beat; bar -1 outside bars.
struct Slot {
  double time;
  float score;   // strength of the onset that lands here, plus a pull toward strong beats
  float bass, mid, high;
  int beatIndex, sub;
  int bar;       // index into Result::bars, -1 outside
};

// index of the bar containing time t, or -1. linear scan (bars are few).
int barAt(const analysis::Result &r, double t) {
  for (int i = 0; i < (int)r.bars.size(); i++)
    if (t >= r.bars[i].start && t < r.bars[i].end) return i;
  return -1;
}

// Directions in the "up" family are swung from below, the "down" family from above.
// dir families: [0]=straight, [1..2]=slanted, [3..4]=sideways. a hand alternates families so swings flow.
const int kUp[] = {0, 4, 5, 2, 3}, kDown[] = {1, 6, 7, 2, 3};

// Section enum -> int stored in cues (visuals read it; 0 intro 1 steady 2 build 3 drop 4 break 5 outro).
int sectionCode(Section s) { return (int)s; }

// Easy cuts any way, Medium cuts along a line either way, Hard and Extreme cut the arrow's way.
// final dir code per level: Easy -> 8 (dot); Medium -> line along the arrow's axis (9 vertical, 10 horizontal, 11 '/', 12 '\\');
// Hard/Extreme keep the arrow (0-7).
int styled(Difficulty d, int dir) {
  if (d == Difficulty::Easy) return 8;
  if (d != Difficulty::Medium) return dir;
  static const int axis[9] = {9, 9, 10, 10, 12, 11, 11, 12, 8};
  return dir >= 0 && dir <= 8 ? axis[dir] : dir;
}

// one level's map. see file header for the stages. returns empty map if < 8 beats were found.
Map build(const analysis::Result &r, Difficulty diff, Rng rng, int hands) {
  Profile p = profile(diff);
  // One hand carries every note: fewer of them, and a longer rest between swings (0.28 s, not 0.19 s).
  const double handGap = hands ? 0.28 : 0.19;
  if (hands) p.notesPerBeat *= 0.8f;
  Map m{diff, {}};
  if (r.beats.size() < 8) return m;

  // The grid: each beat interval split evenly; every onset peak goes to the nearest slot.
  std::vector<Slot> slots;
  for (size_t b = 0; b + 1 < r.beats.size(); b++)
    for (int s = 0; s < p.subdivision; s++) {
      const double t = r.beats[b] + (r.beats[b + 1] - r.beats[b]) * s / p.subdivision;
      slots.push_back({t, 0, 0, 0, 0, (int)b, s, barAt(r, t)});
    }
  for (size_t i = 0; i < r.peaks.size(); i++) {
    const double t = r.peaks[i];
    auto it = std::lower_bound(slots.begin(), slots.end(), t, [](const Slot &a, double v) { return a.time < v; });
    if (it == slots.end() || t < 2.0) continue;  // a lead-in before the first note
    if (it != slots.begin() && std::fabs((it - 1)->time - t) < std::fabs(it->time - t)) --it;
    const double span = it + 1 != slots.end() ? (it + 1)->time - it->time : 0.25;
    if (std::fabs(it->time - t) > 0.6 * span) continue;
    const int f = std::clamp((int)(t * analysis::kFps), 0, (int)r.band[0].size() - 1);
    it->score = std::max(it->score, r.peakStrength[i]);
    it->bass = std::max(it->bass, r.band[0][f]);
    it->mid = std::max(it->mid, r.band[1][f]);
    it->high = std::max(it->high, r.band[2][f]);
  }
  float top = 1e-6f;
  for (auto &s : slots) top = std::max(top, s.score);
  for (auto &s : slots) {
    s.score /= top;
    // Beats and downbeats pull a little, so a sparse map falls on the pulse.
    if (s.sub == 0) s.score += (s.beatIndex % 4 == r.downbeatPhase) ? 0.25f : 0.1f;
  }

  // Choose slots bar by bar: how many depends on the difficulty, the bar's section and its intensity.
  std::vector<int> chosen;
  const double beatLen = 60.0 / r.bpm;
  for (int bi = 0; bi < (int)r.bars.size(); bi++) {
    const analysis::Bar &bar = r.bars[bi];
    float density = p.notesPerBeat;
    switch (bar.section) {
      case Section::Intro: density *= 0.5f; break;
      case Section::Break: density *= 0.15f; break;
      case Section::Outro: density *= 0.4f; break;
      case Section::Build: {
        // Ramp through the run of build bars: sparse at the start, p.buildBoost more at the end.
        int first = bi, last = bi;
        while (first > 0 && r.bars[first - 1].section == Section::Build) first--;
        while (last + 1 < (int)r.bars.size() && r.bars[last + 1].section == Section::Build) last++;
        const float k = last > first ? (float)(bi - first) / (last - first) : 1.0f;
        density *= 0.6f + (1 + p.buildBoost - 0.6f) * k;
        break;
      }
      case Section::Drop: density *= 1.25f; break;
      default: density *= 0.7f + 0.6f * bar.intensity; break;
    }
    int want = (int)std::lround(density * 4);
    std::vector<int> cand;
    for (int i = 0; i < (int)slots.size(); i++)
      if (slots[i].bar == bi && slots[i].score > 0.05f) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return slots[a].score > slots[b].score; });
    std::vector<int> picked;
    for (int c : cand) {
      if ((int)picked.size() >= want) break;
      bool ok = true;
      for (int q : picked) ok = ok && std::fabs(slots[q].time - slots[c].time) >= p.minGap * beatLen * 0.95;
      if (!ok) continue;
      // The previous bar's last note may still be too close.
      if (!chosen.empty() && slots[c].time - slots[chosen.back()].time < p.minGap * beatLen * 0.95) continue;
      picked.push_back(c);
    }
    // Something to play in every live bar, on the downbeat.
    if (picked.empty() && want > 0 && bar.section != Section::Break) {
      for (int i = 0; i < (int)slots.size(); i++)
        if (slots[i].bar == bi && slots[i].sub == 0 && slots[i].beatIndex % 4 == r.downbeatPhase) picked.push_back(i);
    }
    std::sort(picked.begin(), picked.end());
    for (int c : picked) chosen.push_back(c);
  }
  std::sort(chosen.begin(), chosen.end());
  chosen.erase(std::remove_if(chosen.begin(), chosen.end(), [&](int i) { return slots[i].time < 2.0; }), chosen.end());

  // No long empty stretches: a break or a quiet, sparse part still gets a block about every second, on the beat. (Seen on a real
  // track: 29 s of nothing on Easy, where the break density rounds to zero.) Gaps over maxGap are filled from the beat slots.
  {
    const double maxGap = diff == Difficulty::Easy ? 4.0 : diff == Difficulty::Medium ? 3.5 : diff == Difficulty::Hard ? 3.0 : 2.5;
    const double step = std::max(1.0, std::round(1.0 / beatLen)) * beatLen;  // whole beats, about a second
    const double end = r.duration - 4.0;                                     // the outro may fade out unmapped
    std::vector<int> fill;
    auto fillBetween = [&](double a, double b) {
      if (b - a <= maxGap) return;
      for (double t = a + step; b - t > step * 0.6; t += step) {
        int best = -1;
        for (int i = 0; i < (int)slots.size(); i++)
          if (slots[i].sub == 0 && (best < 0 || std::fabs(slots[i].time - t) < std::fabs(slots[best].time - t))) best = i;
        if (best < 0 || slots[best].time <= a + step * 0.5 || slots[best].time >= b - step * 0.5) continue;
        fill.push_back(best);
        t = slots[best].time;
      }
    };
    double prev = 2.0;
    for (int i : chosen) fillBetween(prev, slots[i].time), prev = slots[i].time;
    fillBetween(prev, end);
    for (int i : fill) chosen.push_back(i);
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
  }

  // Place the notes: hands alternate, swings alternate down and up, rows follow the pitch of the onset.
  int lastColor = 1;
  int lastSide[2] = {0, 0};     // the way each hand's last sideways swing went
  int lastDown[2] = {0, 0};     // whether each hand's last swing came from above
  int lastLane[2] = {1, 2}, lastRow[2] = {1, 1}, lastDir[2] = {1, 1};
  double lastTime[2] = {-9, -9};
  bool dropOpened[2048] = {};
  for (int idx : chosen) {
    const Slot &s = slots[idx];
    const analysis::Bar *bar = s.bar >= 0 ? &r.bars[s.bar] : nullptr;
    // A drop opens with both hands together on the downbeat (not in one-handed play).
    if (!hands && bar && bar->section == Section::Drop && !dropOpened[s.bar] && s.beatIndex % 4 == r.downbeatPhase && s.sub == 0 &&
        s.time - lastTime[0] >= 0.19 && s.time - lastTime[1] >= 0.19) {
      dropOpened[s.bar] = true;
      m.notes.push_back({s.time, 1, 1, 0, 1});
      m.notes.push_back({s.time, 2, 1, 1, 1});
      lastDown[0] = lastDown[1] = 1;
      lastLane[0] = 1, lastLane[1] = 2, lastRow[0] = lastRow[1] = 1;
      lastTime[0] = lastTime[1] = s.time;
      lastColor = 1;
      continue;
    }
    int color = hands ? hands - 1 : 1 - lastColor;
    // Alternate hands, but a hand that is still mid-swing from a note this close gives way to the other.
    if (!hands && s.time - lastTime[color] < 0.18 && s.time - lastTime[1 - color] >= 0.18) color = 1 - color;
    if (s.time - lastTime[color] < handGap) continue;  // the hand (or both hands) still swinging: this onset goes unplayed
    lastColor = color;
    const bool down = !lastDown[color];  // swing the other way from this hand's last
    const float total = s.bass + s.mid + s.high + 1e-6f;
    // Low sounds sit low, bright sounds high.
    int row = s.bass / total > 0.5f ? 0 : s.high / total > 0.45f ? 2 : 1;
    if (rng.uniform() < 0.2f) row = std::clamp(row + rng.pick(3) - 1, 0, 2);
    // A swing from above wants the block high enough to start over; from below, low enough.
    if (down && row == 0) row = 1;
    if (!down && row == 2) row = 1;
    const int base = color == 0 ? 1 : 2;
    int lane = base + (rng.uniform() < 0.5f ? (color == 0 ? -1 : 1) : 0);
    const bool fast = s.time - lastTime[color] < 0.45;
    if (fast) {
      lane = std::clamp(lane, lastLane[color] - 1, lastLane[color] + 1);
      row = std::clamp(row, lastRow[color] - 1, lastRow[color] + 1);
    }
    lane = std::clamp(lane, color == 0 ? 0 : 1, color == 0 ? 2 : 3);
    // The other hand's swing carries on past its note for about 0.8 m. A note in that path would be
    // cut by the wrong saber, so move out of it, or leave the onset unplayed.
    auto blocked = [&](int ln, int rw) {
      const int o = 1 - color;
      if (s.time - lastTime[o] > 0.4 || s.time - lastTime[o] < -0.01) return false;
      static const float dv[8][2] = {{0, 1}, {0, -1}, {-1, 0}, {1, 0}, {-0.7071f, 0.7071f}, {0.7071f, 0.7071f}, {-0.7071f, -0.7071f}, {0.7071f, -0.7071f}};
      const int d = lastDir[o] < 8 ? lastDir[o] : 1;
      const float ax = (lastLane[o] - 1.5f) * kLaneW, ay = lastRow[o] * kRowH;
      const float bx = ax + dv[d][0] * 0.9f, by = ay + dv[d][1] * 0.9f;
      const float px = (ln - 1.5f) * kLaneW, py = rw * kRowH;
      const float abx = bx - ax, aby = by - ay, t = std::clamp(((px - ax) * abx + (py - ay) * aby) / (abx * abx + aby * aby), 0.0f, 1.0f);
      const float cx = ax + abx * t - px, cy = ay + aby * t - py;
      return std::sqrt(cx * cx + cy * cy) < 0.45f;
    };
    if (blocked(lane, row)) {
      bool moved = false;
      for (int dl : {0, -1, 1, -2, 2})
        for (int dr : {0, 1, -1}) {
          const int ln = std::clamp(lane + dl, color == 0 ? 0 : 1, color == 0 ? 2 : 3), rw = std::clamp(row + dr, 0, 2);
          if (!moved && !blocked(ln, rw) && (!fast || (std::abs(ln - lastLane[color]) <= 1 && std::abs(rw - lastRow[color]) <= 1))) lane = ln, row = rw, moved = true;
        }
      if (!moved) continue;
    }
    // The cut direction is straight, or slanted toward the middle for blocks near the outside.
    const int *family = down ? kDown : kUp;
    int dir = family[0];
    if (!fast && rng.uniform() < 0.45f) dir = family[1 + rng.pick(2)];
    if (fast) dir = family[0];
    // Now and then a sideways swing, which leaves the up/down rhythm where it was. A block on the outer
    // lane is swung toward the middle; the others alternate.
    bool sideways = false;
    if (!fast && rng.uniform() < p.sideways) {
      sideways = true;
      const bool outer = color == 0 ? lane == 0 : lane == 3;
      const int inward = color == 0 ? 3 : 2;
      dir = outer ? inward : lastSide[color] ? 5 - inward : inward;
      lastSide[color] = dir == inward ? 0 : 1;
    }
    m.notes.push_back({s.time, lane, row, color, dir});
    lastDown[color] = sideways ? lastDown[color] : down, lastLane[color] = lane, lastRow[color] = row, lastTime[color] = s.time, lastDir[color] = dir;
    // Extreme: strong beats are sometimes hit with both hands at once.
    if (!hands && diff == Difficulty::Extreme && s.sub == 0 && s.score > 0.6f && rng.uniform() < 0.5f) {
      const int o = 1 - color;
      const bool odown = !lastDown[o];
      const int ol = std::clamp(3 - lane, o == 0 ? 0 : 1, o == 0 ? 2 : 3);
      int orow = row;
      if (odown && orow == 0) orow = 1;
      if (!odown && orow == 2) orow = 1;
      const double gap = s.time - lastTime[o];
      if (gap >= 0.19 && (color == 0 ? lane < ol : ol < lane) && std::abs(ol - lastLane[o]) <= gap / 0.1) {
        m.notes.push_back({s.time, ol, orow, o, odown ? 1 : 0});
        lastDown[o] = odown, lastLane[o] = ol, lastRow[o] = orow, lastTime[o] = s.time, lastDir[o] = odown ? 1 : 0;
      }
    }
  }
  for (Note &n : m.notes) n.dir = styled(diff, n.dir);
  std::stable_sort(m.notes.begin(), m.notes.end(), [](const Note &a, const Note &b) { return a.time < b.time; });
  return m;
}

}  // namespace

// hands as in mapper.h (one-handed maps use their own random stream and a longer swing gap). 4 maps + cues (per beat: bass/mid/high level normalised to the song's peak, and the section) for the visuals.
Song generate(const analysis::Result &r, unsigned seed, int hands) {
  Song s;
  s.bpm = r.bpm, s.duration = r.duration, s.downbeatPhase = r.downbeatPhase, s.beats = r.beats, s.bars = r.bars;
  for (double t : r.beats) {
    const int f = std::clamp((int)(t * analysis::kFps), 0, (int)r.energy[0].size() - 1);
    const int bi = barAt(r, t);
    s.cues.push_back({t, std::sqrt(r.energy[0][f]), std::sqrt(r.energy[1][f]), std::sqrt(r.energy[2][f]),
                      bi >= 0 ? sectionCode(r.bars[bi].section) : 1});
  }
  float peak[3] = {1e-9f, 1e-9f, 1e-9f};
  for (auto &c : s.cues) peak[0] = std::max(peak[0], c.bass), peak[1] = std::max(peak[1], c.mid), peak[2] = std::max(peak[2], c.high);
  for (auto &c : s.cues) c.bass /= peak[0], c.mid /= peak[1], c.high /= peak[2];
  for (Difficulty d : {Difficulty::Easy, Difficulty::Medium, Difficulty::Hard, Difficulty::Extreme}) {
    Rng rng{seed * 2654435761u + (unsigned)d * 40503u + 1 + (unsigned)hands * 7919u};
    s.maps.push_back(build(r, d, rng, hands));
  }
  return s;
}

// playability check; empty = fine. used by tools/pulse-map and tests. rules in file header.
std::vector<std::string> validate(const Map &m) {
  std::vector<std::string> bad;
  char buf[160];
  double last[2] = {-9, -9};
  int lastLane[2] = {-1, -1};
  for (size_t i = 0; i < m.notes.size(); i++) {
    const Note &n = m.notes[i];
    if (i && n.time < m.notes[i - 1].time) bad.push_back("notes out of order");
    if (n.lane < 0 || n.lane > 3 || n.row < 0 || n.row > 2 || n.color < 0 || n.color > 1 || n.dir < 0 || n.dir > 12) {
      std::snprintf(buf, sizeof buf, "note %zu out of range", i);
      bad.push_back(buf);
    }
    if (n.color >= 0 && n.color < 2) {
      const double gap = n.time - last[n.color];
      // A hand needs about 0.2 s between swings, and cannot cover more than a lane per 0.1 s.
      if (gap < 0.19 && gap > 0.001) {
        std::snprintf(buf, sizeof buf, "note %zu: hand %d only %.0f ms after its last", i, n.color, gap * 1000);
        bad.push_back(buf);
      } else if (lastLane[n.color] >= 0 && gap > 0.001 && std::abs(n.lane - lastLane[n.color]) > gap / 0.1) {
        std::snprintf(buf, sizeof buf, "note %zu: hand %d moves %d lanes in %.0f ms", i, n.color, std::abs(n.lane - lastLane[n.color]), gap * 1000);
        bad.push_back(buf);
      }
      last[n.color] = n.time, lastLane[n.color] = n.lane;
    }
    // Two notes in the same cell at the same time cannot both be hit.
    for (size_t j = i + 1; j < m.notes.size() && m.notes[j].time - n.time < 0.001; j++)
      if (m.notes[j].lane == n.lane && m.notes[j].row == n.row) bad.push_back("two notes in one cell");
  }
  return bad;
}

// .pulse text format v1: header 'pulse 1', bpm, duration, downbeat, 'bar start end section intensity', 'cue t bass mid high
// section', then per map 'map <level> <n>' and 'n time lane row color dir' lines. cache files are exactly this.
bool write(const Song &s, const char *path) {
  FILE *f = std::fopen(path, "w");
  if (!f) return false;
  std::fprintf(f, "pulse 1\nbpm %.3f\nduration %.3f\ndownbeat %d\n", s.bpm, s.duration, s.downbeatPhase);
  for (const auto &b : s.bars)
    std::fprintf(f, "bar %.3f %.3f %s %.3f\n", b.start, b.end, analysis::name(b.section), b.intensity);
  for (const auto &c : s.cues) std::fprintf(f, "cue %.3f %.3f %.3f %.3f %d\n", c.time, c.bass, c.mid, c.high, c.section);
  for (const auto &m : s.maps) {
    std::fprintf(f, "map %s %zu\n", name(m.difficulty), m.notes.size());
    for (const auto &n : m.notes) std::fprintf(f, "n %.3f %d %d %d %d\n", n.time, n.lane, n.row, n.color, n.dir);
  }
  std::fclose(f);
  return true;
}

// parses write()'s format; tolerant of unknown lines. false if no header or no maps (treated as a cache miss).
bool read(const char *path, Song &s) {
  FILE *f = std::fopen(path, "r");
  if (!f) return false;
  char line[256], word[32];
  bool header = false;
  Map *cur = nullptr;
  s = Song();
  while (std::fgets(line, sizeof line, f)) {
    if (std::sscanf(line, "%31s", word) != 1) continue;
    if (!std::strcmp(word, "pulse")) header = true;
    else if (!std::strcmp(word, "bpm")) std::sscanf(line, "bpm %lf", &s.bpm);
    else if (!std::strcmp(word, "duration")) std::sscanf(line, "duration %lf", &s.duration);
    else if (!std::strcmp(word, "downbeat")) std::sscanf(line, "downbeat %d", &s.downbeatPhase);
    else if (!std::strcmp(word, "bar")) {
      analysis::Bar b{};
      char sec[16];
      if (std::sscanf(line, "bar %lf %lf %15s %f", &b.start, &b.end, sec, &b.intensity) == 4) {
        for (int k = 0; k < 6; k++)
          if (!std::strcmp(sec, analysis::name((analysis::Section)k))) b.section = (analysis::Section)k;
        s.bars.push_back(b);
      }
    } else if (!std::strcmp(word, "cue")) {
      Song::Cue c{};
      if (std::sscanf(line, "cue %lf %f %f %f %d", &c.time, &c.bass, &c.mid, &c.high, &c.section) == 5) s.cues.push_back(c), s.beats.push_back(c.time);
    } else if (!std::strcmp(word, "map")) {
      char d[16];
      std::sscanf(line, "map %15s", d);
      s.maps.push_back({!std::strcmp(d, "easy") ? Difficulty::Easy : !std::strcmp(d, "medium") ? Difficulty::Medium : !std::strcmp(d, "hard") ? Difficulty::Hard : Difficulty::Extreme, {}});
      cur = &s.maps.back();
    } else if (!std::strcmp(word, "n") && cur) {
      Note n{};
      if (std::sscanf(line, "n %lf %d %d %d %d", &n.time, &n.lane, &n.row, &n.color, &n.dir) == 5) cur->notes.push_back(n);
    }
  }
  std::fclose(f);
  return header && !s.maps.empty();
}

}  // namespace mapper
