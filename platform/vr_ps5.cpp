// PS VR2 through the system's Hmd2 and VrTracker2 libraries. A port of PS5VR's app/src/app_vr.c
// (github.com/theghostonline/PS5VR, Copyright the PS5VR author, GPL-3.0-or-later), cut down to what
// a game needs: the 13.60 export table, headset and tracker setup, reprojection at 120 Hz with
// half-rate (60 fps) rendering, and one side-by-side eye buffer per frame described by a GPU texture
// descriptor. Changes from PS5VR: eye positions for 3D scenes (it renders video at infinity), and
// eye buffers that may be linear or tiled, at addresses the caller provides.
//
// The life of a VR title, in the order a game calls it (see vr.h for the API and game/main.cpp for a user):
//   init(user)      once: load the libraries, open the headset and tracker, set up reprojection
//   start(targets)  once the eye buffers exist: enter VR mode at 120 Hz
//   each frame:     waitSlot() paces to the headset's 60 Hz render rate; begin(frame) gives the eye poses;
//                   draw both eyes; hand(side, h) reads the controllers; submit() sends the picture
//   stop()          on exit
// The headset takes a picture rendered at the 60 Hz slot and re-warps it to the newest head pose twice per frame, so
// small delays in drawing do not show as lag, but a missed slot does. Everything here talks to private system
// libraries by structure layouts reverse-engineered by the PS5VR project; the offsets are firmware 13.60 only.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "platform.h"
#include "vmath.h"
#include "vr.h"

extern "C" {
int sceSysmoduleLoadModule(uint32_t id);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset, size_t align);
int sceKernelUsleep(unsigned usec);
uint64_t sceKernelGetProcessTime(void);
int sceVideoOutOpen(int user_id, int bus_type, int index, const void *param);
int sceVideoOutClose(int handle);
void sceVideoOutSetBufferAttribute2(void *attr, uint64_t format, uint32_t tiling, uint32_t width, uint32_t height,
                                    uint64_t option, uint32_t reserved0, uint64_t reserved1);
int sceVideoOutRegisterBuffers2(int handle, int set_index, int start_index, void *buffers, int count, void *attr,
                                int category, void *option);
int sceVideoOutUnregisterBuffers(int handle, int set_index);
int sceVideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
int sceVideoOutConfigureOutput(int handle, uint64_t mode, const void *options, const void *r0, uint64_t r1);

typedef struct {
  uint64_t size;
  char name[256];
  struct {
    uint64_t addr;
    uint32_t size;
    int32_t prot;
  } seg[4];
  uint32_t nseg;
  uint8_t fingerprint[20];
} module_info_t;
int sceKernelGetModuleInfo(int handle, module_info_t *info);
int scePadOpen(int user, int type, int index, const void *param);
int scePadReadState(int handle, void *data);
struct PadVibration {
  uint8_t largeMotor, smallMotor;
};
int scePadSetVibration(int handle, const PadVibration *param);
}

