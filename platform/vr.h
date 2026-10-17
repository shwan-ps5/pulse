#pragma once

// The headset. vr_ps5.cpp drives PS VR2 through Hmd2 and VrTracker2, after PS5VR
// (github.com/theghostonline/PS5VR, GPL-3.0); vr_desktop.cpp simulates a slowly turning head so the
// rest of the test runs on a PC.
#include <cstdint>

namespace vr {

struct Eye {
  float pos[3];   // metres, tracker space (y up, -z forward)
  float quat[4];  // x, y, z, w
  float tan[4];   // frustum half-angle tangents: left, right, up, down
};

struct Frame {
  Eye eye[2];
  int buffer;     // which registered eye buffer to fill
  bool tracked;
};

// Where the eye buffers live and how they are laid out: both eyes side by side, width = 2 x eye.
struct Targets {
  uint64_t address[3];  // GPU addresses, 64 KiB aligned
  int width, height;
  int pitch;            // texels per row when linear
  bool tiled;           // the GPU's 64 KiB-block layout (SW_64KB_R_X) instead of linear rows
};

constexpr int kBuffers = 3;

// A PS VR2 Sense controller: pose in the same tracker space as the eyes (the headset's cameras track
// it), the pad state beside it. buttons use the platform BTN_ bits: left hand L2 trigger, L1 grip, L3, Square, Triangle; right hand R2, R1, R3, Options, Circle, Cross.
struct Hand {
  bool tracked, connected;
  float pos[3], quat[4];
  uint32_t buttons;
  float stick[2], trigger;
  uint8_t raw[0x100];
};

// Loads the system modules, opens the headset, tracking and reprojection. False if there is no
// headset (or this is not a PS5); the test then shows only the TV view.
bool init(int user);
// Registers the eye buffers with the headset's video port and enters VR mode.
bool start(const Targets &t);
void stop();
bool active();
// Waits for the next 60 Hz frame slot: in VR the title stops presenting to the TV, whose vsync
// paced it until then.
void waitSlot();
// Begins a frame paced by the headset; false when it can't be drawn now.
bool begin(Frame &f);
// Hands the filled buffer to the headset. Call after the GPU has finished writing it.
void submit();
// Side 0 = left, 1 = right. Call after begin() so the pose is the one predicted for this frame.
// False if the controller can't be opened at all.
bool hand(int side, Hand &h);
// Desktop simulation only: where the hands are (tracker space). A no-op on the console.
void simulateHand(int side, const float pos[3], const float quat[4]);
// Makes the current head position and heading the origin of the scene.
void recenter();
// The pose recentre captured: position and the yaw-only heading.
void origin(float pos[3], float yaw[4]);
// The last step's outcome, for the log and notifications.
const char *status();
// The headset's rumble, 0 (off) to 1.
void vibrate(float strength);
// A short buzz in one Sense controller (0 left, 1 right), strength 0..1; it stops by itself.
void haptic(int side, float strength);
// Desktop only: the simulated head's roll (radians, right ear down positive) and height (metres
// from where it was centred), for steering without a headset.
void simulate(float roll, float dy);

}  // namespace vr
