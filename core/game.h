#pragma once

// The rules: sabers cutting notes, scoring, combo, energy. No graphics or platform in here, so the
// desktop game, the headset build and the tests all run the same code. Scene space: metres, y up, the
// player at the origin facing -z; notes arrive at the hit plane z = hitZ at their time.
#include <vector>

#include "mapper.h"

namespace game {

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

// A saber as the controller sets it: hilt and tip.
struct SaberPose {
  Vec3 base, tip;
};

struct Config {
  float noteSpeed = 14;       // m/s toward the player
  float hitZ = -0.8f;         // where a note is at its time
  float laneW = 0.6f, rowH = 0.55f, row0 = 1.0f;
  float noteSize = 0.4f;
  float minSwingSpeed = 1.2f; // m/s of the tip; slower only touches a note
  float missAfter = 0.25f;    // s past its time a note is lost
  float swingWindow = 0.35f;  // s either side of its time a note can be cut
  bool noFail = true;
};

enum class NoteState : unsigned char { Pending, Hit, BadCut, Missed };

struct Event {
  enum Kind { Hit, BadCut, Miss, ScoreFinal } kind;
  int note;
  double time;
  Vec3 pos;
  int color;
  int score;  // ScoreFinal: points for this cut (pre + post + accuracy) x multiplier at the cut
  int multiplier;
};

struct Stats {
  long score = 0;
  int combo = 0, maxCombo = 0, multiplier = 1;
  int hits = 0, badCuts = 0, misses = 0;
  float energy = 0.5f;
  bool failed = false;
  long maxScoreSoFar = 0;  // what a perfect run would have scored on the notes resolved so far
};

class Session {
 public:
  Session(const std::vector<mapper::Note> &notes, const Config &cfg = Config());
  // Call once per frame with the sabers' poses in the scene. `now` is the song time in seconds.
  // A jump backward or a long gap (a seek, a pause) resets the swing history.
  void update(double now, const SaberPose saber[2]);
  // Starts again from `now` (a seek): notes before it count as not played.
  void restart(double now);

  const Stats &stats() const { return stats_; }
  NoteState state(size_t i) const { return state_[i]; }
  const std::vector<mapper::Note> &notes() const { return notes_; }
  const Config &config() const { return cfg_; }
  Vec3 notePos(size_t i, double now) const;
  // The events since the last call.
  std::vector<Event> takeEvents();
  long maxScore() const { return maxScore_; }
  static int multiplierFor(int combo);

 private:
  struct Sample {
    double t;
    SaberPose p;
  };
  struct Pending {  // a cut waiting for its follow-through
    int note;
    double t;
    float pre, acc;
    float scale;    // 1, or 0.5 for a line cut across instead of along it
    float swept;    // follow-through angle so far, radians
    int multiplier;
    Vec3 pos;
    int color;
  };
  float sweptSince(int c, double t0) const;
  void loseCombo();
  void resolve(size_t i, NoteState s, double now, int color, Vec3 pos);

  std::vector<mapper::Note> notes_;
  Config cfg_;
  std::vector<NoteState> state_;
  Stats stats_;
  long maxScore_ = 0;
  int maxCombo_ = 0;
  std::vector<Sample> history_[2];
  std::vector<Pending> pending_;
  std::vector<Event> events_;
  size_t first_ = 0;
  double last_ = -1e9;
};

}  // namespace game
