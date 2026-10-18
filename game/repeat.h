#pragma once

// Key-repeat for a held menu direction: fires on the press, again after kFirst s, then faster the longer it is held, so a long list
// (300 songs) scrolls when the stick is held but a tap still moves exactly one. Header only, no platform: tests/support_test.cpp.
// contract: update(on, now) once per frame; true = move one step now. `repeated` says that step came from holding, not from the press
// (the song list stops at its ends on a repeat instead of wrapping round).
class AutoRepeat {
 public:
  static constexpr double kFirst = 0.40, kSlow = 0.12, kMedium = 0.07, kFast = 0.04, kMediumAfter = 0.8, kFastAfter = 1.5;
  bool repeated = false;

  bool update(bool on, double now) {
    repeated = false;
    if (!on) {
      held_ = false;
      return false;
    }
    if (!held_) {
      held_ = true, since_ = now, next_ = now + kFirst;
      return true;
    }
    if (now < next_) return false;
    const double heldFor = now - since_;
    next_ = now + (heldFor > kFastAfter ? kFast : heldFor > kMediumAfter ? kMedium : kSlow);
    repeated = true;
    return true;
  }

 private:
  bool held_ = false;
  double since_ = 0, next_ = 0;
};
