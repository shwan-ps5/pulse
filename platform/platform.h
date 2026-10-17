#pragma once

// The window, GL context and clock. platform_sdl.cpp is the desktop build (SDL2 + desktop OpenGL 4.6);
// platform_ps5.cpp opens a fullscreen EGL surface with the ps5-opengl SDK. The renderer and the game
// see only this, so they build unchanged for both.

#include <cstdint>

namespace platform {

enum : uint32_t {
  BTN_L3 = 0x2, BTN_R3 = 0x4, BTN_OPTIONS = 0x8, BTN_UP = 0x10, BTN_RIGHT = 0x20, BTN_DOWN = 0x40, BTN_LEFT = 0x80,
  BTN_L1 = 0x400, BTN_R1 = 0x800, BTN_TRIANGLE = 0x1000, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000,
  BTN_SQUARE = 0x8000, BTN_L2 = 0x100, BTN_R2 = 0x200, BTN_TOUCHPAD = 0x100000, BTN_CREATE = 0x1,
};

// hidden: no window on screen (headless snapshots on the desktop; ignored on the console).
bool init(const char *title, int width, int height, bool hidden);
// Returns false once the user has asked to quit.
bool poll();
void swap();
void shutdown();
int width();
int height();
double seconds();
void sleepMs(int ms);
// DualSense buttons held now, as PS button bits (BTN_*); the desktop maps keys onto them.
uint32_t buttons();
// Buttons, sticks (-1..1, right and down positive) and triggers (0..1).
struct Pad {
  uint32_t buttons;
  float lx, ly, rx, ry, l2, r2;
};
Pad pad();
// The signed-in user the controller and headset belong to (0 on the desktop).
int user();
// Starts sound output: fill(out, frames) is called from an audio thread for interleaved stereo floats.
bool startAudio(void (*fill)(float *out, int frames), int rate);
// The signed-in player's name (the console's user; the desktop's login).
const char *playerName();
// A system notification on the console; the log on the desktop.
void notify(const char *message);
// Desktop: the next key pressed since the last call as its SDL keycode (ASCII for letters and digits,
// lower case; arrows are SDLK_* values), 0x20000000 added with shift, 0 when none. Always 0 on the console.
int nextKey();
// Desktop: the mouse in -1..1 (right and up positive) and its buttons (bit 0 left, 1 right); false on the console.
bool mouse(float &x, float &y, int &buttons);
// Desktop window title; ignored on the console.
void setTitle(const char *title);
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace platform
