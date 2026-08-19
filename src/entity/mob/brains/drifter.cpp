#include "entity/mob/brains/drifter.h"

#include "entity/mob/brains/whim.h"
#include "entity/mob/mob_def.h"
#include "world/world.h"

#include <cmath>

namespace {

// What a drifter is doing. Stored in Wits::mood, which is deliberately an opaque
// number to everyone else — see `Wits`.
enum Mood : std::uint8_t {
    kResting = 0,
    kWalking = 1,
};

// How long a rest and a walk last. A range rather than a figure, so that a row of
// creatures does not step off together and does not turn together.
constexpr float kRestLeast = 0.8f;
constexpr float kRestMost  = 3.2f;
constexpr float kWalkLeast = 1.2f;
constexpr float kWalkMost  = 4.0f;

// Under a full stride. An animal going about its business ambles; the top speed on
// its row is what it runs at when something is wrong, and a creature permanently at
// its own maximum has nothing left to say when it is frightened.
constexpr float kAmble = 0.45f;

// How far ahead the ground is checked, in world pixels, and how far down.
//
// Ahead by about a body's width, so the turn happens at the lip rather than in mid
// air. Down by rather more than the step it could walk down, so a terrace riser is
// not read as a cliff — the point is to refuse a fall, not to refuse a slope.
constexpr float kLookAhead = 14.0f;
constexpr float kLookDown  = 26.0f;

// Below this, a creature that wants to be moving is not moving.
constexpr float kStalled = 6.0f;

// Nothing under its feet, at the step it is about to take.
//
// Asked of the world's own solidity test rather than of the surface height, so
// that a floor somebody built counts and a hole somebody dug counts as well. A
// creature that walked off a player's bridge because the generator says there is
// nothing there would be a creature following the noise rather than the ground.
bool Brink(const mob::Sense &sense, float lean) {
    if (sense.world == nullptr) return false;

    const float ahead = sense.at.x + lean * kLookAhead;

    for (float down = 1.0f; down <= kLookDown; down += 3.0f) {
        if (sense.world->IsSolidAt({ahead, sense.at.y + down})) return false;
    }

    return true;
}

} // namespace

body::Intent mob::Drifter::Think(const Sense &sense, Wits &wits) const {
    wits.holds -= sense.dt;
    wits.since += sense.dt;

    if (wits.holds <= 0.0f) {
        if (wits.mood == kWalking) {
            wits.mood  = kResting;
            wits.holds = Between(wits.seed, kRestLeast, kRestMost);
            wits.lean  = 0.0f;
        } else {
            wits.mood  = kWalking;
            wits.holds = Between(wits.seed, kWalkLeast, kWalkMost);

            // A fresh direction each time it sets off, rather than carrying on the
            // way it was already facing. Otherwise a creature that starts out
            // walking right walks right for the rest of its life, in a series of
            // stages, and a meadow slowly empties itself out to one side.
            wits.lean = (Chance(wits.seed) < 0.5f) ? -1.0f : 1.0f;
        }

        wits.since = 0.0f;
    }

    body::Intent intent;

    if (wits.mood != kWalking) return intent;

    // A creature that floats steers in both axes, and a bat that only ever flew
    // level would be a bat on rails. The vertical wish is a slow figure of its own
    // rather than a second wandering state, because up and down in a cave is a
    // small correction and not a decision.
    if (sense.def != nullptr && sense.def->build.floats) {
        intent.moveY = std::sin(wits.since * 1.7f + static_cast<float>(wits.seed & 0xFFu) * 0.024f) * 0.7f;
    } else if (Brink(sense, wits.lean)) {
        // Nothing ahead. Turn now, and keep the new direction for the rest of this
        // walk rather than turning back next frame — which is what happens if the
        // lean is not written down, and what reads as a creature shivering on the
        // lip of a hole.
        wits.lean = -wits.lean;
    }

    // Trying to move and not moving, which is a wall, a ledge or another creature.
    // All three mean the same thing to a drifter.
    if (sense.grounded && std::fabs(sense.velocity.x) < kStalled && wits.since > 0.25f) {
        wits.lean = -wits.lean;

        // The clock is set back so that the turn is not immediately reconsidered
        // while it is still standing against whatever stopped it.
        wits.since = 0.0f;
    }

    intent.moveX = wits.lean * kAmble;

    return intent;
}

const char *mob::Drifter::Mood(const Wits &wits) const {
    return (wits.mood == kWalking) ? "walk" : "rest";
}

namespace {

// The instance, then the row that points at it, then the registrar that files the
// row. Order inside one translation unit is top to bottom and is guaranteed, which
// is why all three live here rather than in the header.
const mob::Drifter mind;

const mob::BrainDef row = {.name = "drifter", .mind = &mind};

const registry::Registrar<mob::BrainDef> entry{row};

} // namespace

const mob::Drifter &mob::TheDrifter() {
    return mind;
}
