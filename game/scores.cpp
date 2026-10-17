// best score/accuracy/rating/combo/plays per (song id, difficulty), kept in a text file (STORAGE scores.txt).
// line format: "<id>[~seed]:<diff> score accuracy combo plays [rating]"; rating optional (older files) = accuracy.
// rating (0-100) = 50*hit share + 50*min(1, accuracy/60); rank letter S>=90 A80 B70 C50 D40 E30 F: pinned in game_test #10.
#include "scores.h"

#include <cstdio>
#include <algorithm>
#include <map>

namespace scores {

namespace {
std::string file;
std::map<std::string, Best> table;
std::string key(const std::string &id, int d) { return id + ":" + std::to_string(d); }

// rewrites the whole file (small); called on every record(). write failure is silent: scores just do not persist.
void save() {
  if (FILE *f = std::fopen(file.c_str(), "w")) {
    for (const auto &kv : table)
      std::fprintf(f, "%s %ld %.2f %d %d %.2f\n", kv.first.c_str(), kv.second.score, (double)kv.second.accuracy, kv.second.combo, kv.second.plays, (double)kv.second.rating);
    std::fclose(f);
  }
}
}  // namespace

// loads the table from path (missing file = empty); path also where record() saves.
void init(const std::string &path) {
  file = path;
  table.clear();
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) return;
  char line[256], k[64];
  while (std::fgets(line, sizeof line, f)) {
    Best b;
    double acc, rate;
    const int n = std::sscanf(line, "%63s %ld %lf %d %d %lf", k, &b.score, &acc, &b.combo, &b.plays, &rate);
    if (n < 5) continue;
    b.accuracy = (float)acc;
    b.rating = n == 6 ? (float)rate : b.accuracy;
    table[k] = b;
  }
  std::fclose(f);
}

// best so far for (id,diff), zeros if never played.
Best get(const std::string &id, int d) {
  auto it = table.find(key(id, d));
  return it == table.end() ? Best() : it->second;
}

// folds a run into the table (each field keeps its max, plays++) and saves. true when `score` beat the previous best.
bool record(const std::string &id, int d, long score, float accuracy, float rating, int combo) {
  Best &b = table[key(id, d)];
  const bool best = score > b.score;
  if (best) b.score = score;
  b.accuracy = std::max(b.accuracy, accuracy);
  b.rating = std::max(b.rating, rating);
  b.combo = std::max(b.combo, combo);
  b.plays++;
  save();
  return best;
}

float rating(float accuracy, int hits, int notes) {
  const float hitShare = notes > 0 ? (float)hits / notes : 0.0f;
  return 50.0f * hitShare + 50.0f * std::min(1.0f, accuracy / 60.0f);
}

const char *rank(float r) { return r >= 90 ? "S" : r >= 80 ? "A" : r >= 70 ? "B" : r >= 50 ? "C" : r >= 40 ? "D" : r >= 30 ? "E" : "F"; }

}  // namespace scores
