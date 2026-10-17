#include "vr_eyes.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "gl.h"
#include "platform.h"

namespace vr_eyes {

namespace {

int EYE = 2048, W = EYE * 2, H = EYE;

// GL's bottom-up rows into the headset's top-down ones, RGBA8 with red in the low byte, in the GPU's
// 64 KiB blocks of 128 x 128 texels (the boilerplate's tiled_byte_offset, as the VR test checks it).
const char *COPY_CS = R"(#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D uSrc;
layout(std430, binding = 1) writeonly buffer Dst { uint px[]; };
uniform int uWidth, uHeight;
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

uint32_t tiledOffsetCpu(uint32_t x, uint32_t y) {
  const uint32_t o = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^
                     ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
  return (((y >> 7) * ((W + 127) / 128) + (x >> 7)) << 16) + o;
}

GLuint tex, fbo, copy, buffer;
size_t stride, alignOffset;
uint8_t *mappedBase;
vr::Targets t{};

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

int eye() { return EYE; }

unsigned framebuffer() { return fbo; }

unsigned texture() { return tex; }

const vr::Targets &targets() { return t; }

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