namespace vr {

namespace {

struct SizeAlign {
  uint64_t size, align;
};

#define HMD2_FUNCS(X)                                                                                        \
  X(int, sceHmd2Initialize, (const void *param))                                                            \
  X(int, sceHmd2Open, (int user_id, int type, int index, const void *param))                                \
  X(int, sceHmd2GetDeviceInformation, (void *info))                                                         \
  X(int, sceHmd2GetFieldOfViewWithoutHandle, (void *fov))                                                   \
  X(SizeAlign, sceHmd2ReprojectionQueryBufferSizeAlign, (void))                                             \
  X(SizeAlign, sceHmd2ReprojectionQueryDisplayBufferSizeAlign, (void))                                      \
  X(int, sceHmd2ReprojectionInitialize, (const void *param, const void *reserved))                          \
  X(int, sceHmd2ReprojectionEnableVrMode, (uint64_t mode))                                                  \
  X(int, sceHmd2ReprojectionDisableVrMode, (void))                                                          \
  X(int, sceHmd2ReprojectionBeginFrame, (uint64_t frame))                                                   \
  X(int, sceHmd2ReprojectionGetPredictedDisplayTime, (uint64_t frame, uint64_t *time))                      \
  X(int, sceHmd2ReprojectionSetParam, (uint32_t buffer, uint64_t frame, const void *param, const void *trace)) \
  X(int, sceHmd2ReprojectionSetRenderConfig, (uint64_t half_rate, uint64_t r0, uint64_t r1))                \
  X(int, sceHmd2ReprojectionTerminate, (void))                                                              \
  X(SizeAlign, sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign, (void))                                  \
  X(SizeAlign, sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign, (void))                               \
  X(int, sceHmd2ReprojectionSetMirroringOption, (const void *opt))                                          \
  X(int, sceHmd2ReprojectionEnableMirroring, (const void *param))                                           \
  X(int, sceHmd2ReprojectionDisableMirroring, (void))                                                       \
  X(int, sceHmd2SetVibration, (int handle, int strength))                                                   \
  X(int, sceHmd2Close, (int handle))                                                                        \
  X(int, sceHmd2Terminate, (void))
#define VRTRACKER2_FUNCS(X)                                       \
  X(int, sceVrTracker2QueryMemory, (const void *param, void *out)) \
  X(int, sceVrTracker2Initialize, (const void *param))             \
  X(int, sceVrTracker2RegisterDevice, (int type, int handle))      \
  X(int, sceVrTracker2GetResult, (const void *param, void *result)) \
  X(int, sceVrTracker2UnregisterDevice, (int handle))              \
  X(int, sceVrTracker2Finalize, (void))
#define DECLARE_PTR(ret, name, args) ret(*name) args;
HMD2_FUNCS(DECLARE_PTR)
VRTRACKER2_FUNCS(DECLARE_PTR)

// Firmware 13.60 export offsets, from PS5VR's vr_exports_1360.h (tools/gen_export_table.py). An app
// may not dlsym these; they are used only after the module's name and an anchor string check out.
struct ExportSig {
  const char *name;
  uint32_t offset;
};
struct ExportAnchor {
  uint32_t va;
  const char *text;
};
const ExportSig kHmd2Exports[] = {
    {"sceHmd2Initialize", 0x2a670},
    {"sceHmd2Terminate", 0x2a710},
    {"sceHmd2Open", 0x2a740},
    {"sceHmd2Close", 0x2a8e0},
    {"sceHmd2GetDeviceInformation", 0x2ab90},
    {"sceHmd2GetFieldOfViewWithoutHandle", 0x27940},
    {"sceHmd2ReprojectionQueryBufferSizeAlign", 0x218c0},
    {"sceHmd2ReprojectionQueryDisplayBufferSizeAlign", 0x21940},
    {"sceHmd2ReprojectionInitialize", 0x21a00},
    {"sceHmd2ReprojectionTerminate", 0x21df0},
    {"sceHmd2ReprojectionEnableVrMode", 0x21f00},
    {"sceHmd2ReprojectionDisableVrMode", 0x21f90},
    {"sceHmd2ReprojectionBeginFrame", 0x220d0},
    {"sceHmd2ReprojectionGetPredictedDisplayTime", 0x220e0},
    {"sceHmd2ReprojectionSetRenderConfig", 0x22130},
    {"sceHmd2ReprojectionSetParam", 0x22110},
    {"sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign", 0x221b0},
    {"sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign", 0x221c0},
    {"sceHmd2ReprojectionEnableMirroring", 0x221d0},
    {"sceHmd2ReprojectionDisableMirroring", 0x22250},
    {"sceHmd2ReprojectionSetMirroringOption", 0x222c0},
    {"sceHmd2SetVibration", 0x2ae00},
};
const ExportAnchor kHmd2Anchor = {0x7c686, "Not Initialized"};
const ExportSig kVrTracker2Exports[] = {
    {"sceVrTracker2QueryMemory", 0xe0},
    {"sceVrTracker2Initialize", 0x230},
    {"sceVrTracker2Finalize", 0x1140},
    {"sceVrTracker2RegisterDevice", 0x11e0},
    {"sceVrTracker2UnregisterDevice", 0x13d0},
    {"sceVrTracker2GetResult", 0x2890},
};
const ExportAnchor kVrTracker2Anchor = {0x1225e5, "MININLIERS-FOR-MULTICAMSTRICTGRAVITYBOOT"};

struct Slot {
  const char *name;
  void **slot;
};
#define SLOT(ret, name, args) {#name, (void **)&name},
Slot kHmd2Slots[] = {HMD2_FUNCS(SLOT)};
Slot kVrTracker2Slots[] = {VRTRACKER2_FUNCS(SLOT)};
#define COUNT(a) (sizeof(a) / sizeof *(a))

constexpr uint32_t SYSMODULE_HMD2 = 0x12a, SYSMODULE_VRTRACKER2 = 0x12b;
constexpr uint64_t VR_MODE_120HZ = 0x2000f;
constexpr int VR_QUEUE_FULL = (int)0x8A72001A, VO_BUSY = (int)0x80290009;
// The margin the reprojection uses while the head moves between frames.
constexpr float OVERSCAN = 1.15f;
constexpr float IPD = 0.064f;

struct LayerSet {
  void *layers;
  uint32_t count, pad;
};
struct VoBuffer {
  void *data, *metadata, *reserved0, *reserved1;
};

struct State {
  bool loaded, failed, active, frameOpen;
  int hmd = -1, vh = -1, buf;
  float fov[4];  // out, in, top, bottom tangents
  uint8_t layers[kBuffers][0x130] __attribute__((aligned(16)));
  LayerSet sets[kBuffers];
  Targets targets;
  uint64_t frame, t;
  float pd[3], qd[4];
  float originPos[3], originYaw[4] = {0, 0, 0, 1};
  bool haveOrigin;
  int user;
  int padHandle[2] = {-1, -1};
  bool padRegistered[2], padOpened;
  unsigned submits, errors, trackedFrames;
  char status[160] = "not started";
  // Hmd2's own TV mirror (what the headset shows), and the 60 Hz schedule frames start on.
  void *mirrorWork, *mirrorDisplay;
  bool mirroring;
  uint64_t nextBegin;
} g;

void setStatus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Records the outcome of the latest step for status() and writes it to the log; init() and start() call it
// instead of failing silently, so the log says which step of bringing the headset up went wrong.
void setStatus(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g.status, sizeof g.status, fmt, ap);
  va_end(ap);
  platform::log("vr: %s", g.status);
}

// Loads one of the console's VR libraries twice over: sceSysmoduleLoadModule registers it with the system, and
// sceKernelLoadStartModule maps it from the system library folder (named by the sandbox's random word) to get a
// handle we can look functions up in. Returns that handle, negative on failure.
int loadSysmodule(uint32_t id, const char *name) {
  int rc = sceSysmoduleLoadModule(id);
  char path[128];
  std::snprintf(path, sizeof path, "/%s/common/lib/%s.sprx", sceKernelGetFsSandboxRandomWord(), name);
  int res = 0;
  int h = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &res);
  platform::log("vr: sysmodule 0x%x %s -> 0x%08x, handle 0x%x", id, name, rc, h);
  return h;
}

