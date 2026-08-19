#pragma once

#include "entity/mob/mob_def.h"
#include "raylib.h"

#include <cstdint>
#include <optional>

class World;

namespace mob {

class Herd;

// Where creatures come from.
//
// Its own file rather than a method on the herd, because the two answer different
// questions and are wrong in different ways. The herd is about creatures that
// exist; this is about the country — what the ground is like at a spot, how dark it
// is, what climate it sits in, and whether any row wants such a place.
//
// Nothing about it is remembered. A spawn is an attempt at one position, judged
// against the rows, and forgotten. That is what keeps it from becoming a second
// generator with its own state to keep in step with the first.
namespace spawn {

// Where and what one attempt would put down, if anything.
struct Wish {
    Kind kind{};
    Vector2 at{};

    // How many arrive together, already rolled against the row's own range.
    int many = 1;
};

// Tries one spot inside `active` and reports what belongs there, if anything.
//
// The order of the tests is the whole of the cost. Cheap and disqualifying first —
// the distance from the player, then the surface, then the solidity — and the
// climate bell and the light level last, because those are the two that read
// fields. A spawner that asked the climate first would sample noise for every spot
// it was going to reject on a distance test anyway.
//
// `crowdedOut` is asked of the herd for each candidate row, so that a kind at its
// crowd limit is never even considered.
std::optional<Wish> Try(const World &world, Rectangle active, Vector2 quarry, std::uint32_t &seed,
                        const Herd &herd);

// Whether one particular spot suits one particular row.
//
// Public because `--mobs` walks the world with it, which is the only way to find
// out whether a haunt describes anywhere at all. The trap is exactly the one
// CLAUDE.md §8 describes for a wood: a rule is very easy to author into a creature
// that is never anywhere, and nothing errors.
bool Suits(const World &world, const Def &def, Vector2 at);

} // namespace spawn

} // namespace mob
