// rules engine. no gfx/platform deps: runs identically in desktop game, headset build, tests (tests/game_test.cpp).
// CS: scene space (m, y up, player at origin facing -z); notes fly +z toward hit plane z=cfg.hitZ, arrive at note.time.
// flow: Session::update(now, saber[2]) once/frame -> per pending note in [first_, now+swingWindow]: miss if now-time>missAfter;
//       else test blade segment (prev..cur pose, 5 lerp steps) vs note box; touch => classify => resolve(); Hit queues Pending
//       (score finalised kFollowThrough s later from swept angle). events_ drained by takeEvents() for fx/haptics/sfx.
// score/cut = (pre<=70 + post<=30 + acc<=15) * multiplier * scale ; max/note 115 ; multiplier 1,2,4,8 at combo 0,2,6,14.
// per-level rules live in the cut classifier in update(): dot(8)=any dir; lines(9-12)=along axis full, across x0.5, never breaks
//   combo on direction; arrows(0-7)=need dir else BadCut (score 0, combo=0); wrong colour always BadCut. cfg.touchCuts (Easy):
//   right-colour dot cut on touch at any speed. tests/game_test.cpp #8-#9 pin all of this: change rules => update tests+help text.
#include "game.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

// small vec3 helpers (no vmath dep: keeps core/ platform-free).
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 lerp(Vec3 a, Vec3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }

// Does the segment a-b pass through the axis-aligned box? On a hit, `at` is where it first does.
// slab test. pre: half>0. post: at=first entry point (lerp t0) when true. segment a->b = blade/tip sweep this frame.
bool segmentBox(Vec3 a, Vec3 b, Vec3 centre, float half, Vec3 &at) {
  const float lo[3] = {centre.x - half, centre.y - half, centre.z - half};
  const float hi[3] = {centre.x + half, centre.y + half, centre.z + half};
  const float p[3] = {a.x, a.y, a.z}, d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
  float t0 = 0, t1 = 1;
  for (int k = 0; k < 3; k++) {
    if (std::fabs(d[k]) < 1e-9f) {
      if (p[k] < lo[k] || p[k] > hi[k]) return false;
      continue;
    }
    float ta = (lo[k] - p[k]) / d[k], tb = (hi[k] - p[k]) / d[k];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta), t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  at = lerp(a, b, t0);
  return true;
}

// Cut direction codes (mapper::Note::dir) as a unit vector in the x-y plane; false for "any". `both`: a
// line, which is cut either way along it.
// dir code -> unit (x,y) in view plane (x right, y up). 0 up 1 down 2 left 3 right 4 UL 5 UR 6 DL 7 DR; 8 dot->false;
// 9 vertical 10 horizontal 11 '/' 12 '\\' -> both=true (axis, either sense). ref: mapper.h Note::dir, bot.h table, render noteCube.
bool dirVector(int dir, float &x, float &y, bool &both) {
  static const float v[13][2] = {{0, 1}, {0, -1}, {-1, 0}, {1, 0}, {-0.7071f, 0.7071f}, {0.7071f, 0.7071f}, {-0.7071f, -0.7071f}, {0.7071f, -0.7071f},
                                 {0, 0}, {0, 1}, {1, 0}, {0.7071f, 0.7071f}, {-0.7071f, 0.7071f}};
  if (dir < 0 || dir > 12 || dir == 8) return false;
  x = v[dir][0], y = v[dir][1], both = dir >= 9;
  return true;
}

// How far the saber moved between two samples, as the angle a blade of its own length would sweep:
// whichever is larger of its turn and its tip's travel.
// swept angle (rad) between 2 poses = max(blade turn, tip travel / blade length(min .3)). feeds pre/post score.
float swing(const SaberPose &a, const SaberPose &b) {
  const Vec3 da = sub(a.tip, a.base), db = sub(b.tip, b.base);
  const float la = length(da), lb = length(db);
  if (la < 1e-4f || lb < 1e-4f) return 0;
  const float turn = std::acos(std::clamp(dot(da, db) / (la * lb), -1.0f, 1.0f));
  return std::max(turn, length(sub(b.tip, a.tip)) / std::max(lb, 0.3f));
}

// full pre-swing score at 1.75 rad swept in kPreWindow; full post at 1.05 rad in kFollowThrough. U: rad, s.
constexpr float kPreFull = 1.75f, kPostFull = 1.05f;  // 100 and 60 degrees
constexpr double kHistory = 0.6, kFollowThrough = 0.3, kPreWindow = 0.25;

}  // namespace