// The load address of module `h`, used only when its functions cannot be found by name. The module must be the
// expected file, and a known string must sit at its known offset in an executable segment; any mismatch returns 0,
// so a firmware with different offsets never has fixed addresses called blindly.
uintptr_t moduleBase(int h, const char *file, const ExportAnchor &anchor) {
  module_info_t mi;
  std::memset(&mi, 0, sizeof mi);
  mi.size = sizeof mi;
  if (sceKernelGetModuleInfo(h, &mi) != 0 || std::strcmp(mi.name, file) != 0) return 0;
  uintptr_t base = 0;
  for (unsigned i = 0; i < mi.nseg && i < 4; i++)
    if (!base || mi.seg[i].addr < base) base = mi.seg[i].addr;
  const uintptr_t want = base + anchor.va;
  const size_t len = std::strlen(anchor.text) + 1;
  for (unsigned i = 0; i < mi.nseg && i < 4; i++) {
    const uintptr_t lo = mi.seg[i].addr, hi = lo + mi.seg[i].size;
    if (want >= lo && want + len <= hi && (mi.seg[i].prot & 1))
      return std::memcmp((const void *)want, anchor.text, len) == 0 ? base : 0;
  }
  return 0;
}

// Fills each function pointer in `slots`: by name through sceKernelDlsym when the module exports it, otherwise at
// base + offset from `table` (the offsets are firmware 13.60 only). False if any function stays unresolved.
bool resolve(int h, const char *file, Slot *slots, size_t n, const ExportSig *table, size_t tableN,
             const ExportAnchor &anchor) {
  uintptr_t base = 0;
  bool triedBase = false, ok = true;
  for (size_t i = 0; i < n; i++) {
    if (sceKernelDlsym(h, slots[i].name, slots[i].slot) == 0 && *slots[i].slot) continue;
    if (!triedBase) {
      base = moduleBase(h, file, anchor);
      triedBase = true;
      platform::log("vr: %s by export offsets, base %p", file, (void *)base);
    }
    *slots[i].slot = nullptr;
    for (size_t j = 0; base && j < tableN; j++)
      if (!std::strcmp(table[j].name, slots[i].name)) *slots[i].slot = (void *)(base + table[j].offset);
    if (!*slots[i].slot) {
      platform::log("vr: missing %s", slots[i].name);
      ok = false;
    }
  }
  return ok;
}

// Memory the headset's libraries can use: CPU-and-GPU-visible direct memory, 64 KiB aligned and zeroed. These
// libraries refuse ordinary malloc memory. Returns null if the console has none to give.
void *directAlloc(size_t size, size_t align) {
  size = (size + 0xffff) & ~(size_t)0xffff;
  int64_t off = 0;
  void *va = nullptr;
  if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, size, align, 12 /* WB_ONION */, &off) != 0 ||
      sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align) != 0)
    return nullptr;
  std::memset(va, 0, size);
  return va;
}

// The tracker's pose at `time`: device position and orientation, and each eye's orientation.
bool pose(uint64_t time, float dp[3], float dq[4], float lq[4], float rq[4]) {
  static uint8_t res[0x400];
  uint8_t gp[0x20];
  std::memset(gp, 0, sizeof gp);
  *(uint32_t *)(gp + 0x00) = 0x20;
  *(int32_t *)(gp + 0x04) = g.hmd;
  *(uint64_t *)(gp + 0x10) = time;
  std::memset(res, 0, sizeof res);
  if (sceVrTracker2GetResult(gp, res) != 0) return false;
  std::memcpy(dp, res + 0x60, 12);
  std::memcpy(dq, res + 0x70, 16);
  if (lq) std::memcpy(lq, res + 0xb0, 16);
  if (rq) std::memcpy(rq, res + 0xf0, 16);
  return *(uint32_t *)(res + 4) != 0;
}

