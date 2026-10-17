#pragma once

// Music analysis for the map generator: onsets per band, tempo and beats, bars, and the song's
// structure (intro / build / drop / break / steady / outro). Plain C++17 with no dependencies, so the
// same code can run in the PC tool and, later, on the console.
#include <cstdint>
#include <string>
#include <vector>

namespace analysis {

constexpr int kSampleRate = 22050;
constexpr int kFft = 1024;
constexpr int kHop = 256;
constexpr double kFps = (double)kSampleRate / kHop;  // analysis frames per second

enum class Section { Intro, Steady, Build, Drop, Break, Outro };
const char *name(Section s);

struct Bar {
  double start, end;  // seconds
  float low, mid, high, energy, density;  // normalised 0..1 over the track
  float intensity;                         // smoothed 0..1
  Section section;
};

struct Result {
  double duration = 0;
  double bpm = 0;
  double beatConfidence = 0;               // 0..1, how well the beat grid explains the onsets
  std::vector<float> onset;                // per frame, combined
  std::vector<float> band[3];              // per frame onset strength: low, mid, high
  std::vector<float> energy[3];            // per frame band energy (linear), for the visuals
  std::vector<double> beats;               // seconds
  int downbeatPhase = 0;                   // beats[i] is a downbeat when i % 4 == phase
  std::vector<Bar> bars;
  std::vector<double> peaks;               // picked onsets (seconds) and their strengths
  std::vector<float> peakStrength;
};

// Mono float samples at kSampleRate.
Result analyse(const std::vector<float> &mono, double sampleRate = kSampleRate);

}  // namespace analysis