// combo -> multiplier. tests pin table (game_test #6). maxScore_ uses same ramp => accuracy% is vs a perfect run.
int Session::multiplierFor(int combo) { return combo >= 14 ? 8 : combo >= 6 ? 4 : combo >= 2 ? 2 : 1; }

// copies+time-sorts notes (stable). maxScore_ = sum 115*multiplier(running combo): denominator of accuracy%.
Session::Session(const std::vector<mapper::Note> &notes, const Config &cfg) : notes_(notes), cfg_(cfg) {
  std::stable_sort(notes_.begin(), notes_.end(), [](const mapper::Note &a, const mapper::Note &b) { return a.time < b.time; });
  state_.assign(notes_.size(), NoteState::Pending);
  int combo = 0;
  for (size_t i = 0; i < notes_.size(); i++) maxScore_ += 115L * multiplierFor(++combo);
}

// note i position at song time `now`: x=(lane-1.5)*laneW, y=row0+row*rowH, z=hitZ-(time-now)*noteSpeed. CS scene.
Vec3 Session::notePos(size_t i, double now) const {
  const mapper::Note &n = notes_[i];
  return {(n.lane - 1.5f) * cfg_.laneW, cfg_.row0 + n.row * cfg_.rowH, cfg_.hitZ - (float)(n.time - now) * cfg_.noteSpeed};
}

// drains Hit/BadCut/Miss/ScoreFinal events since last call. T: same thread as update().
std::vector<Event> Session::takeEvents() {
  std::vector<Event> e;
  e.swap(events_);
  return e;
}

// seek/restart: zero stats, clear history/events; notes older than now-missAfter marked Missed (not counted).
void Session::restart(double now) {
  stats_ = Stats();
  maxCombo_ = 0;
  pending_.clear();
  events_.clear();
  for (auto &h : history_) h.clear();
  last_ = -1e9;
  first_ = 0;
  for (size_t i = 0; i < notes_.size(); i++) state_[i] = notes_[i].time < now - cfg_.missAfter ? NoteState::Missed : NoteState::Pending;
}

// combo=0, multiplier=1.
void Session::loseCombo() {
  stats_.combo = 0;
  stats_.multiplier = 1;
}

// final state for note i. Hit: combo++, +0.01 energy. BadCut/Missed: combo=0 mult=1, -0.1 energy, failed only if !cfg.noFail
// (default noFail=true). always: maxCombo_/maxScoreSoFar_ advance (every resolved note counts toward the ideal).
void Session::resolve(size_t i, NoteState s, double now, int color, Vec3 pos) {
  state_[i] = s;
  maxCombo_++;
  stats_.maxScoreSoFar += 115L * multiplierFor(maxCombo_);
  Event e{Event::Hit, (int)i, now, pos, color, 0, stats_.multiplier};
  if (s == NoteState::Hit) {
    stats_.hits++;
    stats_.combo++;
    stats_.maxCombo = std::max(stats_.maxCombo, stats_.combo);
    stats_.multiplier = multiplierFor(stats_.combo);
    stats_.energy = std::min(1.0f, stats_.energy + 0.01f);
    e.multiplier = stats_.multiplier;
  } else {
    if (s == NoteState::BadCut) stats_.badCuts++, e.kind = Event::BadCut;
    else stats_.misses++, e.kind = Event::Miss;
    loseCombo();
    stats_.energy -= 0.1f;
    if (stats_.energy <= 0 && !cfg_.noFail) stats_.failed = true;
    stats_.energy = std::max(stats_.energy, 0.0f);
  }
  events_.push_back(e);
}

// sum of swing() over saber c's history samples at t>=t0 (the pre-swing).
float Session::sweptSince(int c, double t0) const {
  const auto &h = history_[c];
  float total = 0;
  for (size_t i = 1; i < h.size(); i++)
    if (h[i].t >= t0) total += swing(h[i - 1].p, h[i].p);
  return total;
}

