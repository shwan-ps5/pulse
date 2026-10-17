#pragma once

// A scripted player for tests and demos: swings each saber through its notes along their cut
// directions, a little before the note reaches the plane (a cut counts when the blade first touches).
#include <algorithm>
#include <cmath>
#include <vector>

#include "game.h"

namespace game {

class Bot {
 public:
  Bot(const std::vector<mapper::Note> &notes, const Config &cfg = Config(), float lead = 0.03f, float reach = 0.8f, float swingTime = 0.09f)
      : cfg_(cfg) {
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
      const double t = n.time - lead;
      auto &w = way_[n.color];
      auto add = [&](double at, float px, float py) {
        if (!w.empty() && at <= w.back().t) at = w.back().t + 0.002;
        w.push_back({at, px, py});
      };
      add(t - swingTime, x - v[d][0] * reach, y - v[d][1] * reach);
      add(t + swingTime, x + v[d][0] * reach, y + v[d][1] * reach);
    }
  }

  SaberPose pose(int color, double t) const {
    const auto &w = way_[color];
    float x = 0.25f * (color ? 1 : -1), y = 1.0f;
    if (!w.empty()) {
      if (t <= w.front().t) x = w.front().x, y = w.front().y;
      else if (t >= w.back().t) x = w.back().x, y = w.back().y;
      else {
        const auto it = std::upper_bound(w.begin(), w.end(), t, [](double v, const Way &a) { return v < a.t; });
        const Way &b = *it, &a = *(it - 1);
        const float u = (float)((t - a.t) / (b.t - a.t));
        x = a.x + (b.x - a.x) * u, y = a.y + (b.y - a.y) * u;
      }
    }
    return {{x, y, -0.35f}, {x, y, -0.35f - 1.0f}};
  }

 private:
  struct Way {
    double t;
    float x, y;
  };
  Config cfg_;
  std::vector<Way> way_[2];
  bool down_[2] = {false, false};
};

}  // namespace game
