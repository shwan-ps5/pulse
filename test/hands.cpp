// Headset and Sense controller test: a floor grid, the head as a box (visible on the TV), and two
// sabers following the controllers. Square cycles which controller axis the blade lies along, R3
// recentres. Raw pad state and tracker words go to the log (vr_ps5.cpp, vr::hand).
#include <dirent.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

#include "gl.h"
#include "platform.h"
#include "vmath.h"
#include "vr.h"
#include "vr_eyes.h"

namespace {

Mat4 scale(float x, float y, float z) {
  Mat4 s;
  s.m[0] = x, s.m[5] = y, s.m[10] = z;
  return s;
}

unsigned compile(unsigned type, const char *src) {
  const unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  int ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    platform::log("shader: %s", log);
  }
  return s;
}

unsigned program(const char *vs, const char *fs) {
  const unsigned p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
  glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(p);
  return p;
}

const char *kSolidVs = R"(#version 460 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
uniform mat4 vp, model;
out vec3 n;
void main() { n = mat3(model) * nrm; gl_Position = vp * model * vec4(pos, 1.0); })";
const char *kSolidFs = R"(#version 460 core
in vec3 n;
uniform vec3 color;
out vec4 o;
void main() {
  float l = 0.35 + 0.65 * max(dot(normalize(n), normalize(vec3(0.4, 0.8, 0.5))), 0.0);
  o = vec4(color * l, 1.0);
})";
const char *kLineVs = R"(#version 460 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
uniform mat4 vp;
out vec3 c;
void main() { c = col; gl_Position = vp * vec4(pos, 1.0); })";
const char *kLineFs = R"(#version 460 core
in vec3 c;
out vec4 o;
void main() { o = vec4(c, 1.0); })";

struct Gl {
  unsigned solid, line, cubeVao, cubeVbo, lineVao, lineVbo;
  int solidVp, solidModel, solidColor, lineVp;
} gl;

void initGl() {
  gl.solid = program(kSolidVs, kSolidFs);
  gl.line = program(kLineVs, kLineFs);
  gl.solidVp = glGetUniformLocation(gl.solid, "vp");
  gl.solidModel = glGetUniformLocation(gl.solid, "model");
  gl.solidColor = glGetUniformLocation(gl.solid, "color");
  gl.lineVp = glGetUniformLocation(gl.line, "vp");

  // A unit cube centred on the origin, 36 vertices (position, face normal).
  std::vector<float> v;
  const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int f = 0; f < 6; f++) {
    const float *a = n[f];
    float u[3] = {a[1], a[2], a[0]}, w[3] = {a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0]};
    const int q[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (auto &c : q) {
      for (int k = 0; k < 3; k++) v.push_back(0.5f * a[k] + 0.5f * c[0] * u[k] + 0.5f * c[1] * w[k]);
      for (int k = 0; k < 3; k++) v.push_back(a[k]);
    }
  }
  glCreateBuffers(1, &gl.cubeVbo);
  glNamedBufferStorage(gl.cubeVbo, v.size() * 4, v.data(), 0);
  glCreateVertexArrays(1, &gl.cubeVao);
  glVertexArrayVertexBuffer(gl.cubeVao, 0, gl.cubeVbo, 0, 24);
  glEnableVertexArrayAttrib(gl.cubeVao, 0);
  glEnableVertexArrayAttrib(gl.cubeVao, 1);
  glVertexArrayAttribFormat(gl.cubeVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
  glVertexArrayAttribFormat(gl.cubeVao, 1, 3, GL_FLOAT, GL_FALSE, 12);
  glVertexArrayAttribBinding(gl.cubeVao, 0, 0);
  glVertexArrayAttribBinding(gl.cubeVao, 1, 0);

  glCreateBuffers(1, &gl.lineVbo);
  glNamedBufferStorage(gl.lineVbo, 1 << 20, nullptr, GL_DYNAMIC_STORAGE_BIT);
  glCreateVertexArrays(1, &gl.lineVao);
  glVertexArrayVertexBuffer(gl.lineVao, 0, gl.lineVbo, 0, 24);
  glEnableVertexArrayAttrib(gl.lineVao, 0);
  glEnableVertexArrayAttrib(gl.lineVao, 1);
  glVertexArrayAttribFormat(gl.lineVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
  glVertexArrayAttribFormat(gl.lineVao, 1, 3, GL_FLOAT, GL_FALSE, 12);
  glVertexArrayAttribBinding(gl.lineVao, 0, 0);
  glVertexArrayAttribBinding(gl.lineVao, 1, 0);
}

// The console's GL doesn't clip at w = 0, so lines are cut at the near plane here.
struct Lines {
  std::vector<float> v;
  const Mat4 *view;
  void add(const float a[3], const float b[3], const float c[3]) {
    const float *m = view->m;
    auto z = [&](const float p[3]) { return m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]; };
    const float near = -0.05f;
    float pa[3] = {a[0], a[1], a[2]}, pb[3] = {b[0], b[1], b[2]};
    const float za = z(pa), zb = z(pb);
    if (za > near && zb > near) return;
    if (za > near) {
      const float t = (near - za) / (zb - za);
      for (int i = 0; i < 3; i++) pa[i] = a[i] + t * (b[i] - a[i]);
    } else if (zb > near) {
      const float t = (near - za) / (zb - za);
      for (int i = 0; i < 3; i++) pb[i] = a[i] + t * (b[i] - a[i]);
    }
    for (const float *p : {pa, pb}) {
      v.insert(v.end(), p, p + 3);
      v.insert(v.end(), c, c + 3);
    }
  }
};