// A 2D image descriptor (T#), as PS5VR's eng_agc_writer.c builds them: RGBA8, base >> 8, the pitch in
// texels when linear, swizzle mode 27 (SW_64KB_R_X) when tiled.
bool buildTsharp(uint32_t out[8], uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch, bool tiled) {
  enum { FORMAT_8_8_8_8_UNORM = 56, SEL_X = 4, SEL_Y = 5, SEL_Z = 6, SEL_W = 7, RSRC_IMG_2D = 9, SW_64KB_R_X = 27 };
  if (!addr || (addr & (tiled ? 0xffffu : 0xffu)) || !width || !height || pitch < width) return false;
  std::memset(out, 0, 32);
  const uint32_t w1 = width - 1, p1 = pitch - 1;
  out[0] = (uint32_t)(addr >> 8);
  out[1] = (uint32_t)(addr >> 40) | (FORMAT_8_8_8_8_UNORM << 20) | ((w1 & 3u) << 30);
  out[2] = ((w1 >> 2) & 0xfffu) | ((height - 1u) << 14);
  out[3] = SEL_X | (SEL_Y << 3) | (SEL_Z << 6) | (SEL_W << 9) | ((uint32_t)RSRC_IMG_2D << 28);
  if (tiled) out[3] |= (uint32_t)SW_64KB_R_X << 20;
  else if (pitch != width) out[4] = (p1 & 0x1fffu) | (((p1 >> 13) & 1u) << 13);
  out[5] = 7u << 20;
  return true;
}

}  // namespace

// Opens the headset for `user`: loads the Hmd2 and VrTracker2 libraries, reads the field of view, opens the
// headset, starts the tracker (profile 0) and the reprojection that re-warps each frame to the latest head pose.
// Runs once; later calls return the first result. Each failure sets status() and returns false, in which case the
// game carries on with the flat TV view.
bool init(int user) {
  g.user = user;
  if (g.loaded) return true;
  if (g.failed) return false;
  g.failed = true;
  int h = loadSysmodule(SYSMODULE_HMD2, "libSceHmd2");
  if (h < 0 || !resolve(h, "libSceHmd2.sprx", kHmd2Slots, COUNT(kHmd2Slots), kHmd2Exports, COUNT(kHmd2Exports), kHmd2Anchor)) {
    setStatus("libSceHmd2 not available");
    return false;
  }
  h = loadSysmodule(SYSMODULE_VRTRACKER2, "libSceVrTracker2");
  if (h < 0 || !resolve(h, "libSceVrTracker2.sprx", kVrTracker2Slots, COUNT(kVrTracker2Slots), kVrTracker2Exports,
                        COUNT(kVrTracker2Exports), kVrTracker2Anchor)) {
    setStatus("libSceVrTracker2 not available");
    return false;
  }
  uint8_t initParam[16] = {0};
  int rc = sceHmd2Initialize(initParam);
  if (rc != 0) {
    setStatus("sceHmd2Initialize failed 0x%08x", rc);
    return false;
  }
  float fov[16] = {0};
  rc = sceHmd2GetFieldOfViewWithoutHandle(fov);
  std::memcpy(g.fov, fov, sizeof g.fov);
  platform::log("vr: FOV rc 0x%08x out %.4f in %.4f top %.4f bottom %.4f", rc, fov[0], fov[1], fov[2], fov[3]);
  if (rc != 0 || g.fov[0] <= 0 || g.fov[2] <= 0) {
    setStatus("no headset field of view (0x%08x): is PS VR2 connected?", rc);
    return false;
  }
  g.hmd = sceHmd2Open(user, 0, 0, nullptr);
  if (g.hmd < 0) {
    setStatus("sceHmd2Open failed 0x%08x", g.hmd);
    return false;
  }

  // The tracker (profile 0, the one open to apps).
  uint32_t qparam[8] = {0x20, 0}, qout[8] = {0};
  if (sceVrTracker2QueryMemory(qparam, qout) != 0) {
    setStatus("tracker memory query failed");
    return false;
  }
  void *twork = directAlloc(qout[1], qout[2] > 0x10000 ? qout[2] : 0x10000);
  if (!twork) {
    setStatus("no direct memory for the tracker");
    return false;
  }
  uint8_t ip[0x48];
  std::memset(ip, 0, sizeof ip);
  *(uint32_t *)(ip + 0x00) = 0x48;
  *(int32_t *)(ip + 0x08) = 700;
  *(int32_t *)(ip + 0x0c) = 700;
  *(uint64_t *)(ip + 0x10) = 0x1f00;
  *(uint64_t *)(ip + 0x18) = 0x1f00;
  *(void **)(ip + 0x20) = twork;
  *(uint32_t *)(ip + 0x28) = qout[1];
  *(uint32_t *)(ip + 0x2c) = qout[2];
  rc = sceVrTracker2Initialize(ip);
  int rc2 = rc == 0 ? sceVrTracker2RegisterDevice(0, g.hmd) : -1;
  if (rc != 0 || rc2 != 0) {
    setStatus("tracker init 0x%08x register 0x%08x", rc, rc2);
    return false;
  }

  // Reprojection (once per process).
  SizeAlign wb = sceHmd2ReprojectionQueryBufferSizeAlign();
  SizeAlign db = sceHmd2ReprojectionQueryDisplayBufferSizeAlign();
  void *work = directAlloc(wb.size, 0x10000), *disp = directAlloc(db.size, 0x10000);
  if (!work || !disp) {
    setStatus("no direct memory for reprojection");
    return false;
  }
  uint8_t rp[72];
  std::memset(rp, 0, sizeof rp);
  *(void **)(rp + 0x00) = work;
  *(void **)(rp + 0x08) = disp;
  *(uint32_t *)(rp + 0x10) = 256;
  *(int32_t *)(rp + 0x14) = 0x1f00;
  *(uint32_t *)(rp + 0x18) = 1;
  rc = sceHmd2ReprojectionInitialize(rp, nullptr);
  if (rc != 0) {
    setStatus("reprojection init failed 0x%08x", rc);
    return false;
  }
  g.loaded = true;
  g.failed = false;
  setStatus("headset ready (FOV out %.2f in %.2f up %.2f down %.2f)", g.fov[0], g.fov[1], g.fov[2], g.fov[3]);
  return true;
}

