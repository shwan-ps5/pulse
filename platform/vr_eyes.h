#pragma once

#include "vr.h"

// The headset's eye buffers, as the VR test proved them on the console: both eyes are drawn side by
// side into a GL texture, then a compute shader copies that, top row first, into a persistently mapped
// GL buffer whose address the headset reads (on the PS5 a mapped buffer is its own direct memory, at
// the same address for the CPU and the GPU). The GPU's 64 KiB-block tiled layout, as PS5VR hands the
// headset its buffers: with linear rows the left eye came out shifted, with garbage from before the
// buffer. Three buffers, as vr.h registers.
namespace vr_eyes {

// Creates the texture, its framebuffer and the buffers, at 2048 pixels an eye (PS VR2's screens are
// 2000 x 2040), or 1536 if the GPU's memory won't stretch that far; false if neither works.
bool init();
int eye();  // pixels per eye, square
unsigned framebuffer();  // both eyes side by side, eye() * 2 x eye()
unsigned texture();      // its colour texture
const vr::Targets &targets();
// Copies the side-by-side picture into eye buffer `b` and waits for the GPU, ready to submit.
void fill(int b);
// Writes eye buffer `b` as the headset reads it (top row first), at half size, as a PPM: for
// checking what was submitted.
bool capture(int b, const char *path);

}  // namespace vr_eyes
