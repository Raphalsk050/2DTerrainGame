#pragma once

#include "ui/debug_view.h"
#include "hand/editor.h"
#include "flora/grove.h"
#include "entity/mob/herd.h"
#include "entity/player/player.h"
#include "raylib.h"
#include "world/world.h"

// What is drawn in the frame's own coordinates rather than in the world's.
//
// The readout, the brush badge and the bar over the block being broken. All of it
// is on the far side of the light multiply, deliberately: a thing meant to be
// *read* must not go dark because the character walked into a cave.
namespace hud {

// One line of the readout, light on a dark outline so it reads against a bright
// sky and a black cave alike.
void Label(const char *text, int x, int y, Color colour);

// Everything the screen says while the world is being played.
void Draw(const World &world, const Grove &grove, const mob::Herd &herd, const Player &player,
          const Editor &editor, const Camera2D &camera,
          const debug_view::Toggles &debug, float lantern, const char *notice, float noticeFor);

} // namespace hud