// Enters VR mode: describes the three eye buffers to the GPU (image descriptors), switches reprojection to 120 Hz,
// opens the VR video port and registers the buffers as the layers shown. Also turns on the TV mirror so the TV
// shows what the headset shows. After this the title must not present its own picture.
bool start(const Targets &t) {
  if (g.active) return true;
  if (!g.loaded) return false;
  g.targets = t;
  uint32_t tsharp[kBuffers][8];
  for (int b = 0; b < kBuffers; b++)
    if (!buildTsharp(tsharp[b], t.address[b], t.width, t.height, t.pitch, t.tiled)) {
      setStatus("eye buffer %d at 0x%llx can't be described (%s)", b, (unsigned long long)t.address[b],
                t.tiled ? "tiled needs 64 KiB alignment" : "linear needs 256-byte alignment");
      return false;
    }
  int rc = sceHmd2ReprojectionEnableVrMode(VR_MODE_120HZ);
  g.vh = sceVideoOutOpen(0xff, 32, 0, nullptr);
  if (g.vh < 0) {
    setStatus("EnableVrMode 0x%08x, VR video port failed 0x%08x", rc, g.vh);
    sceHmd2ReprojectionDisableVrMode();
    return false;
  }
  VoBuffer bufs[kBuffers];
  std::memset(bufs, 0, sizeof bufs);
  std::memset(g.layers, 0, sizeof g.layers);
  for (int b = 0; b < kBuffers; b++) {
    // A type 0x10 layer: the same side-by-side texture for both eyes.
    *(uint32_t *)g.layers[b] = 0x10;
    std::memcpy(g.layers[b] + 0x04, tsharp[b], 32);
    std::memcpy(g.layers[b] + 0x24, tsharp[b], 32);
    g.sets[b].layers = g.layers[b];
    g.sets[b].count = 1;
    bufs[b].data = &g.sets[b];
  }
  uint8_t attr[80];
  std::memset(attr, 0, sizeof attr);
  sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000ull, 0, t.width, t.height, 0, 0, 0);
  rc = sceVideoOutRegisterBuffers2(g.vh, 0, 0, bufs, kBuffers, attr, 0x10, nullptr);
  if (rc != 0) {
    setStatus("RegisterBuffers2 failed 0x%08x", rc);
    sceVideoOutClose(g.vh);
    g.vh = -1;
    sceHmd2ReprojectionDisableVrMode();
    return false;
  }
  int c1 = sceVideoOutConfigureOutput(g.vh, VR_MODE_120HZ, nullptr, nullptr, 0);
  int c2 = sceHmd2ReprojectionSetRenderConfig(1, 0, 0);
  g.buf = 0;
  g.frameOpen = false;
  g.active = true;
  g.nextBegin = 0;
  // The TV shows what the headset shows, as PS5VR sets it up (after Horizon Call of the Mountain):
  // option {1}, then work and display memory. The title stops presenting its own flat picture
  // meanwhile, which the system was also showing in the headset as a screen in front of the scene.
  if (!g.mirrorWork) {
    const SizeAlign w = sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign();
    const SizeAlign d = sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign();
    g.mirrorWork = directAlloc(w.size, w.align > 0x10000 ? w.align : 0x10000);
    g.mirrorDisplay = directAlloc(d.size, d.align > 0x10000 ? d.align : 0x10000);
  }
  if (g.mirrorWork && g.mirrorDisplay) {
    const uint32_t opt[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    const int m1 = sceHmd2ReprojectionSetMirroringOption(opt);
    void *param[2] = {g.mirrorWork, g.mirrorDisplay};
    const int m2 = sceHmd2ReprojectionEnableMirroring(param);
    g.mirroring = m2 == 0;
    platform::log("vr: TV mirror option 0x%08x enable 0x%08x", m1, m2);
  }
  setStatus("VR on: %dx%d %s eye buffers, 120 Hz 0x%08x, half rate 0x%08x", t.width, t.height,
            t.tiled ? "tiled" : "linear", c1, c2);
  return true;
}

void vibrate(float strength);

