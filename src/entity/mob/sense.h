#pragma once

#include "raylib.h"

class World;

namespace mob {

struct Def;

// Everything a brain is allowed to know, gathered once before it is asked.
//
// A struct rather than a pile of parameters, and a *narrow* one rather than
// "here, have the game". What a brain can see is the whole of what a brain can
// react to, so this type is the design: widen it and every behaviour in the
// project quietly gains a new way to be surprising.
//
// It is deliberately missing three things it could have had:
//
//   - **The herd.** A creature that could read every other creature's position
//     would make each brain O(n) and would let a boar react to a boar off screen.
//     Flocking, when it is wanted, is a field the herd fills in here — one number
//     per creature, worked out once — and not a list handed to everyone.
//   - **Anything mutable.** A brain returns a wish and changes nothing. That is
//     what allows a brain to be a shared, stateless object rather than one per
//     creature, and it is what makes the memory below explicit instead of hidden
//     in whichever brain happens to be running.
//   - **The frame's input.** No creature answers to a key.
struct Sense {
    // The creature asking, and its row.
    const Def *def = nullptr;

    Vector2 at{};
    Vector2 velocity{};

    bool grounded = false;
    bool swimming = false;

    // Whether something walked into it since the last time it was asked, and
    // where that came from. This is the whole of a creature's memory of pain, and
    // it is here rather than in the brain because the herd is what knows a blow
    // landed.
    bool stung = false;
    Vector2 stungFrom{};

    // The one thing in the world a brain is told about by name.
    //
    // Single player, so there is exactly one, and pretending otherwise would be
    // machinery for a case that does not exist. When there are two, this becomes
    // "the nearest" and no brain changes.
    Vector2 quarry{};
    float toQuarry = 0.0f; // Distance, precomputed because every brain wants it.

    // Whether the creature can actually get at its quarry, or is merely near it.
    // Distance alone would have a shade lunging at a player on the far side of a
    // hillside.
    bool seesQuarry = false;

    // The world, for asking what is underfoot and what is in the way. Const: a
    // brain may look and may not touch.
    const World *world = nullptr;

    // The weather clock, for anything that behaves differently by the hour, and
    // the frame clock, for anything that has to happen at a rate.
    //
    // Two clocks and not one, for the reason CLAUDE.md §13.4 gives: how fast a
    // creature walks is a rule of the game and must not speed up forty times under
    // F7, while what it does at dusk plainly must.
    float now = 0.0f;
    float dt  = 0.0f;
};

} // namespace mob
