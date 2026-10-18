// GL renderer: notes/sabers/field (solid + line progs), text (baked atlas), menu panels, neon background (fullscreen shader or
// real 3D rings), bloom post-process, head-locked hint. API in render.h; one entry point per frame: draw(vp, scene, bg).
// CS: scene space (m, y up, -z away from player). vp = projection*view, supplied by caller per eye (VR) or once (flat view).
// state: file-scope `gfx` (programs/VAOs/buffers), `post` (bloom targets), palette ids. T: GL thread only.
// draw order inside draw() (!): background (no depth) -> rings (3D mode) -> field lines -> panel -> notes far-to-near -> shards ->
//   sabers (additive glow shells, depth-write off, THEN core) -> HUD. anything opaque drawn after the saber glow overwrites it,
//   because glow does not write depth: keep opaque UI BEFORE sabers (this was a real bug: menu hid saber glow).
// !: the console GL does not clip at the eye plane: a big triangle with a vertex behind the camera vanishes. keep large flat things
//   tiled (drawPanel uses 0.3 m tiles). no gl_ClipDistance/CPU clip here.
// !: bloom (postBegin/postEnd, RGBA16F + 5-level dual filter) is only used by the flat TV view; the headset path draws straight
//   to the eye framebuffer (cost). shader consts: threshold .85, shoulder tone-map .8.
// ref: flow in game/main.cpp drawFlat/drawEyes; colours via kPalettes (accessibility palettes + hand swap).
#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fontatlas.h"
#include "gl.h"

namespace render {

namespace {

// shaders: kVs/kFs solid lit mesh with distance fog; kLine* coloured lines; kText*/kArtFs textured quads (atlas alpha / RGB art);
// kBg* fullscreen background from view direction (no geometry); kPost/kDown/kUp/kComp bloom chain + tone map.
const char *kVs = R"(#version 460 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
uniform mat4 vp, model;
out vec3 n;
void main() { n = mat3(model) * nrm; gl_Position = vp * model * vec4(pos, 1.0); })";
const char *kFs = R"(#version 460 core
in vec3 n;
uniform vec3 color;
uniform float lit, fog;
uniform vec3 fogColor;
out vec4 o;
void main() {
  float l = mix(1.0, 0.4 + 0.6 * max(dot(normalize(n), normalize(vec3(0.3, 0.6, 0.7))), 0.0), lit);
  o = vec4(mix(color * l, fogColor, fog), 1.0);
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

const char *kTextVs = R"(#version 460 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 col;
uniform mat4 vp;
out vec2 t;
out vec3 c;
void main() { t = uv; c = col; gl_Position = vp * vec4(pos, 1.0); })";
const char *kTextFs = R"(#version 460 core
in vec2 t;
in vec3 c;
uniform sampler2D atlas;
out vec4 o;
void main() { o = vec4(c, texture(atlas, t).r); })";

const char *kBgVs = R"(#version 460 core
uniform mat4 vp;
out vec3 dir;
void main() {
  vec2 ndc = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
  mat4 inv = inverse(vp);
  vec4 a = inv * vec4(ndc, -1.0, 1.0), b = inv * vec4(ndc, 1.0, 1.0);
  dir = b.xyz / b.w - a.xyz / a.w;
  gl_Position = vec4(ndc, 1.0, 1.0);
})";
// Neon rings of polygons rushing at the viewer, and (as the song builds, on the harder levels) a kaleidoscoped Julia
// set drawn as glowing filaments. Everything scales with E, the song's energy arc: quiet, building, quiet again.
const char *kBgFs = R"(#version 460 core
in vec3 dir;
out vec4 o;
uniform float t, E, bass, mid, high, beat, hue, fracW, quality, geo;
uniform int sides;
vec3 pal(float h) { return pow(0.5 + 0.5 * cos(6.2831853 * (h + vec3(0.0, 0.33, 0.67))), vec3(1.6)); }
vec2 rot(vec2 p, float a) { float c = cos(a), s = sin(a); return vec2(c * p.x - s * p.y, s * p.x + c * p.y); }
float ngon(vec2 p, float n, float r) {
  float a = atan(p.x, p.y) + 3.14159265, s = 6.2831853 / n;
  return cos(floor(0.5 + a / s) * s - a) * length(p) - r;
}
void main() {
  vec3 d = normalize(dir);
  vec2 p = d.xy / max(-d.z, 0.3);
  vec3 col = vec3(0.008, 0.01, 0.022) + pal(hue) * 0.03 * (0.4 + E);
  int layers = geo > 0.5 ? 3 + int(E * 9.0) : 0;
  float speed = 0.05 + 0.16 * E;
  for (int i = 0; i < 12; i++) {
    if (i >= layers) break;
    float f = fract(t * speed + float(i) / float(layers));
    float r = 0.03 * exp(f * 5.0) * (1.0 + 0.1 * beat * bass);
    vec2 q = rot(p, f * 1.1 + t * 0.12 * (0.4 + E) + float(i) * 0.55);
    float g = (0.005 + 0.012 * r) / (abs(ngon(q, float(sides), r)) + 0.004);
    float fade = smoothstep(0.0, 0.18, f) * (1.0 - smoothstep(0.78, 1.0, f));
    col += pal(hue + 0.06 * float(i) + 0.15 * f) * g * fade * (0.2 + 0.7 * E + 0.4 * beat * bass);
  }
  float spokes = smoothstep(0.35, 0.8, E) * geo;
  if (spokes > 0.0) {
    float a = atan(p.y, p.x), r = length(p);
    float s = abs(sin(a * float(sides) * 0.5 + t * 0.15));
    col += pal(hue + 0.5) * (0.0035 / (s * r + 0.012)) * exp(-r * 0.9) * spokes * (0.5 + mid);
  }
  if (fracW > 0.0) {
    vec2 q = rot(p, t * 0.04);
    float a = atan(q.y, q.x), r = length(q), k = 6.2831853 / float(sides + 2);
    a = abs(mod(a, k) - k * 0.5);
    q = r * vec2(cos(a), sin(a));
    vec2 z = q * (1.1 + 1.0 * (1.0 - E));
    float ang = t * 0.06 + bass * 0.25;
    vec2 c = 0.7885 * vec2(cos(ang), sin(ang));
    float trap = 1e9, it = 0.0;
    int n = quality > 1.5 ? 48 : 22;
    for (int i = 0; i < 48; i++) {
      if (i >= n) break;
      z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
      trap = min(trap, abs(length(z) - 0.75 - 0.15 * sin(t * 0.5)));
      if (dot(z, z) > 16.0) break;
      it += 1.0;
    }
    float g = 0.012 / (trap + 0.012);
    col += pal(hue + it * 0.035 + 0.3 * high) * g * fracW * (0.1 + 0.5 * E + 0.3 * beat * bass);
  }
  col *= 1.0 - 0.3 * clamp(length(p) * 0.35, 0.0, 1.0);
  o = vec4(col, 1.0);
})";
const char *kPostVs = R"(#version 460 core
out vec2 uv;
void main() {
  uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})";