struct HandState {
  vr::Hand h;
  float pos[3], quat[4];  // in scene space
} hands[2];

float headPos[3], headQuat[4] = {0, 0, 0, 1};
int bladeAxis;
const float kAxes[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
const char *kAxisNames[6] = {"-z", "+z", "+y", "-y", "+x", "-x"};

// Tracker space to the scene: about the recentre point, level, the head's rest height at 1.6 m.
void toScene(const float p[3], float out[3]) {
  float op[3], yaw[4];
  vr::origin(op, yaw);
  const float inv[4] = {0, -yaw[1], 0, yaw[3]};
  const float t[3] = {p[0] - op[0], p[1] - op[1], p[2] - op[2]};
  rotate(inv, t, out);
  out[1] += 1.6f;
}

void quatToScene(const float q[4], float out[4]) {
  float op[3], yaw[4];
  vr::origin(op, yaw);
  const float inv[4] = {0, -yaw[1], 0, yaw[3]};
  qmul(inv, q, out);
}

void drawBox(const Mat4 &vp, const Mat4 &model, float r, float g, float b) {
  glUseProgram(gl.solid);
  glUniformMatrix4fv(gl.solidVp, 1, GL_FALSE, vp.m);
  glUniformMatrix4fv(gl.solidModel, 1, GL_FALSE, model.m);
  glUniform3f(gl.solidColor, r, g, b);
  glBindVertexArray(gl.cubeVao);
  glDrawArrays(GL_TRIANGLES, 0, 36);
}

void drawScene(const Mat4 &view, const Mat4 &proj) {
  const Mat4 vp = proj * view;
  Lines lines;
  lines.view = &view;
  const float grey[3] = {0.25f, 0.28f, 0.35f}, red[3] = {1, 0.2f, 0.2f}, grn[3] = {0.2f, 1, 0.2f}, blu[3] = {0.3f, 0.5f, 1};
  for (int i = -8; i <= 8; i++) {
    const float a[3] = {(float)i, 0, -8}, b[3] = {(float)i, 0, 8}, c[3] = {-8, 0, (float)i}, d[3] = {8, 0, (float)i};
    lines.add(a, b, grey);
    lines.add(c, d, grey);
  }
  const float o[3] = {0, 0.01f, 0}, x[3] = {1, 0.01f, 0}, y[3] = {0, 1.01f, 0}, z[3] = {0, 0.01f, 1};
  lines.add(o, x, red);
  lines.add(o, y, grn);
  lines.add(o, z, blu);
  // Each controller's three axes, so the orientation convention can be read off the picture.
  for (int s = 0; s < 2; s++) {
    if (!hands[s].h.tracked) continue;
    const Mat4 r = rotation(hands[s].quat);
    const float *p = hands[s].pos;
    for (int k = 0; k < 3; k++) {
      const float e[3] = {p[0] + 0.15f * r.m[k * 4], p[1] + 0.15f * r.m[k * 4 + 1], p[2] + 0.15f * r.m[k * 4 + 2]};
      lines.add(p, e, k == 0 ? red : k == 1 ? grn : blu);
    }
  }
  glNamedBufferSubData(gl.lineVbo, 0, lines.v.size() * 4, lines.v.data());
  glUseProgram(gl.line);
  glUniformMatrix4fv(gl.lineVp, 1, GL_FALSE, vp.m);
  glBindVertexArray(gl.lineVao);
  glDrawArrays(GL_LINES, 0, (int)lines.v.size() / 6);

  for (int s = 0; s < 2; s++) {
    if (!hands[s].h.tracked) continue;
    const float *d = kAxes[bladeAxis];
    const float len = hands[s].h.connected ? 1.0f : 0.3f, th = 0.025f;
    const Mat4 body = translation(hands[s].pos[0], hands[s].pos[1], hands[s].pos[2]) * rotation(hands[s].quat);
    drawBox(vp, body * scale(0.05f, 0.05f, 0.1f), 0.8f, 0.8f, 0.8f);
    const float sx = std::fabs(d[0]) * len + (1 - std::fabs(d[0])) * th, sy = std::fabs(d[1]) * len + (1 - std::fabs(d[1])) * th,
                sz = std::fabs(d[2]) * len + (1 - std::fabs(d[2])) * th;
    const Mat4 blade = body * translation(d[0] * len * 0.5f, d[1] * len * 0.5f, d[2] * len * 0.5f) * scale(sx, sy, sz);
    s ? drawBox(vp, blade, 0.2f, 0.4f, 1.0f) : drawBox(vp, blade, 1.0f, 0.15f, 0.2f);
  }
  // The head, for the TV view of the player.
  drawBox(vp, translation(headPos[0], headPos[1], headPos[2]) * rotation(headQuat) * scale(0.18f, 0.22f, 0.2f), 0.9f, 0.7f, 0.2f);
}

// draws the floor grid, the head box and both sabers into the eye framebuffer for each eye.
void drawEyes(const vr::Frame &f) {
  const int eye = vr_eyes::eye();
  glBindFramebuffer(GL_FRAMEBUFFER, vr_eyes::framebuffer());
  glEnable(GL_DEPTH_TEST);
  glViewport(0, 0, eye * 2, eye);
  glClearColor(0.02f, 0.02f, 0.05f, 1);
  glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  for (int e = 0; e < 2; e++) {
    float p[3], q[4];
    toScene(f.eye[e].pos, p);
    quatToScene(f.eye[e].quat, q);
    glViewport(e * eye, 0, eye, eye);
    drawScene(eyeView(p, q), frustum(f.eye[e].tan, 0.05f, 100.0f));
  }
}

// Which storage a title can read: lists each candidate path (two levels) and reads the head of the
// first MP3 it finds, so the log says where music can come from.
void probeDir(const char *path, int depth, int &mp3Tried) {
  DIR *d = opendir(path);
  if (!d) {
    if (depth == 0) platform::log("storage: %s: cannot open", path);
    return;
  }
  int shown = 0;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    char full[512];
    std::snprintf(full, sizeof full, "%s/%s", path, e->d_name);
    struct stat st;
    const bool isDir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    if (shown++ < 25) platform::log("storage: %s%s%s", depth ? "  " : "", full, isDir ? "/" : "");
    const size_t n = std::strlen(e->d_name);
    if (!isDir && n > 4 && !strcasecmp(e->d_name + n - 4, ".mp3") && mp3Tried < 1) {
      mp3Tried++;
      if (FILE *f = std::fopen(full, "rb")) {
        unsigned char head[4096];
        const size_t got = std::fread(head, 1, sizeof head, f);
        platform::log("storage: read %zu bytes of %s (size %lld), starts %02x %02x %02x", got, full, (long long)st.st_size, head[0], head[1], head[2]);
        std::fclose(f);
      } else {
        platform::log("storage: cannot open %s for reading", full);
      }
    }
    if (isDir && depth < 1) probeDir(full, depth + 1, mp3Tried);
  }
  closedir(d);
}

