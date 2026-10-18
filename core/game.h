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
  float laneW = mapper::kLaneW, rowH = mapper::kRowH, row0 = 1.0f;
  float noteSize = 0.4f;
  float minSwingSpeed = 1.2f; // m/s of the tip; slower only touches a note
  float missAfter = 0.25f;    // s past its time a note is lost
  float swingWindow = 0.35f;  // s either side of its time a note can be cut
  bool noFail = true;
  bool touchCuts = false;     // a dot cut by the correct saber on contact, however slowly it moves (Easy)
};

// The play area for shorter players, children, seated play or tight spaces: 0 standard, 1 compact, 2 small. Lanes, rows, block size and the
// distance of the hit plane shrink together about the centre of the field (the middle row stays at 1.55 m), so the blocks sit closer and
// every pattern still plays; the maps themselves do not change. Blade length (main.cpp) is separate.
// seated: the field drops so the middle row is at 1.3 m (the head is 1.6 m after the recentre, seated or not) and the rows sit 10% closer,
// so nothing needs a reach above the head or down to the floor from a chair. Lanes and block size stay as the area sets them.
inline Config configFor(int area, bool seated = false) {
  static const float scale[3] = {1.0f, 0.85f, 0.7f};
  const float k = scale[area < 0 ? 0 : area > 2 ? 2 : area];
  Config c;
  c.laneW *= k, c.rowH *= k, c.noteSize *= k;
  c.hitZ = -0.8f * (0.6f + 0.4f * k);
  if (seated) c.rowH *= 0.9f;
  c.row0 = (seated ? 1.3f : 1.55f) - c.rowH;
  return c;
}

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

// Why a note was missed or cut badly, gathered while it was in reach (diagnostics for tuning, read after the event).
struct Diag {
  float closest = 1e9f;        // smallest distance from the correct saber's blade to the note centre, metres
  float dx = 0, dy = 0, dz = 0;  // blade point minus note centre at that moment (x right, y up, z toward the player)
  double at = 0;               // seconds relative to the note's time when it was closest (negative = early)
  float maxSpeed = 0;          // fastest tip speed of that saber while the note was in reach, m/s
  bool touchedSlow = false;    // the blade touched the block but moved too slowly to cut it
  int reason = 0;              // BadCut only: 1 wrong colour, 2 wrong direction, 3 too slow
  float swingDeg = 0, needDeg = 0;  // BadCut: the swing's angle and the one the block asked for (degrees, 0 = right, 90 = up); needDeg 999 = any
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
  const Diag &diag(size_t i) const { return diag_[i]; }
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
  std::vector<Diag> diag_;
  Stats stats_;
  long maxScore_ = 0;
  int maxCombo_ = 0;
  std::vector<Sample> history_[2];
  std::vector<Pending> pending_;
  std::vector<Event> events_;
  size_t first_ = 0;
  double last_ = -1e9;
};

// Closest distance between the two blades (segments base->tip), metres; `mid` = halfway between the closest points.
float bladeGap(const SaberPose &a, const SaberPose &b, Vec3 *mid = nullptr);

// Blade-on-blade contact for spark, clink and haptics. update() once per frame with both poses and the song time:
// returns the strength 0..1 to buzz both controllers with (0 = nothing this frame) and where they met.
// contract: fires on the frame the blades first come within kReach (two blades ~3 cm thick each + margin), harder the faster
// they closed; while they stay together it repeats gently every kGrind s (a scrape), and not again within kGap s of a clash.
// W: separate from Session: it is feedback only, never touches score. T: main thread, any clock that moves forward.
class ClashDetector {
 public:
  static constexpr float kReach = 0.08f;
  static constexpr double kGrind = 0.15, kGap = 0.10;
  float update(double t, const SaberPose pose[2], Vec3 &at);

 private:
  bool touching_ = false, have_ = false;
  double lastFire_ = -1e9, lastT_ = 0;
  Vec3 prevTip_[2];
};

}  // namespace game
