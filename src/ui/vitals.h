#pragma once

#include "core/mode.h"
#include "entity/life/health.h"
#include "raylib.h"

// How much the character has left, drawn as hearts.
//
// Hearts and not a bar, and the reason is Minecraft's rather than taste: a bar says
// *how much of it* is left and a row of hearts says *how many more hits*, and the
// second is the question a player in trouble is actually asking. It is also countable
// out of the corner of an eye, which a bar at 40% is not.
//
// Ten of them, each worth two points, with halves — which is why `player_config::kHealth`
// is twenty. Reading a punch as half a heart is the whole of what makes the unit
// legible, and it only works if the numbers are chosen to make it work.
//
// **Survival only.** Minecraft hides the health, hunger, oxygen, experience and armour
// bars in Creative outright, and it is right to: a number that cannot change is a
// number that teaches the eye to stop looking, and the one time it *did* mean something
// it would be ignored. The mode is asked for here rather than tested by the caller so
// that there is one place the rule lives.
namespace vitals {

// How many hearts a full row is, and what one is worth.
inline constexpr int kHearts = 10;

// The heart itself: seven art cells, three screen pixels each, three apart.
//
// Twenty-one pixels against a forty-four pixel slot, which is the proportion Minecraft
// draws — nine pixels against twenty. It was fourteen first and read as a decoration
// rather than as a reading: a row you are meant to *count* has to be countable at a
// glance from the far side of the screen, and at two pixels a cell the notch between
// the lobes closed up.
//
// Whole pixels, because the art is drawn cell by cell and half of one is a heart with
// its columns alternating two wide and three.
inline constexpr float kCell  = 3.0f;
inline constexpr int kSide    = 7;
inline constexpr float kApart = 3.0f;

// How wide the whole row comes out.
inline constexpr float kRowWide = kHearts * (kSide * kCell + kApart) - kApart;

// Draws the row with its left edge at `where.x` and its middle on `where`'s.
//
// **Always, in Survival.** Nothing in Creative.
//
// It hid itself at full health first, on the reasoning that a row which has never moved
// is furniture. That was wrong, and wrong in the way that is worst: nothing in this game
// damages the player yet — the boar's `hits` is zero and the hostile that had a number
// there was taken out — so the health never fell, so the row never appeared *once*. A
// display that is invisible until a condition nothing can currently produce is not
// restraint, it is a feature that looks broken.
//
// Minecraft shows them always and is right to, and the reason is not that its hearts
// move more often. A readout has to be somewhere the eye already knows before the moment
// it matters; one that appears for the first time *during* the emergency is one the
// player has to find while being hit.
void Draw(const life::Health &health, Gamemode mode, Rectangle where);

} // namespace vitals