// per-frame step. pre: now = song time s (audio clock, not frame time); saber[c] c=0 left/red, 1 right/blue, scene CS.
// seek (now<last-0.05) or gap>0.5s => history reset. speed gate: tip speed over ~50 ms >= cfg.minSwingSpeed (0.7x for the
// direction check) except touchCuts dots. ! order matters: BadCut test precedes score; Hit pushes Pending for follow-through.
void Session::update(double now, const SaberPose saber[2]) {
  if (now < last_ - 0.05 || now - last_ > 0.5) {
    for (auto &h : history_) h.clear();
    pending_.clear();
  }
  last_ = now;
  for (int c = 0; c < 2; c++) {
    auto &h = history_[c];
    h.push_back({now, saber[c]});
    while (h.size() > 2 && h.front().t < now - kHistory) h.erase(h.begin());
  }

  // Tip velocity over the last ~50 ms, per saber.
  Vec3 vel[2];
  for (int c = 0; c < 2; c++) {
    const auto &h = history_[c];
    size_t from = h.size() - 1;
    while (from > 0 && h.back().t - h[from].t < 0.05) from--;
    const double dt = h.back().t - h[from].t;
    if (dt > 1e-4) {
      const Vec3 d = sub(h.back().p.tip, h[from].p.tip);
      vel[c] = {(float)(d.x / dt), (float)(d.y / dt), (float)(d.z / dt)};
    }
  }

  // Follow-through of cuts already made.
  for (size_t k = 0; k < pending_.size();) {
    Pending &p = pending_[k];
    const auto &h = history_[p.color];
    if (h.size() >= 2) p.swept += swing(h[h.size() - 2].p, h.back().p);
    if (now - p.t >= kFollowThrough) {
      const float post = std::clamp(p.swept / kPostFull, 0.0f, 1.0f) * 30;
      const int points = (int)std::lround((p.pre + post + p.acc) * p.multiplier * p.scale);
      stats_.score += points;
      events_.push_back({Event::ScoreFinal, p.note, now, p.pos, p.color, points, p.multiplier});
      pending_.erase(pending_.begin() + k);
    } else {
      k++;
    }
  }

  while (first_ < notes_.size() && state_[first_] != NoteState::Pending) first_++;
  for (size_t i = first_; i < notes_.size() && notes_[i].time <= now + cfg_.swingWindow; i++) {
    if (state_[i] != NoteState::Pending) continue;
    const mapper::Note &n = notes_[i];
    if (now - n.time > cfg_.missAfter) {
      resolve(i, NoteState::Missed, now, n.color, notePos(i, now));
      continue;
    }
    const Vec3 centre = notePos(i, now);
    for (int c = 0; c < 2 && state_[i] == NoteState::Pending; c++) {
      const auto &h = history_[c];
      const float speed = length(vel[c]);
      const bool touchOk = cfg_.touchCuts && n.dir == 8 && n.color == c;
      if (h.size() < 2 || (speed < cfg_.minSwingSpeed && !touchOk)) continue;
      // The blade at now and a little before it, so a fast swing that passed through between two frames is seen.
      const SaberPose &a = h[h.size() - 2].p, &b = h.back().p;
      bool touched = false;
      Vec3 at;
      for (int s = 4; s >= 0 && !touched; s--) {
        const float u = s / 4.0f;
        const Vec3 base = lerp(a.base, b.base, u), tip = lerp(a.tip, b.tip, u);
        touched = segmentBox(base, tip, centre, cfg_.noteSize * 0.5f + 0.02f, at);
      }
      if (!touched) continue;
      const float dx = at.x - centre.x, dy = at.y - centre.y;
      float rx, ry;
      bool good = n.color == c, both = false;
      float scale = 1;
      if (good && dirVector(n.dir, rx, ry, both)) {
        const float vx = vel[c].x, vy = vel[c].y, vl = std::sqrt(vx * vx + vy * vy);
        const float along = (vx * rx + vy * ry) / vl;
        good = vl >= cfg_.minSwingSpeed * 0.7f;
        // Close enough: a cardinal arrow takes a cut nearer to it than to the next cardinal (45 degrees either side),
        // a diagonal one a cut nearer to it than to horizontal or vertical (22.5 degrees either side).
        const float need = (n.dir >= 4 && n.dir <= 7) || n.dir >= 11 ? 0.92f : 0.70f;
        if (both) {
          // A line takes a cut either way along it, or across it for half the points; the combo holds.
          if (std::fabs(along) < need) scale = 0.5f;
        } else {
          good = good && along >= need;
        }
      }
      if (!good) {
        resolve(i, NoteState::BadCut, now, c, centre);
        continue;
      }
      const float pre = std::clamp(sweptSince(c, now - kPreWindow) / kPreFull, 0.0f, 1.0f) * 70;
      const float acc = 15 * (1 - std::clamp(std::sqrt(dx * dx + dy * dy) / 0.2f, 0.0f, 1.0f));
      resolve(i, NoteState::Hit, now, c, centre);
      pending_.push_back({(int)i, now, pre, acc, scale, 0, stats_.multiplier, centre, c});
    }
  }
}

}  // namespace game
