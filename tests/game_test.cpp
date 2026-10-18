// Rules checks: a bot swinging along the cut directions scores, wrong colour / direction / slow
// swings don't, misses cost the combo, and a full generated map plays through.
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>

#include "bot.h"
#include "analysis.h"
#include "game.h"
#include "mapper.h"
#include "scores.h"

using namespace game;

static int failures;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

// Plays `notes` with a pose function for each saber; returns the session.
template <class F>
static Session play(const std::vector<mapper::Note> &notes, F poseOf, double end) {
  Session s(notes);
  for (double t = 0; t < end; t += 1.0 / 90) {
    const SaberPose p[2] = {poseOf(0, t), poseOf(1, t)};
    s.update(t, p);
  }
  return s;
}

static SaberPose still(float x, float y) { return {{x, y, -0.35f}, {x, y, -1.35f}}; }

// each numbered block pins one rule; see core/game.cpp header. #8 = the four levels as the help screen states them, #9 = Easy touch
// cuts, #10 = rank letters/boundaries. a failing CHECK prints file:line and the process exits 1 (make test).
int main() {
  // 1. One red note, cut down by the bot: a hit, a decent score, then the follow-through finalises it.
  {
    std::vector<mapper::Note> notes = {{5.0, 1, 1, 0, 1}};
    Bot bot(notes);
    Session s = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 6.0);
    CHECK(s.stats().hits == 1 && s.stats().misses == 0 && s.stats().badCuts == 0, "bot hit: %d hits %d miss %d bad", s.stats().hits, s.stats().misses, s.stats().badCuts);
    CHECK(s.stats().score > 40 && s.stats().score <= 115, "single score %ld", s.stats().score);
  }
  // 2. Standing still, a note passes through the blade without a cut: a miss.
  {
    std::vector<mapper::Note> notes = {{5.0, 1, 1, 0, 1}};
    Session s = play(notes, [&](int, double) { return still(-0.3f, 1.55f); }, 6.0);
    CHECK(s.stats().hits == 0 && s.stats().misses == 1, "still blade should miss: %d hits %d misses", s.stats().hits, s.stats().misses);
  }
  // 3. The wrong colour of saber through the note is a bad cut.
  {
    std::vector<mapper::Note> notes = {{5.0, 1, 1, 0, 1}};
    std::vector<mapper::Note> asBlue = {{5.0, 1, 1, 1, 1}};
    Bot bot(asBlue);  // swings the blue saber through where the red note is
    Session s = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 6.0);
    CHECK(s.stats().badCuts == 1 && s.stats().hits == 0, "wrong colour: %d hits %d bad", s.stats().hits, s.stats().badCuts);
  }
  // 4. The correct saber swung the wrong way (up through a down note) is a bad cut.
  {
    std::vector<mapper::Note> notes = {{5.0, 1, 1, 0, 1}};
    std::vector<mapper::Note> up = {{5.0, 1, 1, 0, 0}};
    Bot bot(up);
    Session s = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 6.0);
    CHECK(s.stats().badCuts == 1 && s.stats().hits == 0, "wrong direction: %d hits %d bad", s.stats().hits, s.stats().badCuts);
  }
  // 5. A dot note takes any direction.
  {
    std::vector<mapper::Note> notes = {{5.0, 2, 1, 1, 8}};
    std::vector<mapper::Note> side = {{5.0, 2, 1, 1, 3}};
    Bot bot(side);
    Session s = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 6.0);
    CHECK(s.stats().hits == 1, "dot note: %d hits %d bad", s.stats().hits, s.stats().badCuts);
  }
  // 6. A miss ends the combo and multiplier; multipliers climb 1 -> 2 -> 4 -> 8 with the combo.
  CHECK(Session::multiplierFor(0) == 1 && Session::multiplierFor(2) == 2 && Session::multiplierFor(6) == 4 && Session::multiplierFor(14) == 8, "multiplier table");
  {
    std::vector<mapper::Note> notes;
    for (int i = 0; i < 20; i++) notes.push_back({4.0 + i * 0.6, 1 + (i % 2), 1, i % 2, 1});
    Bot bot(notes);
    Session s = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 4.0 + 20 * 0.6 + 1);
    CHECK(s.stats().hits == 20 && s.stats().maxCombo == 20 && s.stats().multiplier == 8, "20-note run: %d hits combo %d x%d", s.stats().hits, s.stats().maxCombo, s.stats().multiplier);
    CHECK(s.stats().score > s.maxScore() / 2, "score %ld of %ld", s.stats().score, s.maxScore());
  }
  // 7. A real generated map, played by the bot at every difficulty.
  {
    mapper::Song song;
    if (mapper::read("build/satisfaction/map.pulse", song)) {
      for (const auto &m : song.maps) {
        Bot bot(m.notes);
        Session s = play(m.notes, [&](int c, double t) { return bot.pose(c, t); }, song.duration + 1);
        const Stats &st = s.stats();
        std::printf("%-6s %4zu notes: %4d hit %2d miss %2d bad, combo %4d, %6ld / %ld (%.1f%%)\n", mapper::name(m.difficulty), m.notes.size(), st.hits, st.misses,
                    st.badCuts, st.maxCombo, st.score, s.maxScore(), 100.0 * st.score / s.maxScore());
        CHECK(st.hits >= (int)(m.notes.size() * 0.97), "bot should hit nearly everything: %d of %zu", st.hits, m.notes.size());
      }
    } else {
      std::printf("(no build/satisfaction/map.pulse, run pulse-map first; skipping the full-map test)\n");
    }
  }
  // 8. The four levels as the in-game help describes them.
  {
    auto run = [&](const std::vector<mapper::Note> &truth, const std::vector<mapper::Note> &swing) {
      Bot bot(swing);
      return play(truth, [&](int c, double t) { return bot.pose(c, t); }, truth.back().time + 1.5);
    };
    auto seq = [&](std::initializer_list<int> dirs, int color = 0) {
      std::vector<mapper::Note> v;
      double t = 4.0;
      for (int d : dirs) v.push_back({t, 1, 1, color, d}), t += 0.7;
      return v;
    };
    // Easy: a dot takes a cut in any of the eight directions, for full points.
    long full = 0;
    for (int d = 0; d < 8; d++) {
      Session s = run(seq({8}), seq({d}));
      CHECK(s.stats().hits == 1 && s.stats().badCuts == 0, "dot cut along %d: %d hits %d bad", d, s.stats().hits, s.stats().badCuts);
      if (d == 1) full = s.stats().score;
    }
    // Medium: a vertical line cut down or up scores in full, cut across for about half, and never breaks the combo.
    for (int along : {1, 0}) {
      Session s = run(seq({9}), seq({along}));
      CHECK(s.stats().hits == 1 && s.stats().badCuts == 0 && s.stats().score > full * 9 / 10, "line along %d: score %ld vs %ld", along, s.stats().score, full);
    }
    for (int across : {2, 3}) {
      Session s = run(seq({9}), seq({across}));
      CHECK(s.stats().hits == 1 && s.stats().badCuts == 0, "line across %d: %d hits %d bad", across, s.stats().hits, s.stats().badCuts);
      CHECK(s.stats().score > full * 4 / 10 && s.stats().score < full * 6 / 10, "line across %d scores %ld, half of %ld expected", across, s.stats().score, full);
    }
    {
      Session s = run(seq({9, 9, 9, 9}), seq({1, 3, 1, 2}));
      CHECK(s.stats().maxCombo == 4 && s.stats().badCuts == 0, "across cuts keep the combo: combo %d bad %d", s.stats().maxCombo, s.stats().badCuts);
    }
    // A diagonal line takes the diagonal, either way, in full; a flat swing through it is across.
    {
      Session along = run(seq({11}), seq({6})), flat = run(seq({11}), seq({3}));
      CHECK(along.stats().score > full * 9 / 10 && flat.stats().hits == 1 && flat.stats().score < full * 6 / 10, "diagonal line: along %ld across %ld", along.stats().score, flat.stats().score);
    }
    // Hard: an arrow takes only its direction. A wrong cut scores nothing and drops the combo and multiplier.
    {
      Session s = run(seq({1, 1, 1, 1, 1, 1, 7}), seq({1, 1, 1, 1, 1, 1, 3}));
      CHECK(s.stats().badCuts == 1 && s.stats().combo == 0 && s.stats().multiplier == 1 && s.stats().maxCombo == 6, "wrong arrow: bad %d combo %d x%d best %d", s.stats().badCuts,
            s.stats().combo, s.stats().multiplier, s.stats().maxCombo);
    }
    {
      Session sideways = run(seq({7}), seq({3})), opposite = run(seq({1}), seq({0}));
      CHECK(sideways.stats().badCuts == 1 && sideways.stats().score == 0, "diagonal arrow cut flat is a bad cut: %d bad %ld", sideways.stats().badCuts, sideways.stats().score);
      CHECK(opposite.stats().badCuts == 1 && opposite.stats().score == 0, "down arrow cut up is a bad cut: %d bad %ld", opposite.stats().badCuts, opposite.stats().score);
    }
  }
  // 9. Easy: a saber held still in front of a dot cuts it, but not for other notes or the wrong colour.
  {
    Config touch;
    touch.touchCuts = true;
    auto held = [&](const std::vector<mapper::Note> &notes, const Config &cfg, int heldColor) {
      Session s(notes, cfg);
      for (double t = 0; t < 6.0; t += 1.0 / 90) {
        const SaberPose p[2] = {heldColor == 0 ? still(-0.3f, 1.55f) : still(-1.5f, 0.2f), heldColor == 1 ? still(-0.3f, 1.55f) : still(1.5f, 0.2f)};
        s.update(t, p);
      }
      return s.stats();
    };
    const Stats dot = held({{5.0, 1, 1, 0, 8}}, touch, 0);
    CHECK(dot.hits == 1 && dot.badCuts == 0, "held saber on a dot: %d hits %d bad %d miss", dot.hits, dot.badCuts, dot.misses);
    CHECK(held({{5.0, 1, 1, 0, 8}}, Config(), 0).hits == 0, "held saber without touch cuts should miss");
    CHECK(held({{5.0, 1, 1, 0, 1}}, touch, 0).hits == 0, "held saber on an arrow should still miss");
    const Stats wrong = held({{5.0, 1, 1, 0, 8}}, touch, 1);
    CHECK(wrong.hits == 0 && wrong.badCuts == 0, "wrong colour held still: %d hits %d bad", wrong.hits, wrong.badCuts);
  }
  // 11. Diagnostics: a blade held still in the block is "touched slow"; one held 40 cm to the side shows the offset, never touches.
  {
    std::vector<mapper::Note> notes = {{5.0, 1, 1, 0, 1}};
    Session inside = play(notes, [&](int, double) { return still(-0.3f, 1.55f); }, 6.0);
    CHECK(inside.stats().misses == 1 && inside.diag(0).touchedSlow && inside.diag(0).closest < 0.1f, "held in the block: slow %d closest %.2f", inside.diag(0).touchedSlow, (double)inside.diag(0).closest);
    Session beside = play(notes, [&](int, double) { return still(0.2f, 1.55f); }, 6.0);
    CHECK(beside.stats().misses == 1 && !beside.diag(0).touchedSlow && beside.diag(0).closest > 0.35f && beside.diag(0).dx > 0.35f, "held beside: closest %.2f dx %.2f", (double)beside.diag(0).closest, (double)beside.diag(0).dx);
    std::vector<mapper::Note> up = {{5.0, 1, 1, 0, 0}};
    Bot bot(up);
    Session wrong = play(notes, [&](int c, double t) { return bot.pose(c, t); }, 6.0);
    CHECK(wrong.diag(0).reason == 2 && wrong.diag(0).needDeg < 900 && std::fabs(wrong.diag(0).needDeg + 90) < 1, "wrong direction diag: reason %d need %.0f", wrong.diag(0).reason, (double)wrong.diag(0).needDeg);
  }
  // 12. One-handed maps: every note is that hand's colour, kept to its three lanes, at least 0.28 s apart, no pairs, and valid.
  {
    analysis::Result r;
    r.duration = 60, r.bpm = 126, r.downbeatPhase = 0;
    const int frames = (int)(60 * analysis::kFps);
    for (int b = 0; b < 3; b++) r.band[b].assign(frames, 0.3f), r.energy[b].assign(frames, 0.2f);
    const double beat = 60.0 / 126;
    for (double t = 1.0; t < 59; t += beat) r.beats.push_back(t);
    for (size_t i = 0; i + 4 < r.beats.size(); i += 4) r.bars.push_back({r.beats[i], r.beats[i + 4], 0.5f, 0.4f, 0.3f, 0.5f, 0.5f, 0.6f, analysis::Section::Steady});
    for (double t = 2.0; t < 58; t += beat / 2) r.peaks.push_back(t), r.peakStrength.push_back(0.6f + 0.4f * std::fmod(t, 1.0));
    for (int hands = 0; hands < 3; hands++) {
      const mapper::Song song = mapper::generate(r, 1, hands);
      for (const auto &m : song.maps) {
        const auto problems = mapper::validate(m);
        CHECK(problems.empty(), "hands %d %s: %s", hands, mapper::name(m.difficulty), problems.empty() ? "" : problems[0].c_str());
        CHECK(m.notes.size() > 10, "hands %d %s has only %zu notes", hands, mapper::name(m.difficulty), m.notes.size());
        for (size_t i = 0; i < m.notes.size(); i++) {
          const mapper::Note &n = m.notes[i];
          if (!hands) continue;
          CHECK(n.color == hands - 1, "hands %d: note %zu has colour %d", hands, i, n.color);
          CHECK(hands == 1 ? n.lane <= 2 : n.lane >= 1, "hands %d: note %zu in lane %d", hands, i, n.lane);
          if (i) CHECK(n.time - m.notes[i - 1].time >= 0.279, "hands %d %s: notes %zu and %zu only %.0f ms apart", hands, mapper::name(m.difficulty), i - 1, i,
                       (n.time - m.notes[i - 1].time) * 1000);
        }
      }
    }
  }
  // 10. Ranks: S 90, A 80, B 70, C 50, D 40, E 30, F below.
  {
    auto grade = [](float acc, int hits, int notes) { return std::string(scores::rank(scores::rating(acc, hits, notes))); };
    CHECK(grade(14.2f, 171, 197) == "C", "87%% of blocks hit with short combos should be a C, got %s", grade(14.2f, 171, 197).c_str());
    CHECK(grade(35, 190, 200) == "B", "95%% hit, 35%% of max should be a B, got %s", grade(35, 190, 200).c_str());
    CHECK(grade(55, 200, 200) == "S" || grade(55, 200, 200) == "A", "everything hit should be S or A, got %s", grade(55, 200, 200).c_str());
    CHECK(grade(100, 200, 200) == "S", "a perfect run is S");
    CHECK(grade(8, 140, 200) == "D", "70%% hit, tiny score should be a D, got %s", grade(8, 140, 200).c_str());
    CHECK(grade(0, 0, 200) == "F", "nothing hit is an F");
  const struct { float r; const char *rank; } edges[] = {{100, "S"}, {90, "S"}, {89.9f, "A"}, {80, "A"}, {79.9f, "B"}, {70, "B"}, {69.9f, "C"}, {50, "C"}, {49.9f, "D"},
                                                         {40, "D"}, {39.9f, "E"}, {30, "E"}, {29.9f, "F"}, {0, "F"}};
  for (const auto &e : edges) CHECK(std::string(scores::rank(e.r)) == e.rank, "rating %.1f should be %s, got %s", (double)e.r, e.rank, scores::rank(e.r));
  }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
