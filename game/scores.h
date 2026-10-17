#pragma once

// Best results per song and difficulty, kept in a small text file.
#include <string>

namespace scores {

struct Best {
  long score = 0;
  float accuracy = 0;  // percent of the maximum score
  int combo = 0, plays = 0;
};

void init(const std::string &path);
Best get(const std::string &songId, int difficulty);
// Records a finished run; true when it is a new best score.
bool record(const std::string &songId, int difficulty, long score, float accuracy, int combo);
// S, A, B, C or D for an accuracy percentage.
const char *rank(float accuracy);

}  // namespace scores
