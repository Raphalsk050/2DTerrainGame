#pragma once

#include "entity/mob/life.h"

#include <cstdint>
#include <vector>

namespace mob {

// One square of country, and everything that has ever been decided about it.
//
// The whole persistence design is in the first field, so it is worth reading before
// the rest of the module.
//
// **`settled` is a memory of decisions, not of creatures.** One bit per row of the
// creature table, set the first time that kind's population was rolled here. It is
// never cleared. So:
//
//   - Walking into new country rolls its creatures once, from a function of the cell
//     and the seed — the same country, in the same places, in every session of that
//     world. Nothing to do with when you happened to arrive.
//   - Walking away and back does not roll again. The creatures that were here are in
//     `asleep`; the ones that were killed are simply not, and **they do not come
//     back**, because nothing will ever ask this cell for boars a second time.
//   - A kind whose conditions were not met on the first visit keeps its bit clear
//     and settles later — which is how a meadow settled at noon still has shades in
//     it when night falls on it for the first time.
//
// That is exactly the argument `Grove::TreeState::cleared` makes about a felled
// tree: **the record has to outlive the thing it is about.** A cell with no record
// is a cell full of animals, so forgetting a cell the player has hunted out is the
// same as putting the animals back.
//
// The cost is that the map grows with the ground explored, one entry per cell ever
// visited, exactly as the wood's does. It is reported for the same reason — see
// `Warren::Remembered`.
struct Patch {
    // One bit per creature kind. See above.
    std::uint32_t settled = 0;

    // Which cell this is. Held rather than unpacked from the map key, because a key
    // that packs two signed numbers into one is a thing that can be unpacked
    // wrongly, and the two bytes are cheaper than the class of bug.
    int cx = 0;
    int cy = 0;

    // Weather-clock time before which this cell will not be asked again about a kind
    // it could not place. See `kAskAgainIn` — without it a shade that cannot be put
    // in a meadow at noon costs twenty-four world probes per patch per frame to keep
    // finding that out.
    float askAgainAt = 0.0f;

    // The creatures resting here: those the view has left, waiting to be woken.
    //
    // Empty while the patch is awake — its creatures are in the herd then, and
    // holding a second copy would be two answers to where a boar is.
    std::vector<Life> asleep;

    // Whether its creatures are currently in the herd.
    bool awake = false;

    // How many were rolled here in total, and how many are gone. Kept only so that
    // `--mobcheck` and the head-up display can say what the memory is holding; no
    // rule reads either.
    int rolled = 0;
    int lost   = 0;
};

} // namespace mob
