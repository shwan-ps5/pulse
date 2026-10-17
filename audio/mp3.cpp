// mp3 -> 48 kHz stereo int16 (audio::kRate), via minimp3. resamples WHILE decoding (linear interpolation) so peak memory is
// the final track only; the console heap is finite. output.pcm reserved from file size / first frame bitrate (+8%).
// !: kMaxSamples (100M int16 ~ 17 min) refuses longer files (returns false) instead of risking heap exhaustion.
#include "mp3.h"

#include <cstdio>
#include <cstdlib>

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

namespace audio {

namespace {
// About 17 minutes at 48 kHz stereo; longer would not leave room in the console's heap.
constexpr size_t kMaxSamples = 100u * 1000 * 1000;
}  // namespace

// out.pcm = interleaved L,R int16 at audio::kRate. false if unreadable, not mp3, or over kMaxSamples. mono is duplicated to stereo.
bool decodeMp3(const char *path, Track &out) {
  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> file((size_t)size);
  const size_t got = std::fread(file.data(), 1, file.size(), f);
  std::fclose(f);
  if (got != file.size()) return false;

  // Resamples while decoding so only the finished 48 kHz track is held: the console's heap is 256 MiB.
  static mp3dec_t dec;
  mp3dec_init(&dec);
  int hz = 0;
  double ratio = 1;
  long n = 0;
  size_t outI = 0;
  float prev[2] = {0, 0};
  static int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t pos = 0;
  out.pcm.clear();
  while (pos < file.size()) {
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(&dec, file.data() + pos, (int)(file.size() - pos), frame, &info);
    if (info.frame_bytes <= 0) break;
    pos += (size_t)info.frame_bytes;
    if (samples <= 0) continue;
    if (!hz) {
      hz = info.hz;
      ratio = (double)hz / kRate;
      if (info.bitrate_kbps > 0) out.pcm.reserve((size_t)((double)size * 8 / (info.bitrate_kbps * 1000.0) * kRate * 2 * 1.08) + 8192);
    }
    if (out.pcm.size() > kMaxSamples) {
      out.pcm.clear();
      out.pcm.shrink_to_fit();
      return false;
    }
    for (int i = 0; i < samples; i++) {
      const float cur[2] = {(float)frame[i * info.channels], (float)(info.channels > 1 ? frame[i * info.channels + 1] : frame[i * info.channels])};
      if (n == 0) {
        out.pcm.push_back((int16_t)cur[0]), out.pcm.push_back((int16_t)cur[1]);
        outI = 1;
      } else {
        while ((double)outI * ratio <= (double)n) {
          const float u = (float)((double)outI * ratio - (double)(n - 1));
          for (int c = 0; c < 2; c++) out.pcm.push_back((int16_t)(prev[c] * (1 - u) + cur[c] * u));
          outI++;
        }
      }
      prev[0] = cur[0], prev[1] = cur[1];
      n++;
    }
  }
  return hz != 0 && !out.pcm.empty();
}

}  // namespace audio
