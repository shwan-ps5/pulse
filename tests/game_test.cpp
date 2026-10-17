// Rules checks: a bot swinging along the cut directions scores, wrong colour / direction / slow
// swings don't, misses cost the combo, and a full generated map plays through.
#include <cstdio>
#include <cstdlib>

#include "bot.h"
#include "game.h"

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
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
