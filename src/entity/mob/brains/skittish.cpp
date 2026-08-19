#include "entity/mob/brains/skittish.h"

#include "entity/mob/brains/drifter.h"
#include "entity/mob/brains/whim.h"
#include "entity/mob/mob_def.h"

#include <cmath>

namespace {

// Moods here start past the drifter's, so that the two never collide in the one
// byte they share. A creature calm enough to be handed to the drifter is left in
// whatever mood the drifter put it in, and this is the range that says otherwise.
constexpr std::uint8_t kFleeing = 16;

// How long a fright lasts. Long enough to get somewhere, short enough that the
// animal is approachable again within a breath.
constexpr float kFrightLeast = 2.5f;
constexpr float kFrightMost  = 4.5f;

// A fresh blow while already running restarts the clock rather than adding to it,
// so a creature being chased and struck keeps running and one struck once does not
// run for a minute.

// How often it will jump while running, and how much of a gap between attempts.
//
// A fleeing animal that never jumped would pile up against the first terrace riser
// it met, which is the shape of "the boar ran into a hill and stayed there". It is
// a chance rather than a rule because a creature that jumped every ledge perfectly
// would look like it was following a path.
constexpr float kHopChance = 0.55f;
constexpr float kHopEvery  = 0.6f;

} // namespace

body::Intent mob::Skittish::Think(const Sense &sense, Wits &wits) const {
    // A blow starts or renews the fright, whatever it was doing.
    if (sense.stung) {
        wits.mood  = kFleeing;
        wits.holds = Between(wits.seed, kFrightLeast, kFrightMost);
        wits.since = 0.0f;

        // Away from what hit it. Decided once, at the moment of the blow, rather
        // than every frame against the attacker's current position: a creature that
        // re-aimed continuously would circle its attacker as they walked around it,
        // which reads as being drawn to the thing that hurt it.
        wits.lean = (sense.stungFrom.x > sense.at.x) ? -1.0f : 1.0f;
    }

    if (wits.mood != kFleeing) return TheDrifter().Think(sense, wits);

    wits.holds -= sense.dt;
    wits.since += sense.dt;

    if (wits.holds <= 0.0f) {
        // Back to grazing, and handed over mid-frame rather than next frame so
        // there is never a tick where the creature is in neither state.
        wits.mood  = 0;
        wits.holds = 0.0f;
        wits.since = 0.0f;

        return TheDrifter().Think(sense, wits);
    }

    body::Intent intent;

    intent.moveX     = wits.lean;
    intent.sprintHeld = true;

    // Jumping is attempted on a cadence rather than every frame, because a jump
    // pressed every frame is a jump held: the body reads `jumpHeld` for the height
    // of the arc, and a creature that never lets go always makes the full one.
    if (sense.grounded && std::fmod(wits.since, kHopEvery) < sense.dt) {
        if (Chance(wits.seed) < kHopChance) {
            intent.jumpPressed = true;
            intent.jumpHeld    = true;
        }
    }

    // Swimming out of trouble rather than drowning in it. A body under water reads
    // the same wish as a stroke upward, so this is what keeps a boar driven into a
    // pond from sitting on the bottom of it.
    if (sense.swimming) intent.jumpHeld = true;

    return intent;
}

const char *mob::Skittish::Mood(const Wits &wits) const {
    if (wits.mood == kFleeing) return "flee";

    return TheDrifter().Mood(wits);
}

namespace {

const mob::Skittish mind;

const mob::BrainDef row = {.name = "skittish", .mind = &mind};

const registry::Registrar<mob::BrainDef> entry{row};

} // namespace

const mob::Skittish &mob::TheSkittish() {
    return mind;
}