// Leaves VR mode in the order the system needs: mirror off, VR mode off (the reprojection keeps reading the last
// buffer until then), then release the buffers, retrying while the port still reports busy.
void stop() {
  if (!g.active) return;
  vibrate(0);
  g.active = false;
  g.frameOpen = false;
  if (g.mirroring) platform::log("vr: TV mirror off 0x%08x", sceHmd2ReprojectionDisableMirroring());
  g.mirroring = false;
  // Leave VR mode first: the reprojection keeps sampling the last buffer until it stops.
  int rc3 = sceHmd2ReprojectionDisableVrMode();
  int rc = 0;
  for (int i = 0; i < 50; i++) {
    rc = sceVideoOutUnregisterBuffers(g.vh, 0);
    if (rc != VO_BUSY) break;
    sceKernelUsleep(10000);
  }
  int rc2 = sceVideoOutClose(g.vh);
  g.vh = -1;
  setStatus("VR off after %u frames (unregister 0x%08x close 0x%08x disable 0x%08x)", g.submits, rc, rc2, rc3);
}

bool active() { return g.active; }


// PS5VR's app_vr_wait_slot: a fixed 60 Hz schedule (the headset shows each frame twice at 120 Hz);
// after a stall it restarts from now.
void waitSlot() {
  if (!g.active) return;
  constexpr uint64_t FRAME_US = 16667;
  uint64_t now = sceKernelGetProcessTime();
  if (g.nextBegin > now && g.nextBegin - now <= FRAME_US) {
    sceKernelUsleep((unsigned)(g.nextBegin - now));
    now = g.nextBegin;
  }
  g.nextBegin = g.nextBegin && now < g.nextBegin + FRAME_US ? g.nextBegin + FRAME_US : now + FRAME_US;
}

// Heading only, about the vertical axis: the scene stays level whatever the head's pitch and roll.
float headingDeg(const float q[4]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float fx = -(2 * (x * z + y * w)), fz = -(1 - 2 * (x * x + y * y));
  return std::atan2(-fx, -fz) * 57.29578f;
}

// Makes where the player is looking now the scene's forward direction, and the head's position the origin. Only
// the heading (rotation about the vertical axis) is kept, so the scene stays level. Called by the game on R3, and
// automatically after three seconds of tracking.
void recenter() {
  float dp[3], dq[4], lq[4], rq[4];
  if (!g.loaded || !pose(0, dp, dq, lq, rq)) return;
  // The eye orientations drive the view, so they decide where "ahead" is. The device orientation
  // can still be the tracker's stale start-up default (yaw -90) after it reports tracking.
  const float device = headingDeg(dq), left = headingDeg(lq), right = headingDeg(rq);
  float dl = right - left;
  if (dl > 180) dl -= 360;
  if (dl < -180) dl += 360;
  const float a = (left + dl / 2) / 57.29578f;
  g.originYaw[0] = g.originYaw[2] = 0;
  g.originYaw[1] = std::sin(a * 0.5f);
  g.originYaw[3] = std::cos(a * 0.5f);
  std::memcpy(g.originPos, dp, sizeof dp);
  g.haveOrigin = true;
  platform::log("vr: recentred at %.2f %.2f %.2f, heading %.0f deg (eyes %.0f / %.0f, device %.0f)", dp[0], dp[1], dp[2],
                a * 57.29578f, left, right, device);
}

// The origin set by recenter(): a position and a yaw-only quaternion, for turning tracker space into scene space.
void origin(float pos[3], float yaw[4]) {
  std::memcpy(pos, g.originPos, 12);
  std::memcpy(yaw, g.originYaw, 16);
}

static void hapticTick();

// Starts a frame: opens it with the reprojection (waiting if its queue is full), asks for the pose predicted for
// the moment the frame will be shown, and fills `f` with both eyes' positions, orientations and frustum tangents.
// Draw with exactly these poses so the reprojection can correct for any head movement since. Calling it again
// before submit() re-reads the pose for the same frame. False if no frame can be started now.
bool begin(Frame &f) {
  hapticTick();
  if (!g.active) return false;
  if (!g.frameOpen) {
    const uint64_t frame = g.frame + 1;
    int rc = sceHmd2ReprojectionBeginFrame(frame);
    for (int tries = 0; rc == VR_QUEUE_FULL && tries < 40; tries++) {
      sceKernelUsleep(1000);
      rc = sceHmd2ReprojectionBeginFrame(frame);
    }
    if (rc != 0) {
      if (g.errors++ < 20) platform::log("vr: BeginFrame(%llu) -> 0x%08x", (unsigned long long)frame, rc);
      return false;
    }
    g.frame = frame;
    g.t = 0;
    sceHmd2ReprojectionGetPredictedDisplayTime(frame, &g.t);
    g.frameOpen = true;
  }
  float lq[4] = {0, 0, 0, 1}, rq[4] = {0, 0, 0, 1};
  g.qd[0] = g.qd[1] = g.qd[2] = 0;
  g.qd[3] = 1;
  g.pd[0] = g.pd[1] = g.pd[2] = 0;
  f.tracked = pose(g.t, g.pd, g.qd, lq, rq);
  // The tracker's first poses after registering are a stale default: centre the scene only after
  // three seconds of tracking (R3 recentres any time).
  g.trackedFrames = f.tracked ? g.trackedFrames + 1 : 0;
  if (!g.haveOrigin && g.trackedFrames == 180) recenter();
  const float *eyeQ[2] = {lq, rq};
  for (int e = 0; e < 2; e++) {
    const float side[3] = {e ? IPD / 2 : -IPD / 2, 0, 0};
    float off[3];
    rotate(g.qd, side, off);
    for (int i = 0; i < 3; i++) f.eye[e].pos[i] = g.pd[i] + off[i];
    std::memcpy(f.eye[e].quat, eyeQ[e], 16);
  }
  // Left eye: out to the left, in to the right; the right eye mirrors it.
  const float tl[4] = {g.fov[0] * OVERSCAN, g.fov[1] * OVERSCAN, g.fov[2] * OVERSCAN, g.fov[3] * OVERSCAN};
  const float tr[4] = {tl[1], tl[0], tl[2], tl[3]};
  std::memcpy(f.eye[0].tan, tl, 16);
  std::memcpy(f.eye[1].tan, tr, 16);
  f.buffer = g.buf;
  return true;
}

