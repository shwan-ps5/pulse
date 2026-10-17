#pragma once

// Turns an analysed song into note maps for Easy / Medium / Hard, and reads and writes the .pulse text
// format the game loads.
#include <string>
#include <vector>

#include "analysis.h"

namespace mapper {

enum class Difficulty { Easy, Medium, Hard, Extreme };
const char *name(Difficulty d);

// Cut direction: 0 up, 1 down, 2 left, 3 right, 4 up-left, 5 up-right, 6 down-left, 7 down-right, 8 any (dot),
// then lines that cut either way along them: 9 vertical, 10 horizontal, 11 diagonal /, 12 diagonal \.
struct Note {
  double time;     // seconds from the start of the audio
  int lane;        // 0..3, left to right
  int row;         // 0..2, bottom to top
  int color;       // 0 left hand (red), 1 right hand (blue)
  int dir;
};

struct Map {
  Difficulty difficulty;
  std::vector<Note> notes;
};

struct Song {
  double bpm = 0, duration = 0;
  int downbeatPhase = 0;  // beats[i] is a downbeat when i % 4 == downbeatPhase
  std::vector<double> beats;
  std::vector<analysis::Bar> bars;
  // Per beat, for the visuals: bass / mid / high level 0..1 and the section the beat sits in.
  struct Cue {
    double time;
    float bass, mid, high;
    int section;
  };
  std::vector<Cue> cues;
  std::vector<Map> maps;
};

Song generate(const analysis::Result &r, unsigned seed);
bool write(const Song &s, const char *path);
// Reads a .pulse file (bpm, duration, bars, cues and the note lists). False if it is missing or not one.
bool read(const char *path, Song &s);
// Checks a map is playable: ordered, in range, a hand never needs to swing faster than it can.
// Returns the problems found, empty when fine.
std::vector<std::string> validate(const Map &m);

}  // namespace mapper
