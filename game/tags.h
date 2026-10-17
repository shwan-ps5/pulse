#pragma once

// ID3 tags of an MP3: artist, title and the embedded cover picture, with the text tidied for display.
#include <cstdint>
#include <string>
#include <vector>

namespace tags {

struct Info {
  std::string artist, title;  // ASCII, tidied; empty when the file has no such tag
};

// Reads the artist and title frames (not the picture). False when there is no ID3v2 tag.
bool read(const char *path, Info &out);

// "Artist - Title" for the song list, from the tags and the file name: no track number, no web addresses.
std::string display(const Info &t, const std::string &fileStem);

// The cover picture as kArtSize x kArtSize RGB, cropped to a square from the middle. False if there is none.
constexpr int kArtSize = 256;
bool cover(const char *path, std::vector<uint8_t> &rgb);

}  // namespace tags
