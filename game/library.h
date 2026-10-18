#pragma once

// The songs the player can pick: MP3 files in a few folders, each analysed once into a cached map.
#include <string>
#include <vector>

#include "mapper.h"
#include "mp3.h"

namespace library {

struct Entry {
  std::string path, title, id;  // id: stable key for the cache and the scores
};

// Every .mp3 in the folders and the folders inside them (four deep), sorted by title.
std::vector<Entry> scan(const std::vector<std::string> &dirs);

// The index.txt fallback on its own (the console may refuse to list a folder): songs named in <dir>/index.txt, appended to list.
// False when there is no index file. scan() uses it by itself; it is public so the tests can reach it on a desktop that can list folders.
bool scanIndex(const std::string &dir, std::vector<Entry> &list);

// Bumped when the mapper changes what it makes, so old cached maps are not used.
constexpr int kMapperVersion = 4;

// What the song select screen shows for a song whose map is already cached.
struct Info {
  double bpm = 0, duration = 0;
  int notes[4] = {0, 0, 0, 0};
};
bool cachedInfo(const Entry &e, const std::string &cacheDir, int seed, int hands, Info &out);

// Decodes the audio and loads the song's map for that pattern seed, analysing and caching it first if there is none.
// hands as in mapper::generate (0 both, 1 left only, 2 right only); each has its own cached map.
bool load(const Entry &e, const std::string &cacheDir, int seed, int hands, audio::Track &track, mapper::Song &song);

}  // namespace library
