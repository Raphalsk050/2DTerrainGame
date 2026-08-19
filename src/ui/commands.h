#pragma once

#include "ui/console.h"
#include "flora/grove.h"
#include "item/inventory.h"
#include "entity/mob/herd.h"
#include "entity/player/player.h"
#include "raylib.h"
#include "world/world.h"

#include <string>

// What a typed line means.
//
// The console knows how to take a line and show an answer and nothing else — see
// the head of console.h for why the two are apart — and this is the other half:
// where a name turns into a change to the world.
//
// A module of its own rather than four hundred lines in the middle of the loop.
// What it needs is the world, the wood, the pack, the character and the camera,
// which is nearly everything the game is made of; having it inside main.cpp made
// that look like the loop's own business, and every command added made the loop
// longer.
namespace commands {

// Runs one line. Everything it has to say, it says through `chat`: a command that
// quietly does nothing is indistinguishable from one that is misspelt, and telling
// those apart is the whole reason the log exists.
void Run(const std::string &line, World &world, Grove &grove, Inventory &inventory, Player &player, mob::Herd &herd,
         Camera2D &camera, console::Console &chat);

} // namespace commands
