#pragma once

#include "raylib.h"

// One hit, described as the thing that lands rather than as the thing that swung.
//
// It carries where it came from and not who threw it, and that is the whole point
// of the type: a boar knocked back by a player, by another boar, or by a rock
// falling on it is knocked back the same way, and none of the three has to be
// known about here. It is also what lets the same struct travel in both
// directions — the player hits a mob and a mob hits the player through one shape.
//
// `from` is a position rather than a direction because a direction has to be
// derived from something, and every caller has the position to hand: a swing has
// the body that made it, a contact has the creature that made contact. Deriving
// it here means one rule for which way a thing is thrown, instead of one per
// caller with a sign error in half of them.
namespace life {

struct Blow {
    int damage = 0;

    // Where it came from, in world pixels.
    Vector2 from{};

    // How hard the target is thrown away from `from`, and how far it is lifted,
    // in pixels per second.
    //
    // The lift is separate and always upward rather than falling out of the
    // direction, because a blow struck downward at something standing on the floor
    // has nowhere to throw it: without a lift the knock is entirely horizontal and
    // the target skates rather than being hit.
    float knock = 0.0f;
    float lift  = 0.0f;
};

} // namespace life
