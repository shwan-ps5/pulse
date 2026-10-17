// The eye buffers: the GL texture a game draws both eyes into, and the tiled memory the headset reads.
// The picture goes GL texture -> compute shader -> mapped buffer in the GPU's own tile order -> headset. The
// shader also flips the picture, because GL's rows run bottom-up and the headset's top-down. A game that draws
// into framebuffer() with each eye in its half and then calls fill() needs nothing more from this file.
#include "vr_eyes.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "gl.h"
#include "platform.h"

namespace vr_eyes {

namespace {

// The size now in use: EYE pixels square per eye, W x H for both side by side. make() may lower EYE.
int EYE = 2048, W = EYE * 2, H = EYE;

// GL's bottom-up rows into the headset's top-down ones, RGBA8 with red in the low byte, in the GPU's
// 64 KiB blocks of 128 x 128 texels (the boilerplate's tiled_byte_offset, as the VR test checks it).
const char *COPY_CS = R"(#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D uSrc;
layout(std430, binding = 1) writeonly buffer Dst { uint px[]; };
uniform int uWidth, uHeight;
// Byte offset of texel (x, y) inside the 64 KiB-block tiled layout: the bits of x and y are interleaved (XORed)
  // into the position within a 128 x 128 block, and blocks follow each other row by row. The same function runs
  // on the CPU in tiledOffsetCpu, for capture().
uint tiledOffset(uint x, uint y) {
  uint o = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^
           ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
  uint blocksPerRow = (uint(uWidth) + 127u) >> 7;
  return (((y >> 7) * blocksPerRow + (x >> 7)) << 16) + o;
}
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= uWidth || p.y >= uHeight) return;
  px[tiledOffset(uint(p.x), uint(p.y)) >> 2] = packUnorm4x8(texelFetch(uSrc, ivec2(p.x, uHeight - 1 - p.y), 0));
}
)";

// The shader's tiledOffset on the CPU, so capture() can read the tiled buffer back as a normal picture.
uint32_t tiledOffsetCpu(uint32_t x, uint32_t y) {
  const uint32_t o = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^
                     ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
  return (((y >> 7) * ((W + 127) / 128) + (x >> 7)) << 16) + o;
}

// The side-by-side colour texture and its framebuffer, the copy program, and the mapped buffer holding all the
// eye buffers. `stride` is one buffer's size; `alignOffset` skips to the first 64 KiB boundary.
GLuint tex, fbo, copy, buffer;
size_t stride, alignOffset;
uint8_t *mappedBase;
vr::Targets t{};

// Creates the texture, framebuffer and eye buffers for `size` pixels per eye. The buffer is mapped persistently
// and coherently because on the PS5 a mapped GL buffer is real direct memory at one address for the CPU, the GPU
// and the headset. False if the console cannot give that much memory (the caller then tries a smaller size).
bool make(int size) {
  EYE = size, W = size * 2, H = size;
  glCreateTextures(GL_TEXTURE_2D, 1, &tex);
  glTextureStorage2D(tex, 1, GL_RGBA8, W, H);
  glCreateFramebuffers(1, &fbo);
  glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);

  stride = (size_t)((W + 127) / 128) * ((H + 127) / 128) * 0x10000;
  const size_t total = stride * vr::kBuffers + 0x10000;
  const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
  glCreateBuffers(1, &buffer);
  glNamedBufferStorage(buffer, total, nullptr, flags);
  auto *mapped = (uint8_t *)glMapNamedBufferRange(buffer, 0, total, flags);
  if (!mapped || glGetError() != GL_NO_ERROR) {
    platform::log("vr eyes: %dx%d persistent map failed", W, H);
    glDeleteBuffers(1, &buffer);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    buffer = fbo = tex = 0;
    return false;
  }
  // The headset wants each buffer 64 KiB aligned.
  mappedBase = mapped;
  const uintptr_t base = ((uintptr_t)mapped + 0xffff) & ~(uintptr_t)0xffff;
  alignOffset = base - (uintptr_t)mapped;
  for (int b = 0; b < vr::kBuffers; b++) t.address[b] = base + b * stride;
  t.width = W, t.height = H, t.pitch = W, t.tiled = true;
  platform::log("vr eyes: %dx%d, buffers from 0x%llx", W, H, (unsigned long long)t.address[0]);
  return true;
}

}  // namespace

// Compiles the copy shader and creates the buffers at 2048 pixels an eye, falling back to 1536. Safe to call twice.
bool init() {
  if (fbo) return true;
  GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
  glShaderSource(cs, 1, &COPY_CS, nullptr);
  glCompileShader(cs);
  copy = glCreateProgram();
  glAttachShader(copy, cs);
  glLinkProgram(copy);
  glDeleteShader(cs);
  GLint ok = 0;
  glGetProgramiv(copy, GL_LINK_STATUS, &ok);
  if (!ok) {
    platform::log("vr eyes: copy shader failed");
    return false;
  }
  return make(2048) || make(1536);
}

// Pixels per eye, square (2048, or 1536 if the larger size did not fit).
int eye() { return EYE; }

// Bind this to draw: both eyes side by side, the left eye in the left half.
unsigned framebuffer() { return fbo; }

unsigned texture() { return tex; }

// What vr::start() needs: the three buffers' addresses and layout.
const vr::Targets &targets() { return t; }

// Copies the finished picture into eye buffer `b` and waits for the GPU, because the headset reads the buffer
// the moment it is submitted, so a half-written copy would show.
void fill(int b) {
  glUseProgram(copy);
  glBindTextureUnit(0, tex);
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffer, alignOffset + b * stride, stride);
  glUniform1i(glGetUniformLocation(copy, "uWidth"), W);
  glUniform1i(glGetUniformLocation(copy, "uHeight"), H);
  glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
  glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
  // The headset reads the buffer as soon as it is submitted.
  glFinish();
}

// Debug aid: writes buffer `b` as a half-size PPM by reading it back through the tiled layout, which shows
// exactly what the headset was given.
bool capture(int b, const char *path) {
  if (!mappedBase) return false;
  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  const uint8_t *px = mappedBase + alignOffset + b * stride;
  std::fprintf(f, "P6\n%d %d\n255\n", W / 2, H / 2);
  for (int y = 0; y < H; y += 2)
    for (int x = 0; x < W; x += 2) std::fwrite(px + tiledOffsetCpu((uint32_t)x, (uint32_t)y), 1, 3, f);
  std::fclose(f);
  return true;
}

}  // namespace vr_eyes