// Dual-filter downsample; the first pass keeps only what is brighter than the threshold.
const char *kDownFs = R"(#version 460 core
in vec2 uv;
out vec4 o;
uniform sampler2D src;
uniform vec2 texel;
uniform float threshold;
vec3 bright(vec3 c) {
  float m = max(c.r, max(c.g, c.b));
  float w = clamp((m - threshold) / max(m, 1e-4), 0.0, 1.0);
  return c * w;
}
vec3 tap(vec2 p) { vec3 c = texture(src, p).rgb; return threshold > 0.0 ? bright(c) : c; }
void main() {
  vec3 c = tap(uv) * 4.0;
  c += tap(uv + texel * vec2(-1, -1)) + tap(uv + texel * vec2(1, -1)) + tap(uv + texel * vec2(-1, 1)) + tap(uv + texel * vec2(1, 1));
  o = vec4(c / 8.0, 1.0);
})";
const char *kUpFs = R"(#version 460 core
in vec2 uv;
out vec4 o;
uniform sampler2D src;
uniform vec2 texel;
void main() {
  vec3 c = texture(src, uv + texel * vec2(-1, 0)).rgb + texture(src, uv + texel * vec2(1, 0)).rgb + texture(src, uv + texel * vec2(0, -1)).rgb + texture(src, uv + texel * vec2(0, 1)).rgb;
  c += 2.0 * (texture(src, uv + texel * vec2(-0.5, -0.5)).rgb + texture(src, uv + texel * vec2(0.5, -0.5)).rgb + texture(src, uv + texel * vec2(-0.5, 0.5)).rgb + texture(src, uv + texel * vec2(0.5, 0.5)).rgb);
  o = vec4(c / 12.0, 1.0);
})";
const char *kCompFs = R"(#version 460 core
in vec2 uv;
out vec4 o;
uniform sampler2D scene, bloom;
uniform float strength;
vec3 shoulder(vec3 x) { return mix(x, 0.8 + 0.2 * tanh((x - 0.8) / 0.2), step(0.8, x)); }
void main() {
  vec3 c = texture(scene, uv).rgb + texture(bloom, uv).rgb * strength;
  o = vec4(shoulder(c), 1.0);
})";

const char *kArtFs = R"(#version 460 core
in vec2 t;
in vec3 c;
uniform sampler2D atlas;
out vec4 o;
void main() { o = vec4(texture(atlas, t).rgb * c, 1.0); })";

// compile one shader; compile errors go to stderr only (no abort): a bad shader renders black, check the console log.
unsigned compile(unsigned type, const char *src) {
  const unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  int ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "shader: %s\n", log);
  }
  return s;
}
// link vs+fs into a program. shaders are not deleted (process lifetime).
unsigned program(const char *vs, const char *fs) {
  const unsigned p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
  glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(p);
  return p;
}

// all GL objects the renderer owns, created once by initGfx(); uniform locations cached for the solid/line/text programs.
struct Gfx {
  unsigned solid, line, cubeVao, lineVao, lineVbo, triVao, cylVao, textProg, artProg, textVao, textVbo, atlas, artTex, bgProg, downProg, upProg, compProg, emptyVao;
  int vp, model, color, lit, fog, fogColor, lineVp, textVp;
} gfx;

constexpr size_t kTextBytes = 1 << 20;

// vertex layout shared by meshes: location 0 pos(vec3), 1 normal/colour(vec3), stride 24 bytes, one buffer.
void attribs(unsigned vao, unsigned vbo) {
  glVertexArrayVertexBuffer(vao, 0, vbo, 0, 24);
  for (int a = 0; a < 2; a++) {
    glEnableVertexArrayAttrib(vao, a);
    glVertexArrayAttribFormat(vao, a, 3, GL_FLOAT, GL_FALSE, a * 12);
    glVertexArrayAttribBinding(vao, a, 0);
  }
}

