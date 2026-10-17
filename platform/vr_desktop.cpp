#include <cmath>
#include <cstring>

#include "vmath.h"
#include "vr.h"

// No headset on a PC: a head 1.6 m up whose roll and height the game sets (simulate), with PS VR2-like
// field-of-view tangents, so the head steering, the stereo rendering and the buffer copy can be
// checked here.

namespace vr {

namespace {
bool started;
int buffer;
float roll, dy;
float originPos[3] = {0, 1.6f, 0}, originYaw[4] = {0, 0, 0, 1};
const float kIpd = 0.064f;
}  // namespace

bool init(int) { return true; }
bool start(const Targets &) { return started = true; }
void stop() { started = false; }
bool active() { return started; }
void waitSlot() {}
void simulate(float r, float d) { roll = r, dy = d; }
void vibrate(float) {}

bool begin(Frame &f) {
  // Right ear down: a turn about the view axis (-z), so about +z by -roll.
  const float q[4] = {0, 0, std::sin(-roll * 0.5f), std::cos(-roll * 0.5f)};
  for (int e = 0; e < 2; e++) {
    const float side[3] = {e ? kIpd / 2 : -kIpd / 2, 0, 0};
    float off[3];
    rotate(q, side, off);
    for (int i = 0; i < 3; i++) f.eye[e].pos[i] = originPos[i] + off[i];
    f.eye[e].pos[1] += dy;
    for (int i = 0; i < 4; i++) f.eye[e].quat[i] = q[i];
    // PS VR2 is wider outwards than inwards for each eye.
    const float out = 1.15f, in = 0.95f, ud = 1.1f;
    f.eye[e].tan[0] = e ? in : out;
    f.eye[e].tan[1] = e ? out : in;
    f.eye[e].tan[2] = f.eye[e].tan[3] = ud;
  }
  f.buffer = buffer;
  f.tracked = true;
  return true;
}

void submit() { buffer = (buffer + 1) % kBuffers; }
void recenter() {}
void origin(float pos[3], float yaw[4]) {
  for (int i = 0; i < 3; i++) pos[i] = originPos[i];
  for (int i = 0; i < 4; i++) yaw[i] = originYaw[i];
}
const char *status() { return "simulated headset"; }

// The hands are wherever the game says (a bot, in tests); until then, resting at the sides.
namespace {
float handPos[2][3] = {{-0.25f, 1.0f, -0.35f}, {0.25f, 1.0f, -0.35f}}, handQuat[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
}
void haptic(int, float) {}
void simulateHand(int side, const float pos[3], const float quat[4]) {
  std::memcpy(handPos[side], pos, 12);
  std::memcpy(handQuat[side], quat, 16);
}
bool hand(int side, Hand &h) {
  std::memset(&h, 0, sizeof h);
  h.tracked = h.connected = true;
  std::memcpy(h.pos, handPos[side], 12);
  std::memcpy(h.quat, handQuat[side], 16);
  return true;
}

}  // namespace vr