// Finishes the frame opened by begin(): describes the picture and the pose it was rendered at, hands it to the
// reprojection and flips the video port to this buffer. The GPU must already have finished writing the buffer
// (vr_eyes::fill waits for that). Then the next of the three buffers becomes the one to draw into.
void submit() {
  if (!g.frameOpen) return;
  g.frameOpen = false;
  static uint8_t lp[0x148] __attribute__((aligned(16)));
  struct {
    void *layers;
    int32_t count, pad;
    void *ext;
  } param = {lp, 1, 0, nullptr};
  const uint32_t tw = g.targets.width, th = g.targets.height, ew = tw / 2;
  const float tl[4] = {g.fov[0] * OVERSCAN, g.fov[1] * OVERSCAN, g.fov[2] * OVERSCAN, g.fov[3] * OVERSCAN};
  const float tr[4] = {tl[1], tl[0], tl[2], tl[3]};
  std::memset(lp, 0, sizeof lp);
  uint32_t *u = (uint32_t *)lp;
  u[0x00 / 4] = 0x10;
  u[0x08 / 4] = tw, u[0x0c / 4] = th, u[0x10 / 4] = tw, u[0x14 / 4] = th;
  u[0x18 / 4] = 0x92, u[0x1c / 4] = 0x00fff000, u[0x20 / 4] = 0x05500000;  // sampler
  u[0x28 / 4] = ew, u[0x2c / 4] = th;                                     // left half
  u[0x38 / 4] = ew, u[0x3c / 4] = th, u[0x40 / 4] = ew;                   // right half
  std::memcpy(lp + 0x48, tl, 16);
  std::memcpy(lp + 0x58, tr, 16);
  std::memcpy(lp + 0x68, g.pd, 12);
  std::memcpy(lp + 0x74, g.qd, 16);
  *(uint64_t *)(lp + 0x88) = g.t;
  *(uint64_t *)(lp + 0x90) = g.frame;
  u[0x98 / 4] = 100;
  u[0xa0 / 4] = ew, u[0xa4 / 4] = th, u[0xa8 / 4] = ew, u[0xac / 4] = th;
  int r1 = sceHmd2ReprojectionSetParam((uint32_t)g.buf, g.frame, &param, nullptr);
  int r2 = sceVideoOutSubmitFlip(g.vh, g.buf, 2, (int64_t)g.frame);
  if ((r1 || r2) && g.errors++ < 20)
    platform::log("vr: frame %llu setparam 0x%08x flip 0x%08x", (unsigned long long)g.frame, r1, r2);
  if (g.submits++ % 600 == 0) platform::log("vr: frame %llu submitted (buffer %d)", (unsigned long long)g.frame, g.buf);
  g.buf = (g.buf + 1) % kBuffers;
}

// As Horizon opens the Sense controllers: a pad of type 3 (left) / 4 (right), registered with the
// tracker as device 2 / 1 (0 is the headset). The SDK's libScePad stub lacks the VR read call, so it
// is taken from the system module at run time.
void openHands() {
  if (g.padOpened || !g.loaded) return;
  g.padOpened = true;
  for (int side = 0; side < 2; side++) {
    g.padHandle[side] = scePadOpen(g.user, side ? 4 : 3, 0, nullptr);
    const int r = g.padHandle[side] >= 0 ? sceVrTracker2RegisterDevice(side ? 1 : 2, g.padHandle[side]) : -1;
    g.padRegistered[side] = r == 0;
    platform::log("vr: %s controller pad 0x%x, tracker register 0x%08x", side ? "right" : "left", g.padHandle[side], r);
  }
}

const char *status() { return g.status; }

