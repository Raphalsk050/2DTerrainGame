#include "entity/mob/brains/drifter.h"

#include "entity/mob/brains/whim.h"
#include "entity/mob/mob_def.h"
#include "entity/nav/plan.h"
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

// Below this, a creature that wants to be moving is not moving.
constexpr float kStalled = 6.0f;

// The least time between two changes of mind. A guard against flipping on consecutive
// frames and nothing more — see the same constant in `skittish.cpp`.
constexpr float kSteady = 0.08f;

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

    // A resting creature still has to be told to stand still rather than simply not
    // asked, because a body handed nothing keeps whatever it had — and a leap
    // committed to on the last frame of a walk has to be allowed to finish. The
    // navigator is what knows about that, so it is called even here.
    const bool walking = (wits.mood == kWalking);

    if (sense.def == nullptr || sense.world == nullptr) return intent;

    // A creature that floats steers in both axes and has no ground to read. The
    // vertical wish is a slow figure of its own rather than a second wandering
    // state, because up and down in a cave is a small correction and not a decision.
    if (sense.def->build.floats) {
        if (!walking) return intent;

        intent.moveX = wits.lean * kAmble;
        intent.moveY = std::sin(wits.since * 1.7f + static_cast<float>(wits.seed & 0xFFu) * 0.024f) * 0.7f;

        return intent;
    }

    bool turn = false;

    intent = nav::Advance(*sense.world, sense.def->build, sense.at, sense.velocity, sense.grounded, wits.lean,
                          walking ? kAmble : 0.0f, false, false, wits.legs, sense.dt, turn);

    if (!walking) return intent;

    // The navigator says this way is no good: a hole it cannot clear, or a riser it
    // cannot climb. A wandering animal simply goes the other way — which is the whole
    // difference between it and a hunter, and is why the decision is here and not in
    // `nav::Plan`.
    //
    // The new direction is written down rather than acted on for one frame, or the
    // creature reconsiders next frame, turns back, and shivers on the lip of the hole
    // for as long as it stands there.
    if (turn && wits.since > kSteady) {
        wits.lean  = -wits.lean;
        wits.since = 0.0f;

        // Re-planned this frame rather than next. A refusal zeroed `moveX`, and giving
        // that back is a creature that stands still for a tick every time it changes
        // its mind — which on a broken hillside is most of the time.
        intent = nav::Advance(*sense.world, sense.def->build, sense.at, sense.velocity, sense.grounded, wits.lean,
                              kAmble, false, false, wits.legs, 0.0f, turn);

        return intent;
    }

    // Trying to move and not moving, which is a wall the scan did not resolve,
    // another creature, or a corner. All of them mean the same thing to a drifter.
    //
    // Kept as well as the navigator rather than replaced by it: the scan reads the
    // ground and cannot see a body standing in the way, and this is the one reading
    // that catches everything the ground does not explain.
    if (sense.grounded && std::fabs(sense.velocity.x) < kStalled && wits.since > 0.25f
        && wits.legs.leaping <= 0.0f) {
        wits.lean  = -wits.lean;
        wits.since = 0.0f;
    }

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
