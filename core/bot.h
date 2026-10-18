#pragma once

// A scripted player for tests and demos: swings each saber through its notes along their cut
// directions, a little before the note reaches the plane (a cut counts when the blade first touches).
#include <algorithm>
#include <cmath>
#include <vector>

#include "game.h"

namespace game {

// contract: for each hand builds timed waypoints (time, x, y) from `notes`: before each note, start `reach` m back along
// the cut axis (dir code; lines/dots resolved to alternating down/up via down_[]), end `reach` m past it, `lead` s before the
// note time (blade touches first). pose(c,t) interpolates linearly; saber is a 1 m blade pointing -z at z=-0.35.
// tests only: used to pin rules and to measure a perfect-player ceiling (52-72% accuracy by level).
// Style::human = the demo player on the console: same waypoints, then made less robotic. W: a robot's straight, constant-speed,
// metronomic swings look dead in a headset. inv: only SMALL deviations from the perfect path, so a human bot still hits ~90%+:
//   ease in/out of each segment (smoothstep), arc the path sideways, +-12 ms timing and +-10% reach per swing (reach <= 0.5 m), arms
//   that pivot at the chest instead of a hand floating at the note (see pose()), the whole body bobs on the beat (style.bpm) and sways over a bar, and ~slop of the swings land 0.5 m off (a miss).
// All of it is deterministic from style.seed (same song, same demo).
struct BotStyle {
  bool human = false;
  float bpm = 120;      // beat the body bobs to
  float slop = 0.04f;   // share of swings that are deliberately off target
  float blade = 1.0f;   // m, blade length drawn/judged
  float eye = 1.6f;     // m, the player's eye height: hands work around eye - 0.55 (waist to chest)
  unsigned seed = 1;
};

class Bot {
 public:
  Bot(const std::vector<mapper::Note> &notes, const Config &cfg = Config(), float lead = 0.03f, float reach = 0.8f, float swingTime = 0.09f,
      const BotStyle &style = BotStyle())
      : cfg_(cfg), style_(style) {
    state_ ^= style.seed * 2654435761u;
    if (!state_) state_ = 1;
    static const float v[8][2] = {{0, 1}, {0, -1}, {-1, 0}, {1, 0}, {-0.7071f, 0.7071f}, {0.7071f, 0.7071f}, {-0.7071f, -0.7071f}, {0.7071f, -0.7071f}};
    std::vector<mapper::Note> sorted = notes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const mapper::Note &a, const mapper::Note &b) { return a.time < b.time; });
    for (const auto &n : sorted) {
      int d = n.dir;
      if (d >= 8) {
        static const int downWay[5] = {1, 1, 3, 6, 7}, upWay[5] = {0, 0, 2, 5, 4};
        d = (down_[n.color] ? upWay : downWay)[d - 8];
      }
      down_[n.color] = d == 1 || d == 6 || d == 7 ? true : d == 0 || d == 4 || d == 5 ? false : down_[n.color];
      const float x = (n.lane - 1.5f) * cfg.laneW, y = cfg.row0 + n.row * cfg.rowH;
      double t = n.time - lead;
      float r = reach, ox = 0, oy = 0;
      if (style_.human) {
        t += (rnd() - 0.5) * 0.024;
        r = std::min(r, 0.5f) * (0.9f + 0.2f * (float)rnd());  // a person's swing is shorter than the robot's, and stays within reach
        if (rnd() < style_.slop) ox = (rnd() < 0.5 ? -0.5f : 0.5f) * (std::fabs(v[d][1]) > 0.5f ? 1.0f : 0.0f), oy = std::fabs(v[d][1]) > 0.5f ? 0.0f : 0.5f;
      }
      auto &w = way_[n.color];
      auto add = [&](double at, float px, float py) {
        if (!w.empty() && at <= w.back().t) at = w.back().t + 0.002;
        w.push_back({at, px, py});
      };
      add(t - swingTime, x + ox - v[d][0] * r, y + oy - v[d][1] * r);
      add(t + swingTime, x + ox + v[d][0] * r, y + oy + v[d][1] * r);
    }
  }

  SaberPose pose(int color, double t) const {
    float x, y;
    where(color, t, x, y);
    if (!style_.human) return {{x, y, -0.35f}, {x, y, -0.35f - style_.blade}};
    // Arms, not a floating cursor: the blade pivots about a point in front of the chest (waist-to-chest height, one per hand) and always
    // points from there THROUGH the contact point C on the hit plane. The tip is C scaled out from the pivot by a constant g, so the tip
    // moves exactly like C (the rules read the tip's direction and speed, which therefore stay what the path says) and the hand is
    // blade-length back from the tip. For a high block the hands rise only to chest height and the blades fan up and out: never
    // parallel overhead. inv: C lies on the blade (g > 1 and the blade is longer than (g-1)*|C-pivot|).
    const float py = style_.eye - 0.55f, pz = -0.3f;
    // raising the arms swings the elbows out: the pivots spread as the target rises, so two blades up high lean toward each other (a V)
    const float spread = 0.22f + 0.3f * std::min(1.0f, std::max(0.0f, (y - py - 0.5f) / 0.8f)), px = spread * (color ? 1 : -1);
    const float cx = x - px, cy = y - py, cz = cfg_.hitZ - pz, len = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float g = (style_.blade + 0.30f) / 1.15f;
    // hand distance from the pivot along the blade; linear in |C| (so the tip is the scaled path) except at the ends of the range:
    // a stretched arm (0.42 m) is as far as a hand goes, the tip then falls short of the scaled path (only at the far ends of a swing).
    const float a = std::min(0.42f, std::max(0.08f, len * g - style_.blade));
    const float ux = cx / len, uy = cy / len, uz = cz / len;
    const Vec3 h{px + ux * a, py + uy * a, pz + uz * a};
    return {h, {h.x + ux * style_.blade, h.y + uy * style_.blade, h.z + uz * style_.blade}};
  }

 private:
  struct Way {
    double t;
    float x, y;
  };

  // deterministic 0..1 from the style seed (xorshift): the same song always gets the same demo.
  double rnd() {
    state_ ^= state_ << 13, state_ ^= state_ >> 17, state_ ^= state_ << 5;
    return (state_ & 0xffffff) / 16777216.0;
  }

  // hand position at t: waypoint path (linear for the test bot; eased, arced, bobbing for the human one).
  void where(int color, double t, float &x, float &y) const {
    const auto &w = way_[color];
    x = (style_.human ? 0.45f : 0.25f) * (color ? 1 : -1), y = style_.human ? 1.05f : 1.0f;
    if (!w.empty()) {
      if (t <= w.front().t) x = w.front().x, y = w.front().y;
      else if (t >= w.back().t) x = w.back().x, y = w.back().y;
      else {
        const auto it = std::upper_bound(w.begin(), w.end(), t, [](double v, const Way &a) { return v < a.t; });
        const Way &b = *it, &a = *(it - 1);
        float u = (float)((t - a.t) / (b.t - a.t));
        if (style_.human) {
          const float dx = b.x - a.x, dy = b.y - a.y, dist = std::sqrt(dx * dx + dy * dy);
          const bool swing = (it - w.begin()) % 2 == 1;  // odd index ends a swing; the others are travel between swings
          const float side = ((it - w.begin()) / 2) % 2 ? 1.0f : -1.0f, bend = (swing ? 0.04f : 0.12f * std::min(1.0f, dist)) * std::sin(3.14159265f * u) * side;
          u = u * u * (3 - 2 * u);
          x = a.x + dx * u, y = a.y + dy * u;
          if (dist > 1e-4f) x += -dy / dist * bend, y += dx / dist * bend;
        } else {
          x = a.x + (b.x - a.x) * u, y = a.y + (b.y - a.y) * u;
        }
      }
    }
    if (style_.human) {
      const double beat = t * style_.bpm / 60.0;
      y += 0.03f * (float)std::cos(2 * 3.14159265 * beat) + 0.012f * (float)std::sin(2 * 3.14159265 * beat / 2 + color);
      x += 0.025f * (float)std::sin(2 * 3.14159265 * beat / 8) * (color ? -1 : 1) * 0.6f + 0.02f * (float)std::sin(2 * 3.14159265 * beat / 8);
    }
  }

  Config cfg_;
  BotStyle style_;
  unsigned state_ = 2463534242u;
  std::vector<Way> way_[2];
  bool down_[2] = {false, false};
};

}  // namespace game
