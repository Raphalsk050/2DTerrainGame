#include "entity/mob/brains/skittish.h"

#include "entity/mob/brains/drifter.h"
#include "entity/mob/brains/whim.h"
#include "entity/mob/mob_def.h"
#include "entity/nav/plan.h"
#include "world/world.h"

#include <cmath>

namespace {

// Moods here start past the drifter's, so that the two never collide in the one
// byte they share. A creature calm enough to be handed to the drifter is left in
// whatever mood the drifter put it in, and this is the range that says otherwise.
constexpr std::uint8_t kFleeing = 16;

// How long a fright lasts. Long enough to get somewhere, short enough that the
// animal is approachable again within a breath.
//
// A fresh blow while already running restarts the clock rather than adding to it, so
// a creature being chased and struck keeps running and one struck once does not run
// for a minute.
constexpr float kFrightLeast = 2.5f;
constexpr float kFrightMost  = 4.5f;

// The least time between two changes of mind, in seconds.
//
// A guard against oscillation and nothing more. It was `kCornered` at 0.45 s and it
// was the freeze: `nav::Plan` stops a refused creature dead, so a frightened animal
// that met anything it would not cross stood perfectly still for half a second, every
// time, while whatever frightened it walked up to it. What that reads as on screen is
// an animal that has given up.
//
// At a sixth of that it turns as soon as it is refused, which is what a cornered
// animal does, and still cannot flip twice in consecutive frames.
constexpr float kSteady = 0.08f;

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
        // Back to grazing, and handed over mid-frame rather than next frame so there
        // is never a tick where the creature is in neither state.
        wits.mood  = 0;
        wits.holds = 0.0f;
        wits.since = 0.0f;

        return TheDrifter().Think(sense, wits);
    }

    if (sense.def == nullptr || sense.world == nullptr) return {};

    bool turn = false;

    // At its bolt, and committed: a frightened animal will take a drop it could not
    // climb back out of, because being stuck later beats being caught now. That is
    // the one thing that differs from the drifter's call, and it is one argument.
    body::Intent intent = nav::Advance(*sense.world, sense.def->build, sense.at, sense.velocity, sense.grounded,
                                       wits.lean, 1.0f, true, true, wits.legs, sense.dt, turn);

    if (turn && wits.since > kSteady) {
        // Cornered: away the other way, at once. Running back past whatever frightened
        // it is a worse answer than running on — and it is a far better one than
        // standing still, which is what it did before.
        wits.lean  = -wits.lean;
        wits.since = 0.0f;

        // And re-planned this frame rather than next, so the creature never spends a
        // tick moving nowhere. A refusal already zeroed `moveX`, and handing that back
        // is the freeze in miniature.
        intent = nav::Advance(*sense.world, sense.def->build, sense.at, sense.velocity, sense.grounded,
                              wits.lean, 1.0f, true, true, wits.legs, 0.0f, turn);
    }

    // Swimming out of trouble rather than drowning in it. A body under water reads a
    // held jump as a stroke upward, so this is what keeps a boar driven into a pond
    // from sitting on the bottom of it.
    //
    // After the navigator rather than before, because it overrides: there is no
    // ground to plan against in the water and every reading the scan took of it is
    // about the bed rather than about the surface.
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
