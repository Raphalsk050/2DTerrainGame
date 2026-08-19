#include "entity/mob/brains/stalker.h"

#include "entity/mob/brains/drifter.h"
#include "entity/mob/brains/whim.h"
#include "entity/mob/mob_def.h"
#include "world/world.h"

#include <cmath>

namespace {

// Past the drifter's moods and past the skittish one's, so the one byte the three
// share can never be read as the wrong state by whichever brain is in charge.
constexpr std::uint8_t kHunting = 32;

// How long the quarry may stay out of reach before the hunt is given up.
//
// A few seconds rather than instantly, so that a player ducking behind a hillside
// is followed round it instead of forgotten the moment the line of sight breaks.
constexpr float kPatience = 4.0f;

// How much further than `notices` the quarry has to get before the chase ends
// outright, as a multiple of it.
//
// A margin, and it is what stops a creature at exactly the noticing distance from
// flickering between hunting and wandering every frame — the same hysteresis a
// thermostat needs and for the same reason.
constexpr float kLoses = 1.6f;

// Below this, a creature that wants to be moving is not moving.
constexpr float kStalled = 8.0f;

// How long it has to be blocked before it tries jumping, and how long between
// attempts. Not every frame: a jump pressed every frame is a jump held, and the
// body reads the hold for the height of the arc.
constexpr float kShoveFor = 0.18f;
constexpr float kHopEvery = 0.45f;

// How close the quarry has to be before a drop is worth taking. About a screen of
// this world at the framing it is played at.
constexpr float kCommitted = 90.0f;

// How far ahead the ground is checked, and how far down. The drifter's figures,
// deliberately the same: what counts as a cliff is a fact about this world's
// terraces and not about the creature reading it.
constexpr float kLookAhead = 14.0f;
constexpr float kLookDown  = 26.0f;

bool Brink(const mob::Sense &sense, float lean) {
    if (sense.world == nullptr) return false;

    const float ahead = sense.at.x + lean * kLookAhead;

    for (float down = 1.0f; down <= kLookDown; down += 3.0f) {
        if (sense.world->IsSolidAt({ahead, sense.at.y + down})) return false;
    }

    return true;
}

} // namespace

body::Intent mob::Stalker::Think(const Sense &sense, Wits &wits) const {
    const float noticing = (sense.def != nullptr) ? sense.def->notices : 260.0f;

    // Being hurt starts a hunt whatever it was doing, and it does not matter
    // whether the blow came from the thing it can see: something is there.
    if (sense.stung) {
        wits.mood  = kHunting;
        wits.holds = kPatience;
    }

    if (wits.mood != kHunting) {
        // Nothing to hunt. Wander, which is the drifter's whole job and is not
        // written a second time here.
        if (!sense.seesQuarry || sense.toQuarry > noticing) return TheDrifter().Think(sense, wits);

        wits.mood  = kHunting;
        wits.holds = kPatience;
        wits.since = 0.0f;
    }

    wits.since += sense.dt;

    // The clock only runs while the quarry is *not* worth chasing, so a hunt that
    // is going well never times out.
    const bool worth = sense.seesQuarry && sense.toQuarry <= noticing * kLoses;

    wits.holds = worth ? kPatience : (wits.holds - sense.dt);

    if (wits.holds <= 0.0f) {
        wits.mood  = 0;
        wits.holds = 0.0f;
        wits.since = 0.0f;

        return TheDrifter().Think(sense, wits);
    }

    body::Intent intent;

    const float toward = (sense.quarry.x > sense.at.x) ? 1.0f : -1.0f;

    wits.lean = toward;

    // A floating hunter steers straight at its quarry in both axes, which is the
    // whole difference between a bat and a thing that has noticed you.
    if (sense.def != nullptr && sense.def->build.floats) {
        const float rise = sense.quarry.y - sense.at.y;

        intent.moveX = toward;
        intent.moveY = std::clamp(rise / 60.0f, -1.0f, 1.0f);

        return intent;
    }

    // A drop is refused until the chase is nearly over. See the head of the header:
    // refusing every one leaves a creature stopped by a single step, and refusing
    // none empties the hillside into the nearest ravine.
    if (Brink(sense, toward) && sense.toQuarry > kCommitted && sense.grounded) {
        // Held at the edge rather than turned away from it. A hunter that turned
        // round would be running from what it is chasing.
        intent.moveX = 0.0f;

        return intent;
    }

    intent.moveX      = toward;
    intent.sprintHeld = true;

    // Blocked, which on this terrain is nearly always a ledge. Jumping is what a
    // drifter's turn is: the same reading, the opposite answer.
    const bool blocked = sense.grounded && std::fabs(sense.velocity.x) < kStalled && wits.since > kShoveFor;

    if (blocked && std::fmod(wits.since, kHopEvery) < sense.dt) {
        intent.jumpPressed = true;
        intent.jumpHeld    = true;
    }

    // Out of the water rather than down in it, on the same terms the skittish brain
    // swims: a body under water reads a held jump as a stroke upward.
    if (sense.swimming) intent.jumpHeld = true;

    return intent;
}

bool mob::Stalker::WouldStrike(const Sense &sense, const Wits &wits) const {
    if (wits.mood != kHunting) return false;
    if (wits.rested > 0.0f) return false;
    if (sense.def == nullptr || sense.def->hits <= 0) return false;

    return sense.seesQuarry && sense.toQuarry <= sense.def->reach;
}

const char *mob::Stalker::Mood(const Wits &wits) const {
    if (wits.mood == kHunting) return "hunt";

    return TheDrifter().Mood(wits);
}

namespace {

const mob::Stalker mind;

const mob::BrainDef row = {.name = "stalker", .mind = &mind};

const registry::Registrar<mob::BrainDef> entry{row};

} // namespace

const mob::Stalker &mob::TheStalker() {
    return mind;
}
