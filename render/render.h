#pragma once

// Draws the play field: lanes, beat lines, notes, cut shards, sabers and a small HUD, in scene space
// (metres, y up, the player at the origin facing -z). It draws into whatever framebuffer and viewport
// are bound and does not clear, so a flat window and both eyes of the headset share it.
#include <string>
#include <vector>

#include "game.h"
#include "mapper.h"
#include "vmath.h"

namespace render {

struct Fx {
  game::Vec3 pos;
  int color;  // 0 red, 1 blue, 2 grey (a bad cut)
  double t;
};

// Lines of text floating in front of the player: titles, menus, scores. Style picks the colour.
struct Row {
  std::string text;
  float size = 0.016f;  // font size: the em is 8 times this, in metres
  int style = 1;        // 0 title gold, 1 normal, 2 selected, 3 dim, 4 good, 5 bad
  bool art = false;     // a blank row that the cover picture is drawn over
};
struct Panel {
  std::vector<Row> rows;
  bool preview = false;  // two sample blocks beside the panel, in the current colours
  void add(const std::string &t, float size = 0.016f, int style = 1) { rows.push_back({t, size, style, false}); }
  void addArt(float side) { rows.push_back({" ", side / 12, 1, true}); }
};

// The size in metres a panel is drawn at, and the distance in front of it (z = -1.6 for the panel itself).
void panelSize(const Panel &p, float &width, float &height);

struct Scene {
  double now = 0;
  const mapper::Song *song = nullptr;
  int diff = 1;
  const game::Session *session = nullptr;
  const game::SaberPose *sabers = nullptr;  // [2]
  const std::vector<Fx> *effects = nullptr;
  const Panel *panel = nullptr;
  double clock = 0;  // wall-clock seconds, to animate the background when no song is playing
  int bg = 0;        // the background: 0 none, 1 light, 2 full
  bool bg3d = false; // its rings as geometry in the scene rather than painted on the sky
  bool hdr = false;  // drawing into the bloom target: lets the sabers burn brighter than white
  float speed = 14;
  int downbeatPhase = 0;
  // Nothing is drawn nearer than this z: the console's GL doesn't clip at the eye plane, so anything
  // crossing it would ghost. A flat camera behind the player can set it far forward.
  float maxZ = -0.3f;
};

bool init();
// Bloom: draw the frame between postBegin and postEnd (which then writes the glowing result to the window).
bool postBegin(int w, int h);
void postEnd();
// The colours of the blocks and sabers: a preset by index, and whether the hands swap them.
void setPalette(int id, bool swap);
// The cover picture shown in a panel's art row: 256 x 256 RGB, or null for none.
void setArt(const uint8_t *rgb);
int paletteCount();
const char *paletteName(int id);
// The background colour for the song's current section and intensity.
void background(const Scene &s, float rgb[3]);
void draw(const Mat4 &viewProj, const Scene &s, const float bg[3]);

}  // namespace render
