#include "scores.h"

#include <cstdio>
#include <algorithm>
#include <map>

namespace scores {

namespace {
std::string file;
std::map<std::string, Best> table;
std::string key(const std::string &id, int d) { return id + ":" + std::to_string(d); }

void save() {
  if (FILE *f = std::fopen(file.c_str(), "w")) {
    for (const auto &kv : table) std::fprintf(f, "%s %ld %.2f %d %d\n", kv.first.c_str(), kv.second.score, (double)kv.second.accuracy, kv.second.combo, kv.second.plays);
    std::fclose(f);
  }
}
}  // namespace

void init(const std::string &path) {
  file = path;
  table.clear();
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) return;
  char k[64];
  Best b;
  double acc;
  while (std::fscanf(f, "%63s %ld %lf %d %d", k, &b.score, &acc, &b.combo, &b.plays) == 5) b.accuracy = (float)acc, table[k] = b;
  std::fclose(f);
}

Best get(const std::string &id, int d) {
  auto it = table.find(key(id, d));
  return it == table.end() ? Best() : it->second;
}

bool record(const std::string &id, int d, long score, float accuracy, int combo) {
  Best &b = table[key(id, d)];
  const bool best = score > b.score;
  if (best) b.score = score;
  b.accuracy = std::max(b.accuracy, accuracy);
  b.combo = std::max(b.combo, combo);
  b.plays++;
  save();
  return best;
}

const char *rank(float a) { return a >= 90 ? "S" : a >= 80 ? "A" : a >= 65 ? "B" : a >= 50 ? "C" : "D"; }

}  // namespace scores
