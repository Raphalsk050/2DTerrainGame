#pragma once

#include "entity/mob/mob_def.h"
#include "raylib.h"

class World;

namespace mob {

// Whether one spot will hold one creature.
//
// The whole of what a `Haunt` means, in one function, and it is deliberately the
// *only* place that reads those fields. Two things ask it and they must never
// disagree: the warren, which is placing creatures, and `--mobs`, which is checking
// that a row describes somewhere real. A report written against a second copy of
// this test would be a report about a spawner that does not exist.
//
// The order of the tests inside it is the whole of the cost: cheap and disqualifying
// first — the depth band, then the room to stand — and the light level and the
// climate bell last, because those are the two that read fields. Asking the climate
// first would sample noise for every spot that was going to fail on depth anyway.
//
// It has no opinion about *when*, and no opinion about the player. Whether a place
// is settled at all, and how far from the view that happens, is the warren's.
bool Suits(const World &world, const Def &def, Vector2 at);

// How much room above the ground a creature is given when it is placed, as a
// multiple of its own height.
//
// A creature settled flush against a ceiling is a creature settled inside one: the
// body is unstuck on its first frame, which throws it somewhere it was never meant
// to be, and what a player sees is an animal walking out of a wall.
//
// Public because `--mobs` measures against it, and a probe that used a figure of its
// own would drift from the rule it is reporting on.
inline constexpr float kHeadroom = 1.25f;

} // namespace mob
