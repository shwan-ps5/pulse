#include "platform.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <ps5_opengl_display_modes.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <ctime>

// The console: one fullscreen EGL surface from ps5-opengl. Its runtime_shims.c sends stdout and
// stderr to /download0/ps5-opengl.log, and a system notification shows what failed before that.

extern "C" {
struct NotificationRequest {
  unsigned char reserved[45];
  char message[3075];
};
int sceKernelSendNotificationRequest(int device, NotificationRequest *req, size_t size, int blocking);
int sceSystemServiceLoadExec(const char *path, const char **args);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetLoginUserIdList(int *users);
int sceUserServiceGetUserName(int user, char *name, size_t size);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *param);
// ScePadData as PS5VR reads it: the button bits come first.
struct PadData {
  uint32_t buttons;
  uint8_t sticks[4], analog[4];
  float orientation[4], acceleration[3], angularVelocity[3];
  uint8_t touch[24], rest[72];
};
int scePadReadState(int handle, PadData *data);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *data);
}

namespace platform {

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static EGLint w, h;
static int signedIn, padHandle = -1;

void notify(const char *message) {
  static NotificationRequest req;
  std::strncpy(req.message, message, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  std::fprintf(stderr, "[notification] %s\n", message);
}

bool init(const char *title, int width, int height, bool hidden) {
  (void)title, (void)hidden;
  static const EGLint config[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
                                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
  static const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
  sceUserServiceInitialize(nullptr);
  int users[4] = {0};
  sceUserServiceGetLoginUserIdList(users);
  signedIn = users[0];
  scePadInit();
  padHandle = scePadOpen(signedIn, 0, 0, nullptr);
  EGLConfig cfg = nullptr;
  EGLint count = 0;
  display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  // The display mode is fixed before EGL starts; the PS5 scales it to whatever the TV takes.
  eglSetDisplayModePS5(display, width, height);
  eglSetDisplayRefreshPS5(display, 60);
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API) ||
      !eglChooseConfig(display, config, &cfg, 1, &count) || count != 1) {
    notify("Pulse: EGL display failed");
    return false;
  }
  // ps5-opengl's window surface takes native handle 0 and no attributes.
  surface = eglCreateWindowSurface(display, cfg, (EGLNativeWindowType)0, nullptr);
  context = eglCreateContext(display, cfg, EGL_NO_CONTEXT, attribs);
  if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context) ||
      !eglQuerySurface(display, surface, EGL_WIDTH, &w) || !eglQuerySurface(display, surface, EGL_HEIGHT, &h)) {
    notify("Pulse: OpenGL 4.6 context failed");
    return false;
  }
  eglSwapInterval(display, 1);
  log("EGL surface %dx%d", (int)w, (int)h);
  return true;
}

// The PS button belongs to the system, so a title quits itself on Create + Options held for a second.
bool poll() {
  static double since = 0;
  const uint32_t both = BTN_CREATE | BTN_OPTIONS;
  if ((pad().buttons & both) != both) return since = 0, true;
  if (since == 0) since = seconds();
  return seconds() - since < 1.0;
}

int nextKey() { return 0; }
bool mouse(float &, float &, int &) { return false; }
void setTitle(const char *) {}

void swap() { eglSwapBuffers(display, surface); }

// A native title that returns from main or calls exit() is killed with a crash report; asking the system
// to end it is the clean way out.
void shutdown() {
  if (display != EGL_NO_DISPLAY) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    eglTerminate(display);
  }
  log("exit: asking the system to close the title");
  const int refused = sceSystemServiceLoadExec("exit", nullptr);
  log("exit: refused rc=0x%x", (unsigned)refused);
  for (;;) sceKernelUsleep(100000);
}

uint32_t buttons() { return pad().buttons; }

Pad pad() {
  PadData d;
  Pad p{};
  if (padHandle < 0 || scePadReadState(padHandle, &d) != 0) return p;
  auto stick = [](uint8_t v) { return (v - 127.5f) / 127.5f; };
  p.buttons = d.buttons;
  p.lx = stick(d.sticks[0]), p.ly = stick(d.sticks[1]), p.rx = stick(d.sticks[2]), p.ry = stick(d.sticks[3]);
  p.l2 = d.analog[0] / 255.0f, p.r2 = d.analog[1] / 255.0f;
  return p;
}

int user() { return signedIn; }

const char *playerName() {
  static char name[32];
  if (!name[0] && sceUserServiceGetUserName(signedIn, name, sizeof name) != 0) std::strcpy(name, "PLAYER");
  return name;
}

// sceAudioOut as PS5VR opens it: the system user's main port, 48 kHz float stereo, and an output call
// that blocks until the port wants the next block, which paces the audio thread.
namespace {
void (*audioFill)(float *, int);
int audioPort = -1;
constexpr int AUDIO_BLOCK = 256;

void *audioThread(void *) {
  static float block[AUDIO_BLOCK * 2];
  for (;;) {
    audioFill(block, AUDIO_BLOCK);
    sceAudioOutOutput(audioPort, block);
  }
  return nullptr;
}
}  // namespace

bool startAudio(void (*fill)(float *out, int frames), int rate) {
  sceAudioOutInit();
  audioPort = sceAudioOutOpen(0xFF, 0, 0, AUDIO_BLOCK, (unsigned)rate, 4 /* FLOAT_STEREO */);
  if (audioPort < 1) {
    log("audio: sceAudioOutOpen failed: 0x%08x", (unsigned)audioPort);
    return false;
  }
  audioFill = fill;
  pthread_t t;
  if (pthread_create(&t, nullptr, audioThread, nullptr) != 0) {
    log("audio: thread failed");
    return false;
  }
  log("audio: port %d, %d Hz float stereo", audioPort, rate);
  return true;
}

int width() { return w; }
int height() { return h; }

double seconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void sleepMs(int ms) {
  if (ms > 0) usleep((useconds_t)ms * 1000);
}

// Every line also goes out as a UDP datagram to the PC (tools/logsrv.py), so the title can be watched
// live: the host is loghost.txt in the title's /download0 (by FTP) or the one packaged at build time.
namespace {
int logSock = -2;
sockaddr_in logDest;

void logOut(const char *line) {
  if (logSock == -2) {
    logSock = -1;
    char host[64] = {};
    for (const char *path : {"/download0/loghost.txt", "/app0/assets/loghost.txt"})
      if (FILE *f = std::fopen(path, "r")) {
        const bool got = std::fgets(host, sizeof host, f) != nullptr;
        std::fclose(f);
        if (got && host[0] > ' ') break;
        host[0] = 0;
      }
    host[std::strcspn(host, " \r\n")] = 0;
    std::memset(&logDest, 0, sizeof logDest);
    logDest.sin_family = AF_INET;
    logDest.sin_port = htons(9023);
    if (host[0] && inet_pton(AF_INET, host, &logDest.sin_addr) == 1) logSock = socket(AF_INET, SOCK_DGRAM, 0);
  }
  if (logSock >= 0) sendto(logSock, line, std::strlen(line), 0, (const sockaddr *)&logDest, sizeof logDest);
}
}  // namespace

void log(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "%s\n", line);
  logOut(line);
  // Until the picture is up, the log file is out of reach on the console; errors go to a pop-up too.
  if (std::strstr(line, "error") || std::strstr(line, "failed") || std::strstr(line, "no such")) notify(line);
}

}  // namespace platform