// Reads one Sense controller (0 left, 1 right): buttons, stick and trigger from the pad, and the position and
// orientation from the tracker at the frame's predicted time. Returns false if the controller cannot be opened;
// `tracked` stays false while the headset's cameras cannot see it. Call after begin().
bool hand(int side, Hand &out) {
  std::memset(&out, 0, sizeof out);
  out.quat[3] = 1;
  if (side < 0 || side > 1 || !g.active) return false;
  openHands();
  if (!g.padRegistered[side]) return false;
  // The Sense controllers read through the ordinary pad call, as a DualSense does: the VR-specific read
  // fails. Buttons are PS button bits, the stick and the two triggers are bytes.
  const int padRc = scePadReadState(g.padHandle[side], out.raw);
  out.connected = padRc == 0;
  if (out.connected) {
    std::memcpy(&out.buttons, out.raw, 4);
    // The left stick is in bytes 4-5. The right controller's alternates between 4-5 and 6-7 from read to
    // read, the other pair holding 00 00 or 80 80, so a 00 00 pair in 6-7 means the stick is in 4-5.
    const int k = side && !(out.raw[6] == 0 && out.raw[7] == 0) ? 6 : 4;
    out.stick[0] = (out.raw[k] - 127.5f) / 127.5f;
    out.stick[1] = (out.raw[k + 1] - 127.5f) / 127.5f;
    out.trigger = out.raw[side ? 9 : 8] / 255.0f;
  }
  static uint8_t res[0x400];
  uint8_t gp[0x20];
  std::memset(gp, 0, sizeof gp);
  *(uint32_t *)(gp + 0x00) = 0x20;
  *(int32_t *)(gp + 0x04) = g.padHandle[side];
  *(uint64_t *)(gp + 0x10) = g.t;
  std::memset(res, 0, sizeof res);
  const int rc = sceVrTracker2GetResult(gp, res);
  // The raw state and pose words go to the log every few seconds and on any button change, until
  // the trigger, grip and touch bits are mapped.
  static uint32_t lastButtons[2];
  static uint64_t next[2];
  const uint64_t now = sceKernelGetProcessTime();
  if (now >= next[side] || out.buttons != lastButtons[side]) {
    next[side] = now + 1000000;
    lastButtons[side] = out.buttons;
    const float *f = (const float *)(res + 0x60), *imu = (const float *)(out.raw + 12), *gyro = (const float *)(out.raw + 40);
    // Pose, the tracker's orientation and the pad's own IMU orientation and gyro: for judging drift.
    platform::log("vr: hand %d pad rc 0x%08x tracker rc 0x%08x tracked %u | buttons %08x trigger %.2f stick %.2f %.2f | %.3f %.3f %.3f | tq %.3f %.3f %.3f %.3f | imu %.3f %.3f %.3f %.3f gyro %.3f %.3f %.3f | w %08x %08x",
                  side, (unsigned)padRc, rc, *(uint32_t *)(res + 4), out.buttons, (double)out.trigger, (double)out.stick[0], (double)out.stick[1], (double)f[0], (double)f[1],
                  (double)f[2], (double)f[4], (double)f[5], (double)f[6], (double)f[7], (double)imu[0], (double)imu[1], (double)imu[2], (double)imu[3], (double)gyro[0],
                  (double)gyro[1], (double)gyro[2], *(uint32_t *)res, *(uint32_t *)(res + 4));
  }
  if (rc != 0 || !*(uint32_t *)(res + 4)) return true;
  out.tracked = true;
  std::memcpy(out.pos, res + 0x60, 12);
  std::memcpy(out.quat, res + 0x70, 16);
  return true;
}

// scePadSetVibration holds its setting until changed, so each buzz is a start and a timed stop.
static uint64_t hapticStop[2];
static bool hapticOn[2];

// A short buzz in a Sense controller: starts it now and schedules the stop 60 ms later (hapticTick).
void haptic(int side, float strength) {
  if (side < 0 || side > 1 || g.padHandle[side] < 0) return;
  const uint8_t v = (uint8_t)std::lround(std::fmin(std::fmax(strength, 0.0f), 1.0f) * 255);
  static bool logged;
  const PadVibration vib{v, v};
  const int rc = scePadSetVibration(g.padHandle[side], &vib);
  if (!logged) logged = true, platform::log("vr: haptic side %d strength %u rc 0x%08x", side, v, (unsigned)rc);
  hapticOn[side] = true;
  hapticStop[side] = sceKernelGetProcessTime() + 60000;
}

// Stops any buzz whose time is up. Called at the start of each frame.
static void hapticTick() {
  const uint64_t now = sceKernelGetProcessTime();
  for (int s = 0; s < 2; s++)
    if (hapticOn[s] && now >= hapticStop[s]) {
      const PadVibration off{0, 0};
      scePadSetVibration(g.padHandle[s], &off);
      hapticOn[s] = false;
    }
}

// The headset's own rumble motor, 0 to 1 mapped onto the levels 0 to 25 it accepts.
void vibrate(float strength) {
  // The headset's rumble motor takes 0 (off) to 25, as PS5VR drives it. Sent only on a change.
  static int last = -1;
  const int level = (int)std::lround(std::fmin(std::fmax(strength, 0.0f), 1.0f) * 25);
  if (!g.loaded || g.hmd < 0 || !sceHmd2SetVibration || level == last) return;
  sceHmd2SetVibration(g.hmd, level);
  last = level;
}
void simulate(float, float) {}
void simulateHand(int, const float[3], const float[4]) {}

}  // namespace vr
