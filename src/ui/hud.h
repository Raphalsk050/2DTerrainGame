#pragma once

#include "core/mode.h"
#include "entity/life/health.h"
#include "item/inventory.h"
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
// The whole strip along the foot of the screen: the bar, what is held, the hearts and
// what the hand is.
//
// One call, because they are one block on screen and were being drawn from two places
// — which is how the health came to be drawn *underneath* the inventory panel, the
// panel replacing only the bar. Anything laid out together has to be drawn together or
// the layout is a coincidence.
void Strip(const Inventory &inventory, const life::Health &health, const Editor &editor, Gamemode mode);

// `mode` is here for one reason and it is the same one Minecraft has: the vitals are
// hidden outright in Creative. A number that cannot change teaches the eye to stop
// looking, and then the one time it does mean something it is missed.
void Draw(const World &world, const Grove &grove, const mob::Herd &herd, const Player &player,
          const Editor &editor, Gamemode mode, const Camera2D &camera,
          const debug_view::Toggles &debug, float lantern, const char *notice, float noticeFor);

} // namespace hud
