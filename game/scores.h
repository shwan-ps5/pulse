#pragma once

// Best results per song and difficulty, kept in a small text file.
#include <string>

namespace scores {

struct Best {
  long score = 0;
  float accuracy = 0;  // percent of the maximum score
  float rating = 0;    // 0-100, what the rank is read from
  int combo = 0, plays = 0;
};

void init(const std::string &path);
Best get(const std::string &songId, int difficulty);
// Records a finished run; true when it is a new best score.
bool record(const std::string &songId, int difficulty, long score, float accuracy, float rating, int combo);
// How well a run went, 0-100: half the share of blocks hit, half the score as a share of a solid run (60% of the
// maximum, which needs long combos and wide swings), so a few misses do not sink the rank.
float rating(float accuracy, int hits, int notes);
// S (90 and up), A (80), B (70), C (50), D (40), E (30), F below that.
const char *rank(float rating);

}  // namespace scores
