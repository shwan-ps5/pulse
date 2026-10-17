#pragma once

// MP3 to PCM for the game's music, on the console and the desktop alike (minimp3, public domain).
#include <cstdint>
#include <vector>

namespace audio {

constexpr int kRate = 48000;

struct Track {
  std::vector<int16_t> pcm;  // interleaved stereo at kRate
  size_t frames() const { return pcm.size() / 2; }
};

// Reads and decodes the whole file; resamples to kRate. False if it can't be read or has no audio.
bool decodeMp3(const char *path, Track &out);

}  // namespace audio
