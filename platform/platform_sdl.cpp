#include "platform.h"

#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace platform {

static SDL_Window *window;
static SDL_GLContext context;
static int w, h;
static SDL_GameController *controller;
static std::deque<int> keyQueue;

bool init(const char *title, int width, int height, bool hidden) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    log("SDL_Init: %s", SDL_GetError());
    return false;
  }
  // The console's ps5-opengl is a 4.6 core context, so the desktop build asks for the same.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  Uint32 flags = SDL_WINDOW_OPENGL | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
  window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
  if (!window || !(context = SDL_GL_CreateContext(window))) {
    log("OpenGL 4.6 core context: %s", SDL_GetError());
    return false;
  }
  SDL_GL_SetSwapInterval(1);
  SDL_GL_GetDrawableSize(window, &w, &h);
  return true;
}

bool poll() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) return false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
    if (e.type == SDL_KEYDOWN) keyQueue.push_back(e.key.keysym.sym | ((e.key.keysym.mod & KMOD_SHIFT) ? 0x20000000 : 0));
    if (e.type == SDL_CONTROLLERDEVICEADDED && !controller) controller = SDL_GameControllerOpen(e.cdevice.which);
  }
  SDL_GL_GetDrawableSize(window, &w, &h);
  return true;
}

void swap() { SDL_GL_SwapWindow(window); }

int nextKey() {
  if (keyQueue.empty()) return 0;
  const int k = keyQueue.front();
  keyQueue.pop_front();
  return k;
}

bool mouse(float &x, float &y, int &buttons) {
  int mx, my;
  const Uint32 b = SDL_GetMouseState(&mx, &my);
  int ww, wh;
  SDL_GetWindowSize(window, &ww, &wh);
  x = 2.0f * mx / std::max(ww, 1) - 1, y = 1 - 2.0f * my / std::max(wh, 1);
  buttons = (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0);
  return true;
}

void setTitle(const char *title) { SDL_SetWindowTitle(window, title); }

void shutdown() {
  if (context) SDL_GL_DeleteContext(context);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
}

uint32_t buttons() {
  const Uint8 *k = SDL_GetKeyboardState(nullptr);
  const struct { SDL_Scancode key; uint32_t bit; } map[] = {
      {SDL_SCANCODE_SPACE, BTN_CROSS}, {SDL_SCANCODE_C, BTN_CIRCLE}, {SDL_SCANCODE_S, BTN_SQUARE},
      {SDL_SCANCODE_T, BTN_TRIANGLE}, {SDL_SCANCODE_R, BTN_R3}, {SDL_SCANCODE_UP, BTN_UP},
      {SDL_SCANCODE_DOWN, BTN_DOWN}, {SDL_SCANCODE_LEFT, BTN_LEFT}, {SDL_SCANCODE_RIGHT, BTN_RIGHT},
      {SDL_SCANCODE_LEFTBRACKET, BTN_L1}, {SDL_SCANCODE_RIGHTBRACKET, BTN_R1}, {SDL_SCANCODE_P, BTN_OPTIONS}, {SDL_SCANCODE_BACKSPACE, BTN_CREATE},
  };
  uint32_t b = 0;
  for (auto &m : map)
    if (k[m.key]) b |= m.bit;
  return b;
}

// The keyboard stands in for the pad (arrows: left stick, W/S: right stick, left Ctrl or Enter: R2, left
// Shift: L2); a connected game controller is read as a DualSense.
Pad pad() {
  const Uint8 *k = SDL_GetKeyboardState(nullptr);
  Pad p{};
  p.buttons = buttons();
  p.lx = (float)(k[SDL_SCANCODE_RIGHT] - k[SDL_SCANCODE_LEFT]);
  p.ly = (float)(k[SDL_SCANCODE_DOWN] - k[SDL_SCANCODE_UP]);
  p.ry = (float)(k[SDL_SCANCODE_S] - k[SDL_SCANCODE_W]);
  p.rx = (float)(k[SDL_SCANCODE_E] - k[SDL_SCANCODE_Q]);
  p.r2 = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RETURN];
  p.l2 = k[SDL_SCANCODE_LSHIFT];
  if (!controller) return p;
  auto axis = [](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(controller, a) / 32767.0f; };
  const struct { SDL_GameControllerButton b; uint32_t bit; } map[] = {
      {SDL_CONTROLLER_BUTTON_A, BTN_CROSS}, {SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE}, {SDL_CONTROLLER_BUTTON_X, BTN_SQUARE},
      {SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L1},
      {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R1}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, BTN_L3},
      {SDL_CONTROLLER_BUTTON_RIGHTSTICK, BTN_R3}, {SDL_CONTROLLER_BUTTON_START, BTN_OPTIONS},
      {SDL_CONTROLLER_BUTTON_BACK, BTN_CREATE}, {SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP},
      {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT},
      {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT}, {SDL_CONTROLLER_BUTTON_TOUCHPAD, BTN_TOUCHPAD},
  };
  for (auto &m : map)
    if (SDL_GameControllerGetButton(controller, m.b)) p.buttons |= m.bit;
  p.lx += axis(SDL_CONTROLLER_AXIS_LEFTX), p.ly += axis(SDL_CONTROLLER_AXIS_LEFTY);
  p.rx += axis(SDL_CONTROLLER_AXIS_RIGHTX), p.ry += axis(SDL_CONTROLLER_AXIS_RIGHTY);
  p.l2 += axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), p.r2 += axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  return p;
}

int user() { return 0; }

const char *playerName() {
  const char *n = std::getenv("USER");
  return n && *n ? n : "PLAYER";
}

static void (*audioFill)(float *, int);

bool startAudio(void (*fill)(float *out, int frames), int rate) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
  audioFill = fill;
  SDL_AudioSpec want{}, have{};
  want.freq = rate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = [](void *, Uint8 *stream, int bytes) { audioFill(reinterpret_cast<float *>(stream), bytes / 8); };
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev) {
    log("audio: %s", SDL_GetError());
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  return true;
}
void notify(const char *message) { log("[notification] %s", message); }

int width() { return w; }
int height() { return h; }
double seconds() { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }
void sleepMs(int ms) { SDL_Delay((Uint32)std::max(0, ms)); }

void log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

}  // namespace platform
