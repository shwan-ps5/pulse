// ID3v2 reading for the song list: artist/title text and the embedded cover picture; plus tidying names for display.
// scope: v2.2/2.3/2.4 frames TT2/TIT2, TP1/TPE1, PIC/APIC; text encodings ISO-8859-1, UTF-16 (+BOM), UTF-16BE, UTF-8.
// output text is ASCII-ish: Latin-1 letters folded, Cyrillic A-ya kept as UTF-8 (the font atlas has them), other non-ASCII
//   becomes '?'. display(): "Artist - Title", from tags else file name; strips track numbers, URLs, "(1)" copy marks, splits
//   squashed "Artist-Title(Mix)" names. cover(): JPEG/PNG only (stb_image, no stdio) cropped square to kArtSize^2 RGB.
// !: tag frames can be huge; walk() caps at 16 MiB and cover at 12 MiB. never trust sizes: all reads are bounds-checked.
#include "tags.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace tags {

namespace {

// Latin-1 letters from 0xC0 up, as plain ASCII.
const char kFold[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

// append codepoint cp to out: printable ASCII as is, ` and the acute accent as ', Latin-1 letters folded to ASCII via kFold,
// dashes to '-', Cyrillic А-я/Ё/ё as UTF-8, everything else >=0x100 as '?', control chars as space.
void put(std::string &out, unsigned cp) {
  if (cp == 0x60 || cp == 0xB4 || cp == 0x2019 || cp == 0x2018) out += '\'';
  else if (cp >= 0x20 && cp < 0x7F) out += (char)cp;
  else if (cp >= 0xC0 && cp <= 0xFF) out += kFold[cp - 0xC0];
  else if (cp == 0x2013 || cp == 0x2014) out += '-';
  else if (cp == 0x401 || cp == 0x451 || (cp >= 0x410 && cp <= 0x44F)) {
    out += (char)(0xC0 | cp >> 6);
    out += (char)(0x80 | (cp & 63));
  } else if (cp >= 0x100) out += '?';
  else out += ' ';
}

// ID3 text frame body (first byte = encoding 0 Latin-1, 1 UTF-16+BOM, 2 UTF-16BE, 3 UTF-8) -> display text via put().
std::string decode(const uint8_t *d, size_t n) {
  std::string out;
  if (n < 1) return out;
  const int enc = d[0];
  d++, n--;
  if (enc == 0) {
    for (size_t i = 0; i < n && d[i]; i++) put(out, d[i]);
  } else if (enc == 1 || enc == 2) {
    bool be = enc == 2;
    if (enc == 1 && n >= 2) {
      if (d[0] == 0xFE && d[1] == 0xFF) be = true, d += 2, n -= 2;
      else if (d[0] == 0xFF && d[1] == 0xFE) d += 2, n -= 2;
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
      unsigned u = be ? (d[i] << 8 | d[i + 1]) : (d[i + 1] << 8 | d[i]);
      if (u == 0) break;
      put(out, u);
    }
  } else {
    for (size_t i = 0; i < n && d[i];) {
      unsigned c = d[i], cp = c, extra = 0;
      if (c >= 0xF0) cp = c & 7, extra = 3;
      else if (c >= 0xE0) cp = c & 15, extra = 2;
      else if (c >= 0xC0) cp = c & 31, extra = 1;
      i++;
      while (extra-- && i < n && (d[i] & 0xC0) == 0x80) cp = cp << 6 | (d[i++] & 63);
      put(out, cp);
    }
  }
  return out;
}

// token looks like a web address (http, www., ://, or a known TLD suffix / 'tld/' inside). used to drop site spam from names.
bool isUrl(std::string t) {
  while (!t.empty() && std::strchr("([{<\"'", t.front())) t.erase(0, 1);
  while (!t.empty() && std::strchr(")]}>\"',;:", t.back())) t.pop_back();
  for (char &c : t) c = (char)std::tolower((unsigned char)c);
  if (t.find("http") == 0 || t.find("www.") != std::string::npos || t.find("://") != std::string::npos) return true;
  static const char *tld[] = {".com", ".net", ".org", ".info", ".biz", ".ru", ".fm", ".co.uk", ".me", ".io", ".to", ".cc", ".tv", ".ws", ".us", ".eu"};
  for (const char *e : tld) {
    const size_t l = std::strlen(e);
    if (t.size() > l + 1 && t.compare(t.size() - l, l, e) == 0) return true;
    const size_t at = t.find(std::string(e) + "/");
    if (at != std::string::npos && at > 0) return true;
  }
  return false;
}

// Drops web addresses (and the brackets left empty), underscores and doubled spaces.
// drops URL tokens, '_' -> space, empty ()[]{} pairs, leading/trailing space, '-' and quote, doubled spaces.
std::string tidy(const std::string &in) {
  std::string s = in;
  for (char &c : s)
    if (c == '_') c = ' ';
  std::string out, tok;
  auto flush = [&] {
    if (!tok.empty() && !isUrl(tok)) out += (out.empty() ? "" : " ") + tok;
    tok.clear();
  };
  for (char c : s) {
    if (c == ' ') flush();
    else tok += c;
  }
  flush();
  for (const char *pair : {"()", "[]", "{}"}) {
    for (size_t p; (p = out.find(pair)) != std::string::npos;) out.erase(p, 2);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '-')) out.pop_back();
  while (!out.empty() && (out.front() == ' ' || out.front() == '-' || out.front() == '\'')) out.erase(0, 1);
  std::string sq;
  for (char c : out)
    if (!(c == ' ' && !sq.empty() && sq.back() == ' ')) sq += c;
  return sq;
}

// A track number at the front: "01 ", "03. ", "1 - ", "07)".
// strips a leading track number ('01 ', '03. ', '1 - ', '07)'); leaves plain numbers that are part of the title ('22 x A').
std::string noTrackNumber(const std::string &s) {
  size_t i = 0;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
  if (i == 0 || i > 3 || i >= s.size()) return s;
  size_t j = i;
  bool sep = false;
  if (s[j] == '.' || s[j] == ')') sep = true, j++;
  while (j < s.size() && s[j] == ' ') j++;
  if (j + 1 < s.size() && s[j] == '-' && s[j + 1] == ' ') sep = true, j += 2;
  while (j < s.size() && s[j] == ' ') j++;
  if (j == i || j >= s.size()) return s;
  if (!sep && s[0] != '0') return s;
  return s.substr(j);
}

// "BeckyHill-LastTime(BiscitsMix)" with no spaces at all becomes "Becky Hill - Last Time (Biscits Mix)".
// no-space names only: 'BeckyHill-LastTime(Mix)' -> 'Becky Hill - Last Time (Mix)' (split on first '-', camelCase and '(').
std::string unsquash(const std::string &s) {
  if (s.find(' ') != std::string::npos || s.size() < 6) return s;
  std::string out;
  bool dashed = false;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i], p = i ? s[i - 1] : ' ';
    if (c == '-' && !dashed && i > 0 && i + 1 < s.size() && s[i + 1] != '-') {
      dashed = true;
      out += " - ";
    } else if (c == '(' && i > 0 && p != '-') {
      out += " (";
    } else if (std::isupper((unsigned char)c) && std::islower((unsigned char)p)) {
      out += ' ';
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}

// A trailing copy marker: "Title (1)".
// removes a trailing ' (N)' duplicate marker.
std::string noCopyMark(std::string s) {
  const size_t p = s.rfind(" (");
  if (p != std::string::npos && p + 4 >= s.size() && s.back() == ')' && std::isdigit((unsigned char)s[p + 2]) && s.size() - p <= 5) s.erase(p);
  return s;
}

// big-endian u32.
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
// ID3 sync-safe integer (7 bits per byte).
uint32_t safe32(const uint8_t *p) { return (uint32_t)(p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

// Walks the frames of the ID3v2 tag, handing each wanted one to `take(id, bytes)`.
template <class F>
// visits each frame of the file's ID3v2 tag: take(id, size, FILE*, body) returns true if it consumed the frame body
// (else walk seeks past it). false if the file has no ID3v2 tag. handles v2.2 6-byte and v2.3/2.4 10-byte frame headers
// and an extended header.
bool walk(const char *path, F take) {
  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t h[10];
  if (std::fread(h, 1, 10, f) != 10 || std::memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) return std::fclose(f), false;
  const int ver = h[3];
  long pos = 10;
  const long end = 10 + (long)safe32(h + 6);
  if (h[5] & 0x40 && ver >= 3) {
    uint8_t e[4];
    if (std::fread(e, 1, 4, f) == 4) pos += 4 + (long)(ver == 4 ? safe32(e) - 4 : be32(e));
    std::fseek(f, pos, SEEK_SET);
  }
  const int hdr = ver == 2 ? 6 : 10;
  while (pos + hdr <= end) {
    uint8_t fh[10];
    if (std::fread(fh, 1, hdr, f) != (size_t)hdr || fh[0] == 0) break;
    char id[5] = {};
    std::memcpy(id, fh, ver == 2 ? 3 : 4);
    const uint32_t size = ver == 2 ? (uint32_t)fh[3] << 16 | fh[4] << 8 | fh[5] : ver == 4 ? safe32(fh + 4) : be32(fh + 4);
    pos += hdr + size;
    if (size > (1u << 24) || pos > end) break;
    std::vector<uint8_t> body;
    if (take(id, size, f, body)) continue;
    std::fseek(f, pos, SEEK_SET);
  }
  std::fclose(f);
  return true;
}

}  // namespace

// artist (TPE1/TP1) and title (TIT2/TT2), tidied; strings > 2 KiB ignored. false when no ID3v2 tag.
bool read(const char *path, Info &out) {
  return walk(path, [&](const char *id, uint32_t size, FILE *f, std::vector<uint8_t> &body) {
    const bool isTitle = !std::strcmp(id, "TIT2") || !std::strcmp(id, "TT2"), isArtist = !std::strcmp(id, "TPE1") || !std::strcmp(id, "TP1");
    if ((!isTitle && !isArtist) || size > 2048) return false;
    body.resize(size);
    if (std::fread(body.data(), 1, size, f) != size) return false;
    (isTitle ? out.title : out.artist) = tidy(decode(body.data(), body.size()));
    return true;
  });
}

// song-list name. rule order: tags artist+title (title stripped of a repeated 'Artist - ' prefix) -> file name if it holds
// ' - ' -> artist + file -> title or file. pinned by manual checks against ~300 real files.
std::string display(const Info &t, const std::string &stem) {
  std::vector<uint8_t> utf8;
  utf8.push_back(3);
  for (char c : stem) utf8.push_back((uint8_t)c);
  std::string file = unsquash(noCopyMark(tidy(noTrackNumber(tidy(decode(utf8.data(), utf8.size()))))));
  std::string title = tidy(noTrackNumber(t.title));
  if (!t.artist.empty() && title.size() > t.artist.size() + 3 && title.compare(0, t.artist.size() + 3, t.artist + " - ") == 0) title.erase(0, t.artist.size() + 3);
  if (!t.artist.empty() && !title.empty()) return t.artist + " - " + title;
  if (file.find(" - ") != std::string::npos) return file;
  if (!t.artist.empty()) return t.artist + " - " + file;
  return title.empty() ? file : title;
}

// embedded picture (APIC/PIC) decoded and box-resampled to kArtSize x kArtSize RGB from the central square. false if none/unsupported.
bool cover(const char *path, std::vector<uint8_t> &rgb) {
  std::vector<uint8_t> pic;
  walk(path, [&](const char *id, uint32_t size, FILE *f, std::vector<uint8_t> &body) {
    if ((std::strcmp(id, "APIC") && std::strcmp(id, "PIC")) || !pic.empty() || size > (12u << 20)) return false;
    body.resize(size);
    if (std::fread(body.data(), 1, size, f) != size) return true;
    size_t i = 1;
    if (!std::strcmp(id, "PIC")) i += 3;
    else
      while (i < size && body[i]) i++;
    if (!std::strcmp(id, "APIC")) i++;
    i++;  // the picture type
    if (i >= size) return true;
    const bool wide = body[0] == 1 || body[0] == 2;
    if (wide) {
      while (i + 1 < size && (body[i] || body[i + 1])) i += 2;
      i += 2;
    } else {
      while (i < size && body[i]) i++;
      i++;
    }
    if (i < size) pic.assign(body.begin() + i, body.end());
    return true;
  });
  if (pic.empty()) return false;
  int w, h, n;
  uint8_t *px = stbi_load_from_memory(pic.data(), (int)pic.size(), &w, &h, &n, 3);
  if (!px) return false;
  const int s = std::min(w, h), x0 = (w - s) / 2, y0 = (h - s) / 2;
  rgb.assign((size_t)kArtSize * kArtSize * 3, 0);
  for (int y = 0; y < kArtSize; y++)
    for (int x = 0; x < kArtSize; x++) {
      const int a = x * s / kArtSize, b = std::max(a + 1, (x + 1) * s / kArtSize), c = y * s / kArtSize, d = std::max(c + 1, (y + 1) * s / kArtSize);
      int sum[3] = {0, 0, 0}, cnt = 0;
      for (int yy = c; yy < d; yy++)
        for (int xx = a; xx < b; xx++, cnt++)
          for (int k = 0; k < 3; k++) sum[k] += px[((size_t)(y0 + yy) * w + x0 + xx) * 3 + k];
      for (int k = 0; k < 3; k++) rgb[((size_t)y * kArtSize + x) * 3 + k] = (uint8_t)(sum[k] / cnt);
    }
  stbi_image_free(px);
  return true;
}

}  // namespace tags