// builds programs, unit cube/cylinder/triangle meshes, text and line buffers, the font atlas texture, empty VAO for
// fullscreen triangles. false never today (errors are logged). call after a GL context exists.
bool initGfx() {
  gfx.solid = program(kVs, kFs);
  gfx.line = program(kLineVs, kLineFs);
  gfx.vp = glGetUniformLocation(gfx.solid, "vp"), gfx.model = glGetUniformLocation(gfx.solid, "model");
  gfx.color = glGetUniformLocation(gfx.solid, "color"), gfx.lit = glGetUniformLocation(gfx.solid, "lit");
  gfx.fog = glGetUniformLocation(gfx.solid, "fog"), gfx.fogColor = glGetUniformLocation(gfx.solid, "fogColor");
  gfx.lineVp = glGetUniformLocation(gfx.line, "vp");
  gfx.textProg = program(kTextVs, kTextFs);
  gfx.textVp = glGetUniformLocation(gfx.textProg, "vp");
  gfx.artProg = program(kTextVs, kArtFs);
  gfx.bgProg = program(kBgVs, kBgFs);
  gfx.downProg = program(kPostVs, kDownFs);
  gfx.upProg = program(kPostVs, kUpFs);
  gfx.compProg = program(kPostVs, kCompFs);
  glCreateVertexArrays(1, &gfx.emptyVao);
  glCreateTextures(GL_TEXTURE_2D, 1, &gfx.artTex);
  glTextureStorage2D(gfx.artTex, 1, GL_RGB8, 256, 256);
  glTextureParameteri(gfx.artTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTextureParameteri(gfx.artTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glCreateTextures(GL_TEXTURE_2D, 1, &gfx.atlas);
  glTextureStorage2D(gfx.atlas, 1, GL_R8, fontatlas::kW, fontatlas::kH);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTextureSubImage2D(gfx.atlas, 0, 0, 0, fontatlas::kW, fontatlas::kH, GL_RED, GL_UNSIGNED_BYTE, fontatlas::kPixels);
  glTextureParameteri(gfx.atlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTextureParameteri(gfx.atlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTextureParameteri(gfx.atlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTextureParameteri(gfx.atlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glCreateBuffers(1, &gfx.textVbo);
  glNamedBufferStorage(gfx.textVbo, kTextBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
  glCreateVertexArrays(1, &gfx.textVao);
  glVertexArrayVertexBuffer(gfx.textVao, 0, gfx.textVbo, 0, 32);
  for (int a = 0; a < 3; a++) {
    glEnableVertexArrayAttrib(gfx.textVao, a);
    glVertexArrayAttribBinding(gfx.textVao, a, 0);
  }
  glVertexArrayAttribFormat(gfx.textVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
  glVertexArrayAttribFormat(gfx.textVao, 1, 2, GL_FLOAT, GL_FALSE, 12);
  glVertexArrayAttribFormat(gfx.textVao, 2, 3, GL_FLOAT, GL_FALSE, 20);
  std::vector<float> v;
  const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int f = 0; f < 6; f++) {
    const float *a = n[f];
    const float u[3] = {a[1], a[2], a[0]}, w[3] = {a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0]};
    const int q[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (auto &c : q) {
      for (int k = 0; k < 3; k++) v.push_back(0.5f * a[k] + 0.5f * c[0] * u[k] + 0.5f * c[1] * w[k]);
      for (int k = 0; k < 3; k++) v.push_back(a[k]);
    }
  }
  unsigned vbo;
  glCreateBuffers(1, &vbo);
  glNamedBufferStorage(vbo, v.size() * 4, v.data(), 0);
  glCreateVertexArrays(1, &gfx.cubeVao);
  attribs(gfx.cubeVao, vbo);
  // A cylinder along z, 1 across and 1 long, with flat ends: the saber blade.
  {
    constexpr int kSides = 20;
    std::vector<float> cv;
    auto vert = [&](float x, float y, float z, float nx, float ny, float nz) {
      for (float f : {x, y, z, nx, ny, nz}) cv.push_back(f);
    };
    for (int i = 0; i < kSides; i++) {
      const float a0 = 6.2831853f * i / kSides, a1 = 6.2831853f * (i + 1) / kSides;
      const float x0 = std::cos(a0), y0 = std::sin(a0), x1 = std::cos(a1), y1 = std::sin(a1);
      vert(x0 * 0.5f, y0 * 0.5f, -0.5f, x0, y0, 0), vert(x1 * 0.5f, y1 * 0.5f, -0.5f, x1, y1, 0), vert(x1 * 0.5f, y1 * 0.5f, 0.5f, x1, y1, 0);
      vert(x0 * 0.5f, y0 * 0.5f, -0.5f, x0, y0, 0), vert(x1 * 0.5f, y1 * 0.5f, 0.5f, x1, y1, 0), vert(x0 * 0.5f, y0 * 0.5f, 0.5f, x0, y0, 0);
      for (float zc : {-0.5f, 0.5f}) {
        const float nz = zc < 0 ? -1.0f : 1.0f;
        vert(0, 0, zc, 0, 0, nz), vert(x0 * 0.5f, y0 * 0.5f, zc, 0, 0, nz), vert(x1 * 0.5f, y1 * 0.5f, zc, 0, 0, nz);
      }
    }
    unsigned cvbo;
    glCreateBuffers(1, &cvbo);
    glNamedBufferStorage(cvbo, cv.size() * 4, cv.data(), 0);
    glCreateVertexArrays(1, &gfx.cylVao);
    attribs(gfx.cylVao, cvbo);
  }
  // The arrow: a chevron pointing up (+y) in the z = 0 plane, facing +z.
  std::vector<float> tri;
  for (int side = -1; side <= 1; side += 2) {
    const float p[4][2] = {{0, 0.5f}, {side * 0.5f, 0.0f}, {side * 0.5f, -0.25f}, {0, 0.25f}};
    for (int k : {0, 1, 2, 0, 2, 3})
      for (float f : {p[k][0], p[k][1] - 0.125f, 0.0f, 0.0f, 0.0f, 1.0f}) tri.push_back(f);
  }
  unsigned tvbo;
  glCreateBuffers(1, &tvbo);
  glNamedBufferStorage(tvbo, tri.size() * 4, tri.data(), 0);
  glCreateVertexArrays(1, &gfx.triVao);
  attribs(gfx.triVao, tvbo);
  glCreateBuffers(1, &gfx.lineVbo);
  glNamedBufferStorage(gfx.lineVbo, 4 << 20, nullptr, GL_DYNAMIC_STORAGE_BIT);
  glCreateVertexArrays(1, &gfx.lineVao);
  attribs(gfx.lineVao, gfx.lineVbo);
  return true;
}

// non-uniform scale matrix.
Mat4 scale(float x, float y, float z) {
  Mat4 s;
  s.m[0] = x, s.m[5] = y, s.m[10] = z;
  return s;
}
// rotation about +z by a radians (counter-clockwise seen from +z looking at -z).
Mat4 rotZ(float a) {
  Mat4 r;
  r.m[0] = std::cos(a), r.m[1] = std::sin(a), r.m[4] = -std::sin(a), r.m[5] = std::cos(a);
  return r;
}

// draw a mesh: colour c, lit 0..1 = how much directional shading, fog 0..1 = mix toward fogColor (distance cue for notes).
void solid(const Mat4 &vp, const Mat4 &model, unsigned vao, int verts, const float c[3], float lit, float fog, const float fogColor[3]) {
  glUseProgram(gfx.solid);
  glUniformMatrix4fv(gfx.vp, 1, GL_FALSE, vp.m);
  glUniformMatrix4fv(gfx.model, 1, GL_FALSE, model.m);
  glUniform3fv(gfx.color, 1, c);
  glUniform1f(gfx.lit, lit);
  glUniform1f(gfx.fog, fog);
  glUniform3fv(gfx.fogColor, 1, fogColor);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES, 0, verts);
}

// Cut direction to the arrow's angle (counter-clockwise from up).
// dir code 0-7 -> radians for the arrow marker texture (0 up, ccw positive). other codes -> 0.
float arrowAngle(int dir) {
  static const float deg[8] = {0, 180, 90, -90, 45, -45, 135, -135};
  return dir >= 0 && dir < 8 ? deg[dir] * 3.14159265f / 180 : 0;
}


// batch of coloured line segments (6 floats per vertex: pos, colour); flushed with one draw.
struct Lines {
  std::vector<float> v;
  void add(float ax, float ay, float az, float bx, float by, float bz, const float c[3]) {
    const float p[12] = {ax, ay, az, c[0], c[1], c[2], bx, by, bz, c[0], c[1], c[2]};
    v.insert(v.end(), p, p + 12);
  }
};

// one colour scheme. note/glow/core per HAND (index = hand(c) so 'swap hands' is a lookup). frame[]: white frame behind a block
// (high-contrast black block); mono: greys only. add schemes by appending to kPalettes (UI cycles paletteCount()).
struct Palette {
  const char *name;
  float note[2][3];   // block colour per hand
  float glow[2][3];   // saber glow
  float core[2][3];   // saber core
  float coreW[2];     // saber core width
  bool frame[2];      // a white frame behind the block, to tell a black block from the background
  bool mono;          // greys only: the background loses its tint
};

const Palette kPalettes[] = {
    {"DEFAULT", {{0.95f, 0.15f, 0.2f}, {0.2f, 0.45f, 1.0f}}, {{1.0f, 0.2f, 0.3f}, {0.3f, 0.6f, 1.0f}}, {{1.0f, 0.6f, 0.65f}, {0.65f, 0.8f, 1.0f}}, {0.026f, 0.026f}, {false, false}, false},
    {"COLOUR BLIND", {{1.0f, 0.6f, 0.0f}, {0.15f, 0.45f, 1.0f}}, {{1.0f, 0.6f, 0.0f}, {0.2f, 0.5f, 1.0f}}, {{1.0f, 0.85f, 0.5f}, {0.6f, 0.78f, 1.0f}}, {0.026f, 0.026f}, {false, false}, false},
    {"TRITAN", {{1.0f, 0.25f, 0.4f}, {0.0f, 0.85f, 0.7f}}, {{1.0f, 0.25f, 0.4f}, {0.0f, 0.9f, 0.75f}}, {{1.0f, 0.7f, 0.75f}, {0.6f, 1.0f, 0.95f}}, {0.026f, 0.026f}, {false, false}, false},
    {"HIGH CONTRAST", {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f}, {0.9f, 0.9f, 0.9f}}, {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}, {0.026f, 0.05f}, {false, true}, true},
};
constexpr int kPaletteCount = sizeof kPalettes / sizeof kPalettes[0];
int paletteId = 0;
bool paletteSwap = false;

// current palette.
const Palette &pal() { return kPalettes[paletteId]; }
// colour index c (0 left/red) -> palette slot, honouring the swap-hands option.
int hand(int c) { return paletteSwap ? 1 - c : c; }
const float *noteColour(int c) { return pal().note[hand(c)]; }
// pick a dark marker on light blocks and a white one on dark blocks (luminance .3R+.59G+.11B > .55).
bool darkMarker(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2] > 0.55f; }

// One block with its cut marker on the face turned to the player, centred where `base` puts it.
void noteCube(const Mat4 &vp, const Mat4 &base, float size, int color, int dir, float fog, const float bg[3]) {
  const float dark[3] = {0.02f, 0.02f, 0.03f}, white[3] = {1, 1, 1};
  const float *body = noteColour(color), *mark = darkMarker(body) ? dark : white;
  if (pal().frame[hand(color)]) solid(vp, base * scale(size * 1.2f, size * 1.2f, size * 0.9f), gfx.cubeVao, 36, white, 0.0f, fog, bg);
  solid(vp, base * scale(size, size, size), gfx.cubeVao, 36, body, pal().mono ? 0.0f : 1.0f, fog, bg);
  if (dir >= 9) {
    static const float deg[4] = {90, 0, 45, -45};
    solid(vp, base * translation(0, 0, size * 0.52f) * rotZ(deg[dir - 9] * 3.14159265f / 180) * scale(size * 0.8f, size * 0.12f, 0.001f), gfx.cubeVao, 36, mark, 0.0f, fog, bg);
  } else if (dir == 8) {
    solid(vp, base * translation(0, 0, size * 0.52f) * scale(size * 0.3f, size * 0.3f, 0.001f), gfx.cubeVao, 36, mark, 0.0f, fog, bg);
  } else {
    solid(vp, base * translation(0, 0, size * 0.51f) * rotZ(arrowAngle(dir)) * scale(size * 0.8f, size * 0.8f, 1), gfx.triVao, 12, mark, 0.0f, fog, bg);
  }
}

// How far in front of the eye plane a point is (metres): clip-space w of the perspective projection, so it follows the head's TURN as well as
// its position. ! GL does not clip at the eye plane on the console: a triangle or line with any corner behind it vanishes whole, so anything
// big near the head (long ring edges, lane and beat lines, a block or shard passing the head) is cut into short pieces and dropped when one
// is nearer than a margin. ref: the menu plate (tiled for the same reason).
float eyeDist(const Mat4 &vp, float x, float y, float z) { return vp.m[3] * x + vp.m[7] * y + vp.m[11] * z + vp.m[15]; }

// one note in flight: position from (lane,row,time-now)*speed, grows in over its first 0.25 s, fogs with distance, culled
// beyond the horizon, past hitZ+0.12 s, and when any of it would pass s.maxZ (so nothing flies through the headset at the eye plane).
void noteBox(const mapper::Note &n, const game::Config &cfg, const Scene &s, const Mat4 &vp, const float bg[3]) {
  const double dt = n.time - s.now;
  const double horizon = (80.0 + cfg.hitZ) / s.speed;
  if (dt > horizon || dt < -0.12) return;
  const float x = (n.lane - 1.5f) * cfg.laneW, y = cfg.row0 + n.row * cfg.rowH, z = cfg.hitZ - (float)dt * s.speed;
  float grow = (float)std::min(1.0, (horizon - dt) / 0.25), size = cfg.noteSize * (0.3f + 0.7f * grow);
  if (z + 0.5f * size > s.maxZ || eyeDist(vp, x, y, z) < 0.9f * size + 0.15f) return;  // the whole block must stay in front of the eye plane (see eyeDist)
  float fog = std::min(1.0f, (float)(dt / horizon)) * 0.85f;
  noteCube(vp, translation(x, y, z), size, n.color, n.dir, fog, bg);
}

// Text is textured quads from the baked atlas. `em` is the height of the font's em square in metres.
// vertices (x y z u v r g b = 8 floats) of text quads, drawn by flushText in one call.
struct TextBatch {
  std::vector<float> v;
};

// Plain ASCII, plus Cyrillic folded into bytes 0x80..0xC1 (А..я, then Ё and ё) that follow ASCII in the atlas.
int glyphIndex(unsigned char c) {
  if (c >= fontatlas::kFirst && c < 127) return c - fontatlas::kFirst;
  if (c >= 0x80 && c <= 0xC1 && 95 + (c - 0x80) < fontatlas::kCount) return 95 + (c - 0x80);
  return '?' - fontatlas::kFirst;
}

// Other non-ASCII UTF-8 sequences become one '?'.
std::string plain(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    const unsigned char c = s[i];
    if (c < 0x80) out += (char)c;
    else if (c == 0xD0 && i + 1 < s.size()) {
      const unsigned char d = s[++i];
      out += d == 0x81 ? (char)0xC0 : d >= 0x90 && d <= 0xBF ? (char)(0x80 + d - 0x90) : '?';
    } else if (c == 0xD1 && i + 1 < s.size()) {
      const unsigned char d = s[++i];
      out += d == 0x91 ? (char)0xC1 : d >= 0x80 && d <= 0x8F ? (char)(0x80 + 48 + d - 0x80) : '?';
    } else if (c >= 0xC0) out += '?';
  }
  return out;
}

// width in metres of `str` at em-square height `em` (sum of baked advances). em = size*8 for Row sizes.
float textWidth(const std::string &str, float em) {
  float w = 0;
  for (unsigned char c : plain(str)) w += fontatlas::kAdvance[glyphIndex(c)];
  return w * em / fontatlas::kEm;
}

// appends centred text at (cx,cy,z); baseline sits 0.36 em below cy. glyphs via plain()/glyphIndex (ASCII + Cyrillic).
void addText(TextBatch &b, const std::string &str, float cx, float cy, float z, float em, const float colour[3]) {
  const float k = em / fontatlas::kEm;
  float x = cx - textWidth(str, em) / 2;
  const float baseline = cy - 0.36f * em;
  for (unsigned char c : plain(str)) {
    const int g = glyphIndex(c);
    const float x0 = x - fontatlas::kPad * k, x1 = x0 + fontatlas::kCellW * k;
    const float y1 = baseline + fontatlas::kBase * k, y0 = y1 - fontatlas::kCellH * k;
    const float u0 = (float)(g % fontatlas::kCols * fontatlas::kCellW) / fontatlas::kW, u1 = u0 + (float)fontatlas::kCellW / fontatlas::kW;
    const float v0 = (float)(g / fontatlas::kCols * fontatlas::kCellH) / fontatlas::kH, v1 = v0 + (float)fontatlas::kCellH / fontatlas::kH;
    const float q[6][5] = {{x0, y0, z, u0, v1}, {x1, y0, z, u1, v1}, {x1, y1, z, u1, v0}, {x0, y0, z, u0, v1}, {x1, y1, z, u1, v0}, {x0, y1, z, u0, v0}};
    for (const auto &p : q) {
      b.v.insert(b.v.end(), p, p + 5);
      b.v.insert(b.v.end(), colour, colour + 3);
    }
    x += fontatlas::kAdvance[g] * k;
  }
}

bool haveArt = false;

// The cover picture as a square of side `side` centred at (cx, cy).
void drawArt(const Mat4 &vp, float cx, float cy, float z, float side, const float bg[3]) {
  if (!haveArt) {
    const float dark[3] = {0.09f, 0.1f, 0.17f};
    solid(vp, translation(cx, cy, z) * scale(side, side, 0.004f), gfx.cubeVao, 36, dark, 0.0f, 0.0f, bg);
    return;
  }
  const float x0 = cx - side / 2, x1 = cx + side / 2, y0 = cy - side / 2, y1 = cy + side / 2, w[3] = {1, 1, 1};
  const float q[6][5] = {{x0, y0, z, 0, 1}, {x1, y0, z, 1, 1}, {x1, y1, z, 1, 0}, {x0, y0, z, 0, 1}, {x1, y1, z, 1, 0}, {x0, y1, z, 0, 0}};
  float v[6 * 8];
  for (int i = 0; i < 6; i++) {
    std::memcpy(v + i * 8, q[i], 20);
    std::memcpy(v + i * 8 + 5, w, 12);
  }
  glNamedBufferSubData(gfx.textVbo, 0, sizeof v, v);
  glUseProgram(gfx.artProg);
  glUniformMatrix4fv(glGetUniformLocation(gfx.artProg, "vp"), 1, GL_FALSE, vp.m);
  glBindTextureUnit(0, gfx.artTex);
  glBindVertexArray(gfx.textVao);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}

// uploads and draws the batch with alpha blending, depth test on, depth write off; clears the batch.
void flushText(const Mat4 &vp, TextBatch &b) {
  if (b.v.empty()) return;
  const size_t bytes = std::min(b.v.size() * 4, kTextBytes);
  glNamedBufferSubData(gfx.textVbo, 0, bytes, b.v.data());
  glUseProgram(gfx.textProg);
  glUniformMatrix4fv(gfx.textVp, 1, GL_FALSE, vp.m);
  glBindTextureUnit(0, gfx.atlas);
  glBindVertexArray(gfx.textVao);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glDrawArrays(GL_TRIANGLES, 0, (int)(bytes / 32));
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  b.v.clear();
}

}  // namespace

namespace {

// Rows wider than the headset's comfortable view shrink to fit.
constexpr float kMaxTextWidth = 2.4f;

// scale (<=1) that keeps a row within kMaxTextWidth (2.4 m): wide rows shrink instead of overflowing the headset's view.
float fitScale(const Row &r) {
  const float w = textWidth(r.text, r.size * 8);
  return w > kMaxTextWidth ? kMaxTextWidth / w : 1.0f;
}

}  // namespace

// panel extent in metres from its rows: height .3 + sum(size*12), width max(1.2, widest row*fit + .4), +1 when a preview is shown.
// also used by main.cpp to place the flat-view camera.
void panelSize(const Panel &pn, float &width, float &height) {
  height = 0.3f, width = 1.2f;
  for (const Row &r : pn.rows) {
    height += r.size * 12;
    width = std::max(width, textWidth(r.text, r.size * 8) * fitScale(r) + 0.4f);
  }
  if (pn.preview) width += 1.0f;
}

namespace {

// menu panel 1.6 m ahead of the origin at y=1.5: tiled backing plate, optional preview cubes, art rows, text rows. rows are
// {text,size,style}: style 0 gold title, 1 pale, 2 white (selected), 3 grey, 4 green, 5 red.
void drawPanel(const Mat4 &vp, const Panel &pn, const float bg[3]) {
  static const float colour[6][3] = {{1.0f, 0.8f, 0.2f}, {0.75f, 0.8f, 0.92f}, {1, 1, 1}, {0.5f, 0.54f, 0.68f}, {0.3f, 0.95f, 0.5f}, {1.0f, 0.4f, 0.4f}};
  const float z = -1.6f, cy = 1.5f, back[3] = {0.04f, 0.05f, 0.09f};
  float height, width;
  panelSize(pn, width, height);
  // Tiles, not one big slab: the console's GL does not clip at the eye plane, so a triangle that reaches behind the
  // eye when the head is turned makes the whole slab vanish in that eye.
  const int tx = std::max(1, (int)std::ceil(width / 0.3f)), ty = std::max(1, (int)std::ceil(height / 0.3f));
  for (int i = 0; i < tx; i++)
    for (int j = 0; j < ty; j++) {
      const float w = width / tx, h = height / ty;
      solid(vp, translation(-width / 2 + (i + 0.5f) * w, cy - height / 2 + (j + 0.5f) * h, z - 0.03f) * scale(w, h, 0.02f), gfx.cubeVao, 36, back, 0.0f, 0.0f, bg);
    }
  if (pn.preview) {
    const float px = width / 2 - 0.4f;
    noteCube(vp, translation(-px, cy, z), 0.4f, 0, 1, 0.0f, bg);
    noteCube(vp, translation(px, cy, z), 0.4f, 1, 8, 0.0f, bg);
  }
  TextBatch tb;
  float y = cy + height / 2 - 0.15f;
  for (const Row &r : pn.rows) {
    const float step = r.size * 12;
    if (r.art) drawArt(vp, 0, y - step / 2, z - 0.005f, step * 0.94f, bg);
    else addText(tb, r.text, 0, y - step / 2, z, r.size * 8 * fitScale(r), colour[std::clamp(r.style, 0, 5)]);
    y -= step;
  }
  flushText(vp, tb);
}

}  // namespace

namespace {

float lastBloom = 0.5f;

// smoothstep(a,b,x) clamped.
float smooth01(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// What the music is doing now, for the background. E is the energy arc of the whole song: a quiet start that
// builds to a peak a little over a third of the way in, holds, and eases off over the last stretch.
struct Mood {
  float E = 0.4f, bass = 0.5f, mid = 0.4f, high = 0.4f, beat = 0, hue = 0.58f, time = 0;
  int sides = 5;
};

// background drivers at s.now: E energy arc (quiet start, builds to ~38% of the song, plateau, eases off after 84%) scaled by
// bar intensity; bass/mid/high and beat pulse interpolated from the per-beat cues; hue by section; ring sides 3 + section%4.
// no session (menus): a gentle idle cycle from s.clock.
Mood moodAt(const Scene &s) {
  Mood m;
  if (!(s.session && s.song) || s.song->duration <= 0) {
    m.time = (float)s.clock;
    m.beat = std::exp(-5.0f * (float)std::fmod(s.clock * 1.7, 1.0));
    m.hue = 0.58f + (float)s.clock * 0.01f;
    return m;
  }
  const mapper::Song &song = *s.song;
  m.time = (float)s.now;
  const float p = (float)std::clamp(s.now / song.duration, 0.0, 1.0);
  const float arc = (0.18f + 0.82f * smooth01(0.0f, 0.38f, p)) * (1.0f - 0.82f * smooth01(0.84f, 1.0f, p));
  float intensity = 0.7f;
  int section = 1;
  for (const auto &b : song.bars)
    if (s.now >= b.start && s.now < b.end) {
      intensity = b.intensity, section = (int)b.section;
      break;
    }
  m.E = arc * (0.6f + 0.4f * intensity);
  static const float hues[6] = {0.6f, 0.55f, 0.12f, 0.0f, 0.42f, 0.78f};
  m.hue = hues[std::clamp(section, 0, 5)] + (float)s.now * 0.004f;
  m.sides = 3 + section % 4;
  const auto &beats = song.beats;
  const auto it = std::upper_bound(beats.begin(), beats.end(), s.now);
  if (it != beats.begin() && it != beats.end() && song.cues.size() == beats.size()) {
    const size_t i = (size_t)(it - beats.begin()) - 1;
    const float phase = (float)((s.now - beats[i]) / (beats[i + 1] - beats[i]));
    const auto &c = song.cues[i], &q = song.cues[i ? i - 1 : 0];
    const float k = smooth01(0.0f, 0.3f, phase);
    m.bass = q.bass + (c.bass - q.bass) * k, m.mid = q.mid + (c.mid - q.mid) * k, m.high = q.high + (c.high - q.high) * k;
    m.beat = std::exp(-5.0f * phase);
  }
  return m;
}

// fullscreen background pass for the current view (reads the inverse of vp for ray directions). fractal weight rises with
// energy and with level (0.6 + 0.15*diff); s.bg>1 = 48 iterations (flat view) else 22; geo=0 when real 3D rings are drawn.
// also sets lastBloom (bloom strength for postEnd).
void drawBackground(const Mat4 &vp, const Scene &s) {
  const Mood m = moodAt(s);
  float fractal = 0.45f;
  if (s.session) fractal = (0.6f + 0.15f * s.diff) * smooth01(0.2f, 0.55f, m.E);
  lastBloom = 0.4f + 0.3f * m.E + 0.25f * m.beat * m.bass;
  glDisable(GL_DEPTH_TEST);
  glUseProgram(gfx.bgProg);
  glUniformMatrix4fv(glGetUniformLocation(gfx.bgProg, "vp"), 1, GL_FALSE, vp.m);
  auto f = [&](const char *n, float v) { glUniform1f(glGetUniformLocation(gfx.bgProg, n), v); };
  f("t", m.time), f("E", m.E), f("bass", m.bass), f("mid", m.mid), f("high", m.high), f("beat", m.beat), f("hue", m.hue), f("fracW", fractal),
      f("quality", (float)s.bg), f("geo", s.bg3d ? 0.0f : 1.0f);
  glUniform1i(glGetUniformLocation(gfx.bgProg, "sides"), m.sides);
  glBindVertexArray(gfx.emptyVao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glEnable(GL_DEPTH_TEST);
}

// The same neon rings as real geometry: polygon outlines in the scene that fly toward the player, around the
// play area, so in the headset they pass by with proper depth.
void drawRings(const Mat4 &vp, const Scene &s, const float bg[3]) {
  const Mood m = moodAt(s);
  const int layers = 3 + (int)(m.E * 9.0f);
  const float speed = 5.0f + 15.0f * m.E, span = 73.0f, zMax = std::min(1.0f, s.maxZ - 0.3f);
  struct Edge {
    Mat4 model;
    float colour[3];
  };
  std::vector<Edge> edges;
  for (int i = 0; i < layers; i++) {
    const float f = (float)std::fmod(m.time * speed / span + (float)i / layers + 10.0, 1.0);
    const float z = -70.0f + span * f;
    if (z > zMax) continue;
    const float fade = smooth01(0.0f, 0.12f, f) * (1.0f - smooth01(0.85f, 1.0f, f));
    const float k = fade * (0.35f + 0.65f * m.E + 0.35f * m.beat * m.bass);
    const float h = m.hue + 0.06f * i + 0.15f * f;
    float c[3];
    for (int a = 0; a < 3; a++) c[a] = std::pow(0.5f + 0.5f * std::cos(6.2831853f * (h + a * 0.33f)), 1.6f) * k;
    const float r = 5.0f * (1.0f + 0.08f * m.beat * m.bass), spin = -(m.time * 0.12f * (0.4f + m.E) + i * 0.55f + f * 1.1f);
    for (int e = 0; e < m.sides; e++) {
      const float a0 = spin + 6.2831853f * e / m.sides, a1 = spin + 6.2831853f * (e + 1) / m.sides;
      const float x0 = r * std::sin(a0), y0 = 1.4f + r * std::cos(a0), x1 = r * std::sin(a1), y1 = 1.4f + r * std::cos(a1);
      const float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)), ang = std::atan2(y1 - y0, x1 - x0);
      // An edge that is clearly in front goes in whole; one that nears the eye plane (a ring passing the head, or the head turned
      // toward its far side) is cut into 0.4 m pieces and the pieces too close to that plane are dropped.
      if (eyeDist(vp, x0, y0, z) > 0.8f && eyeDist(vp, x1, y1, z) > 0.8f) {
        edges.push_back({translation((x0 + x1) / 2, (y0 + y1) / 2, z) * rotZ(ang) * scale(len + 0.08f, 1, 1), {c[0], c[1], c[2]}});
        continue;
      }
      const int pieces = std::max(1, (int)std::ceil(len / 0.4f));
      for (int k = 0; k < pieces; k++) {
        const float u = (k + 0.5f) / pieces, px = x0 + (x1 - x0) * u, py = y0 + (y1 - y0) * u;
        if (eyeDist(vp, px, py, z) < 0.5f) continue;
        edges.push_back({translation(px, py, z) * rotZ(ang) * scale(len / pieces + 0.08f, 1, 1), {c[0], c[1], c[2]}});
      }
    }
  }
  glEnable(GL_DEPTH_TEST);
  for (const Edge &e : edges) solid(vp, e.model * scale(1, 0.05f, 0.05f), gfx.cubeVao, 36, e.colour, 0.0f, 0.0f, bg);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glDepthMask(GL_FALSE);
  for (const Edge &e : edges) {
    const float g[3] = {e.colour[0] * 0.3f, e.colour[1] * 0.3f, e.colour[2] * 0.3f};
    solid(vp, e.model * scale(1, 0.2f, 0.2f), gfx.cubeVao, 36, g, 0.0f, 0.0f, bg);
  }
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
}

// The window-sized floating-point target the scene is drawn into, and the chain of smaller ones the glow is blurred in.
struct Post {
  unsigned fbo = 0, scene = 0, depth = 0, mip[5] = {}, mipFbo[5] = {};
  int w = 0, h = 0;
} post;

// RGBA16F colour target + framebuffer, linear filtered, clamped. returns the texture.
unsigned makeTarget(int w, int h, unsigned &fbo) {
  unsigned tex;
  glCreateTextures(GL_TEXTURE_2D, 1, &tex);
  glTextureStorage2D(tex, 1, GL_RGBA16F, w, h);
  glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glCreateFramebuffers(1, &fbo);
  glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
  return tex;
}

// deletes all bloom targets (on resize).
void freePost() {
  if (!post.fbo) return;
  glDeleteFramebuffers(1, &post.fbo);
  glDeleteTextures(1, &post.scene);
  glDeleteRenderbuffers(1, &post.depth);
  for (int i = 0; i < 5; i++) glDeleteFramebuffers(1, &post.mipFbo[i]), glDeleteTextures(1, &post.mip[i]);
  post = Post();
}

}  // namespace

// binds the HDR scene target (recreated if the size changed) for drawing; pair with postEnd().
bool postBegin(int w, int h) {
  if (post.w != w || post.h != h) {
    freePost();
    post.w = w, post.h = h;
    post.scene = makeTarget(w, h, post.fbo);
    glCreateRenderbuffers(1, &post.depth);
    glNamedRenderbufferStorage(post.depth, GL_DEPTH_COMPONENT32F, w, h);
    glNamedFramebufferRenderbuffer(post.fbo, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, post.depth);
    for (int i = 0; i < 5; i++) post.mip[i] = makeTarget(std::max(1, w >> (i + 1)), std::max(1, h >> (i + 1)), post.mipFbo[i]);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, post.fbo);
  glViewport(0, 0, w, h);
  return true;
}

// bloom: bright-pass+downsample chain (5 levels), upsample-add back up, then composite scene + bloom*lastBloom with a shoulder
// tone map to the default framebuffer. no-op if postBegin was never called.
void postEnd() {
  if (!post.fbo) return;
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(gfx.emptyVao);
  auto size = [&](int i, int &w, int &h) { w = std::max(1, post.w >> (i + 1)), h = std::max(1, post.h >> (i + 1)); };
  glUseProgram(gfx.downProg);
  glUniform1i(glGetUniformLocation(gfx.downProg, "src"), 0);
  for (int i = 0; i < 5; i++) {
    int w, h, sw = post.w, sh = post.h;
    size(i, w, h);
    if (i) size(i - 1, sw, sh);
    glBindFramebuffer(GL_FRAMEBUFFER, post.mipFbo[i]);
    glViewport(0, 0, w, h);
    glBindTextureUnit(0, i ? post.mip[i - 1] : post.scene);
    glUniform2f(glGetUniformLocation(gfx.downProg, "texel"), 1.0f / sw, 1.0f / sh);
    glUniform1f(glGetUniformLocation(gfx.downProg, "threshold"), i ? 0.0f : 0.85f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glUseProgram(gfx.upProg);
  glUniform1i(glGetUniformLocation(gfx.upProg, "src"), 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  for (int i = 4; i >= 1; i--) {
    int w, h, sw, sh;
    size(i - 1, w, h);
    size(i, sw, sh);
    glBindFramebuffer(GL_FRAMEBUFFER, post.mipFbo[i - 1]);
    glViewport(0, 0, w, h);
    glBindTextureUnit(0, post.mip[i]);
    glUniform2f(glGetUniformLocation(gfx.upProg, "texel"), 1.0f / sw, 1.0f / sh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glDisable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, post.w, post.h);
  glUseProgram(gfx.compProg);
  glUniform1i(glGetUniformLocation(gfx.compProg, "scene"), 0);
  glUniform1i(glGetUniformLocation(gfx.compProg, "bloom"), 1);
  glUniform1f(glGetUniformLocation(gfx.compProg, "strength"), lastBloom);
  glBindTextureUnit(0, post.scene);
  glBindTextureUnit(1, post.mip[0]);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glEnable(GL_DEPTH_TEST);
}

bool init() { return initGfx(); }

// head-locked message: a plate 1.1 m ahead of pose (p,q) (scene space, from the headset midpoint) and 0.34 m below the eye
// line, two text lines, no depth test (always on top). used for 'PRESS R3 TO CENTRE THE MENU' before the first recentre.
void hint(const Mat4 &vp, const float p[3], const float q[4], const char *line1, const char *line2) {
  const Mat4 m = translation(p[0], p[1], p[2]) * rotation(q) * translation(0, -0.34f, -1.1f);
  const float back[3] = {0.04f, 0.05f, 0.09f}, gold[3] = {1.0f, 0.8f, 0.2f}, white[3] = {0.9f, 0.92f, 1.0f}, none[3] = {0, 0, 0};
  glDisable(GL_DEPTH_TEST);
  solid(vp, m * translation(0, 0, -0.01f) * scale(1.05f, 0.24f, 0.004f), gfx.cubeVao, 36, back, 0.0f, 0.0f, none);
  TextBatch tb;
  addText(tb, line1, 0, 0.045f, 0, 0.06f, gold);
  addText(tb, line2, 0, -0.045f, 0, 0.034f, white);
  flushText(vp * m, tb);
  glEnable(GL_DEPTH_TEST);
}

// song cover as kArtSize^2 RGB; null clears (a dark placeholder is drawn). haveArt tracks which.
void setArt(const uint8_t *rgb) {
  haveArt = rgb != nullptr;
  if (!rgb) return;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTextureSubImage2D(gfx.artTex, 0, 0, 0, 256, 256, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}

// selects the colour scheme and hand swap used by every later draw.
void setPalette(int id, bool swap) {
  paletteId = std::clamp(id, 0, kPaletteCount - 1);
  paletteSwap = swap;
}
int paletteCount() { return kPaletteCount; }
const char *paletteName(int id) { return kPalettes[std::clamp(id, 0, kPaletteCount - 1)].name; }

// clear/fog colour: near-black, tinted by the current section and bar intensity (greys in the mono palette) so fog and clears match.
void background(const Scene &s, float bg[3]) {
  const float tints[6][3] = {{0.10f, 0.14f, 0.28f}, {0.12f, 0.12f, 0.16f}, {0.35f, 0.25f, 0.04f}, {0.45f, 0.08f, 0.08f}, {0.05f, 0.30f, 0.22f}, {0.2f, 0.12f, 0.3f}};
  bg[0] = 0.03f, bg[1] = 0.03f, bg[2] = 0.05f;
  if (!s.song) return;
  for (const auto &b : s.song->bars)
    if (s.now >= b.start && s.now < b.end) {
      const float k = 0.08f + 0.22f * b.intensity;
      for (int i = 0; i < 3; i++) bg[i] = pal().mono ? 0.12f * k : tints[(int)b.section][i] * k;
      break;
    }
}

// one view of the whole scene. caller clears and sets viewport/framebuffer. see file header for draw order and hazards.
void draw(const Mat4 &vp, const Scene &s, const float bg[3]) {
  const game::Config cfg = s.session ? s.session->config() : game::Config();
  if (s.bg) drawBackground(vp, s);
  glEnable(GL_DEPTH_TEST);
  if (s.bg && s.bg3d) drawRings(vp, s, bg);
  Lines lines;
  const float dim[3] = {0.18f, 0.2f, 0.28f}, bright[3] = {0.55f, 0.6f, 0.8f}, bar4[3] = {0.9f, 0.9f, 1.0f};
  const float kHitZ = cfg.hitZ, kLaneW = cfg.laneW;
  // line pieces: a line with an end behind the eye plane vanishes whole, so long and wide lines are cut up and the near pieces dropped
  auto addLine = [&](float ax, float az, float bx, float bz, const float col[3], float piece) {
    const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    const int n = std::max(1, (int)std::ceil(len / piece));
    for (int k = 0; k < n; k++) {
      const float u0 = (float)k / n, u1 = (float)(k + 1) / n;
      const float x0 = ax + (bx - ax) * u0, z0 = az + (bz - az) * u0, x1 = ax + (bx - ax) * u1, z1 = az + (bz - az) * u1;
      if (eyeDist(vp, x0, 0, z0) < 0.3f || eyeDist(vp, x1, 0, z1) < 0.3f) continue;
      lines.add(x0, 0, z0, x1, 0, z1, col);
    }
  };
  for (int i = 0; i <= 4; i++) addLine((i - 2) * kLaneW, kHitZ, (i - 2) * kLaneW, -80, dim, 4.0f);
  if (s.song)
    for (size_t i = 0; i < s.song->beats.size(); i++) {
      const double dt = s.song->beats[i] - s.now;
      if (dt < -0.1 || dt > 80.0 / s.speed) continue;
      const float z = kHitZ - (float)dt * s.speed, fade = 1 - (float)(dt * s.speed / 80);
      if (z > s.maxZ) continue;
      const bool down = (int)i % 4 == s.downbeatPhase;
      const float c[3] = {(down ? bar4[0] : dim[0]) * fade, (down ? bar4[1] : dim[1]) * fade, (down ? bar4[2] : dim[2]) * fade};
      addLine(-2 * kLaneW, z, 2 * kLaneW, z, c, 0.25f);
    }
  if (s.panel) lines.v.clear();
  glNamedBufferSubData(gfx.lineVbo, 0, std::min(lines.v.size() * 4, (size_t)(4 << 20)), lines.v.data());
  glUseProgram(gfx.line);
  glUniformMatrix4fv(gfx.lineVp, 1, GL_FALSE, vp.m);
  glBindVertexArray(gfx.lineVao);
  glDrawArrays(GL_LINES, 0, (int)lines.v.size() / 6);

  // Before the sabers: their glow does not write depth, so anything opaque drawn after it would cover it.
  if (s.panel) drawPanel(vp, *s.panel, bg);

  // Notes still to be cut, far ones first so near ones draw over them.
  static const std::vector<mapper::Note> none;
  const std::vector<mapper::Note> &notes = s.session ? s.session->notes() : s.song ? s.song->maps[s.diff].notes : none;
  const auto first = std::lower_bound(notes.begin(), notes.end(), s.now - 0.2, [](const mapper::Note &n, double v) { return n.time < v; });
  std::vector<const mapper::Note *> visible;
  for (auto it = first; it != notes.end() && it->time - s.now < 80.0 / s.speed; ++it)
    if (!s.session || s.session->state(it - notes.begin()) == game::NoteState::Pending) visible.push_back(&*it);
  for (auto it = visible.rbegin(); it != visible.rend(); ++it) noteBox(**it, cfg, s, vp, bg);

  // Shards of cut notes, drifting a little toward the player and shrinking.
  static const float corner[8][3] = {{1, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {-1, -1, 1}, {1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}};
  const float grey3[3] = {0.5f, 0.5f, 0.5f}, dimGrey[3] = {0.45f, 0.45f, 0.45f};
  const float *shardColour[3] = {noteColour(0), noteColour(1), grey3};
  if (pal().mono) shardColour[hand(1)] = dimGrey;
  if (s.effects)
    for (const Fx &f : *s.effects) {
      const float age = (float)(s.now - f.t);
      if (age < 0 || age > 0.45f) continue;
      const float size = 0.14f * (1 - age / 0.45f);
      for (const auto &c : corner) {
        const float z = f.pos.z + c[2] * age * 0.8f + age * 1.2f;
        const float sx = f.pos.x + c[0] * age * 1.4f, sy = f.pos.y + c[1] * age * 1.4f - age * age * 2;
        if (z + size > s.maxZ || eyeDist(vp, sx, sy, z) < 1.8f * size + 0.15f) continue;  // as for blocks: a shard poking behind the eye plane would vanish
        solid(vp, translation(f.pos.x + c[0] * age * 1.4f, f.pos.y + c[1] * age * 1.4f - age * age * 2, z) * scale(size, size, size), gfx.cubeVao, 36,
              shardColour[f.color], 1.0f, 0.0f, bg);
      }
    }

  // The sabers: a glowing blade from the hilt to the tip, along whatever way the controller points.
  const float grey[3] = {0.5f, 0.5f, 0.55f};
  if (s.sabers)
    for (int c = 0; c < 2; c++) {
      const game::Vec3 &b = s.sabers[c].base, &t = s.sabers[c].tip;
      const float dx = t.x - b.x, dy = t.y - b.y, dz = t.z - b.z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (len < 1e-4f) continue;
      // Orient a unit box along the blade: z axis to the blade direction.
      const float fz[3] = {dx / len, dy / len, dz / len};
      float up[3] = {0, 1, 0};
      if (std::fabs(fz[1]) > 0.95f) up[0] = 1, up[1] = 0;
      float fx[3] = {up[1] * fz[2] - up[2] * fz[1], up[2] * fz[0] - up[0] * fz[2], up[0] * fz[1] - up[1] * fz[0]};
      const float fl = std::sqrt(fx[0] * fx[0] + fx[1] * fx[1] + fx[2] * fx[2]);
      for (float &v : fx) v /= fl;
      const float fy[3] = {fz[1] * fx[2] - fz[2] * fx[1], fz[2] * fx[0] - fz[0] * fx[2], fz[0] * fx[1] - fz[1] * fx[0]};
      Mat4 r;
      for (int k = 0; k < 3; k++) r.m[k] = fx[k], r.m[4 + k] = fy[k], r.m[8 + k] = fz[k];
      const Mat4 at = translation((b.x + t.x) / 2, (b.y + t.y) / 2, (b.z + t.z) / 2) * r;
      const int kCylVerts = 20 * 12;
      // Wider additive shells that fade out for the glow, then the core on top.
      const int h = hand(c);
      const float *glowColour = pal().glow[h];
      if (!s.plainSabers) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        static const float shell[4][2] = {{0.045f, 0.55f}, {0.075f, 0.28f}, {0.12f, 0.14f}, {0.19f, 0.07f}};
        for (const auto &sh : shell) {
          const float col[3] = {glowColour[0] * sh[1], glowColour[1] * sh[1], glowColour[2] * sh[1]};
          solid(vp, at * scale(sh[0], sh[0], len + sh[0] * 0.5f), gfx.cylVao, kCylVerts, col, 0.0f, 0.0f, bg);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
      }
      const float boost = s.hdr ? 2.2f : 1.0f, hot[3] = {pal().core[h][0] * boost, pal().core[h][1] * boost, pal().core[h][2] * boost};
      solid(vp, at * scale(pal().coreW[h], pal().coreW[h], len), gfx.cylVao, kCylVerts, hot, 0.0f, 0.0f, bg);
      Mat4 hilt = translation(b.x + fz[0] * 0.08f, b.y + fz[1] * 0.08f, b.z + fz[2] * 0.08f) * r;
      solid(vp, hilt * scale(0.04f, 0.04f, 0.16f), gfx.cylVao, kCylVerts, grey, 1.0f, 0.0f, bg);
    }

  // A small HUD below the field: energy, the multiplier, and how far through the song.
  if (s.session) {
    const game::Stats &st = s.session->stats();
    const float cx = 0, y = 0.12f, z = -2.2f;
    const float green[3] = {0.2f, 0.9f, 0.4f}, red[3] = {0.9f, 0.2f, 0.2f}, back[3] = {0.12f, 0.12f, 0.16f}, gold[3] = {1.0f, 0.8f, 0.2f};
    solid(vp, translation(cx, y, z) * scale(2.0f, 0.05f, 0.02f), gfx.cubeVao, 36, back, 0.0f, 0.0f, bg);
    const float e = std::clamp(st.energy, 0.0f, 1.0f);
    solid(vp, translation(cx - 1.0f + e, y, z + 0.011f) * scale(2.0f * e, 0.05f, 0.02f), gfx.cubeVao, 36, e > 0.3f ? green : red, 0.0f, 0.0f, bg);
    const int pips = st.multiplier == 8 ? 4 : st.multiplier == 4 ? 3 : st.multiplier == 2 ? 2 : 1;
    for (int i = 0; i < pips; i++) solid(vp, translation(-0.15f + i * 0.1f, y + 0.12f, z) * scale(0.07f, 0.07f, 0.02f), gfx.cubeVao, 36, gold, 0.0f, 0.0f, bg);
    {
      TextBatch tb;
      char buf[48];
      const float white[3] = {1, 1, 1}, dimc[3] = {0.6f, 0.65f, 0.8f};
      std::snprintf(buf, sizeof buf, "%ld", st.score);
      addText(tb, buf, cx, y + 0.4f, z, 0.17f, white);
      if (st.combo > 1) {
        std::snprintf(buf, sizeof buf, "%dx COMBO", st.combo);
        addText(tb, buf, cx, y + 0.26f, z, 0.075f, dimc);
      }
      flushText(vp, tb);
    }
    if (s.song && s.song->duration > 0) {
      const float p = (float)std::clamp(s.now / s.song->duration, 0.0, 1.0);
      solid(vp, translation(-1.0f + p, y - 0.08f, z) * scale(2.0f * p, 0.015f, 0.02f), gfx.cubeVao, 36, bright, 0.0f, 0.0f, bg);
    }
  }
}

}  // namespace render
