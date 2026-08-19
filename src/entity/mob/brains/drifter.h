#pragma once

#include "entity/mob/brain.h"

namespace mob {

// Wanders where it happens to be, pauses, wanders again. Notices nothing.
//
// The simplest brain there is, and the one every other one is built out of: both
// of the others fall back to this when they have nothing else to do, which is what
// keeps "a creature going about its business" from being written three times.
//
// It is the whole behaviour of a bat and the resting half of a boar's.
//
// Two things in it are decisions rather than tuning, and both are about the same
// complaint — a creature that walks into a wall and keeps walking:
//
//   - **It turns at a wall.** Not by testing for one, which would need the brain
//     to know what a collider is, but by noticing that it is trying to move and
//     is not moving. That reads the same for a wall, a ledge it will not jump and
//     another creature standing in the way, which is right: all three mean "not
//     this way".
//   - **It turns at an edge.** A grounded drifter checks that there is ground
//     under the step it is about to take. Without it a meadow empties itself into
//     the nearest ravine over about a minute, and nothing on screen says why the
//     boars keep disappearing.
struct Drifter : Brain {
    body::Intent Think(const Sense &sense, Wits &wits) const override;

    const char *Mood(const Wits &wits) const override;
};

// The one instance. See `Brain` for why a brain is shared rather than owned.
// The one instance, shared by every creature of this temper.
//
// It registers itself under the name a creature's `temper` field names — see
// `drifter.cpp`. Nothing anywhere lists the behaviours that exist.
const Drifter &TheDrifter();

} // namespace mob