// logs which of the likely storage paths the sandbox lets the title list/read (finding that opendir is refused on most is
// why the shipped game reads songs/index.txt instead). tool, not part of the game.
void probeStorage() {
  int mp3 = 0;
  for (const char *path : {"/download0", "/app0", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/usb0", "/usb1", "/data", "/mnt/ext0", "/mnt/ext1"}) {
    platform::log("storage: probing %s", path);
    probeDir(path, 0, mp3);
  }
  platform::log("storage: probe done");
}

}  // namespace

int main() {
  if (!platform::init("Hands", 1280, 720, false)) return 1;
  initGl();
  probeStorage();
  const bool available = vr::init(platform::user());
  bool on = false;
  unsigned depth = 0;
  if (available && vr_eyes::init()) {
    const int e = vr_eyes::eye();
    glCreateRenderbuffers(1, &depth);
    glNamedRenderbufferStorage(depth, GL_DEPTH_COMPONENT32F, e * 2, e);
    glNamedFramebufferRenderbuffer(vr_eyes::framebuffer(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    on = vr::start(vr_eyes::targets());
  }
  platform::log("hands: headset %s", on ? "on" : vr::status());
  platform::notify(on ? "Hands test: Square cycles blade axis, R3 recentres" : "Hands test: no headset");

  uint32_t last = 0;
  while (platform::poll()) {
    const uint32_t b = platform::pad().buttons, edge = b & ~last;
    last = b;
    if (edge & platform::BTN_SQUARE) {
      bladeAxis = (bladeAxis + 1) % 6;
      platform::log("hands: blade along controller %s", kAxisNames[bladeAxis]);
    }
    if (edge & platform::BTN_R3) vr::recenter();

    vr::Frame f;
    if (on && vr::begin(f)) {
      float a[3], c[3];
      toScene(f.eye[0].pos, a);
      toScene(f.eye[1].pos, c);
      for (int i = 0; i < 3; i++) headPos[i] = 0.5f * (a[i] + c[i]);
      quatToScene(f.eye[0].quat, headQuat);
      for (int s = 0; s < 2; s++) {
        vr::hand(s, hands[s].h);
        toScene(hands[s].h.pos, hands[s].pos);
        quatToScene(hands[s].h.quat, hands[s].quat);
      }
      static double nextReport;
      if (platform::seconds() >= nextReport) {
        nextReport = platform::seconds() + 0.5;
        platform::log("pose head %.2f %.2f %.2f q %.2f %.2f %.2f %.2f tracked %d", headPos[0], headPos[1], headPos[2], headQuat[0],
                      headQuat[1], headQuat[2], headQuat[3], f.tracked);
        for (int s = 0; s < 2; s++) {
          const vr::Hand &h = hands[s].h;
          platform::log("pose %s tracked %d conn %d pos %.2f %.2f %.2f q %.2f %.2f %.2f %.2f btn %08x stick %.2f %.2f", s ? "R" : "L", h.tracked,
                        h.connected, hands[s].pos[0], hands[s].pos[1], hands[s].pos[2], hands[s].quat[0], hands[s].quat[1],
                        hands[s].quat[2], hands[s].quat[3], h.buttons, h.stick[0], h.stick[1]);
        }
      }
      drawEyes(f);
      glFinish();
      vr_eyes::fill(f.buffer);
      vr::submit();
    }

    // The TV: a fixed camera behind and above the player, looking down the room. On the console
    // the headset's own mirror shows the TV while VR is on, so nothing is presented there.
#ifdef PLATFORM_PS5
    if (on) {
      vr::waitSlot();
      continue;
    }
#endif
    const float cam[3] = {0, 2.0f, 2.2f}, camQ[4] = {std::sin(-0.12f), 0, 0, std::cos(-0.12f)};
    const float aspect = (float)platform::width() / platform::height(), tan[4] = {0.6f * aspect, 0.6f * aspect, 0.6f, 0.6f};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, platform::width(), platform::height());
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.02f, 0.02f, 0.05f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawScene(eyeView(cam, camQ), frustum(tan, 0.05f, 100.0f));
    platform::swap();
  }
  if (on) vr::stop();
  platform::shutdown();
  return 0;
}
