#include "entity/mob/mob.h"

#include "core/config.h"
#include "world/world.h"

#include <cmath>

namespace {

// How far a creature will look through the ground to see its quarry, and how
// finely. Nothing here is a proper line of sight — it is a walk along the straight
// line between two centres, refusing at the first solid step.
//
// Coarse on purpose. A creature that could see through a one-pixel gap in a
// hillside would notice the player through the floor, and one sampled finely enough
// to be right would cost a world lookup every few pixels for every creature every
// frame.
constexpr float kSightStep = 12.0f;

// Beyond this the sight test is not run at all: nothing in this world notices
// anything at half a screen, and walking the line is the expensive part.
constexpr float kSightMost = 420.0f;

// What a hurt creature is tinted by. A multiply, so it can only darken — which is
// why this is a red that removes the green and blue rather than a white that could
// not brighten anything. See `figure::Draw`.
constexpr Color kStung = {255, 96, 96, 255};

bool Sees(const World &world, Vector2 from, Vector2 to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;

    const float span = std::sqrt(dx * dx + dy * dy);

    if (span > kSightMost) return false;
    if (span <= kSightStep) return true;

    const int steps = static_cast<int>(span / kSightStep);

    for (int s = 1; s < steps; s++) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);

        if (world.IsSolidAt({from.x + dx * t, from.y + dy * t})) return false;
    }

    return true;
}

} // namespace

void mob::Mob::Wake(Kind kind, Vector2 at, std::uint32_t seed) {
    const Def &def = kinds::Of(kind);

    kind_ = kind;
    body_ = body::Body(def.build, at);

    wits_      = {};
    wits_.seed = seed;

    health_      = {};
    health_.most = def.hardy;
    health_.now  = def.hardy;

    brain_ = brain::Find(def.temper);

    facing_    = 1;
    stung_     = false;
    stungFrom_ = {};
    live_      = true;
}

bool mob::Mob::Take(const life::Blow &blow) {
    if (!live_) return false;
    if (!health_.Hurt(blow.damage)) return false;

    // Thrown away from where the blow came from. The direction is derived here and
    // not passed in, so that every source of damage throws a creature the same way
    // — see `life::Blow` for why the struct carries a position rather than a
    // direction.
    const float away = (blow.from.x > Centre().x) ? -1.0f : 1.0f;

    body_.Shove({away * blow.knock, -blow.lift});

    // Remembered for exactly one think. What the brain does with it is the brain's
    // business: a skittish creature bolts, a hunter turns on it, a drifter never
    // looks at it at all.
    stung_     = true;
    stungFrom_ = blow.from;

    if (!health_.Alive()) live_ = false;

    return true;
}

bool mob::Mob::Update(const World &world, Vector2 quarry, bool quarryVisible, float now, float dt) {
    if (!live_) return false;

    const Def &def = Made();

    health_.Tick(dt);
    wits_.rested = std::max(0.0f, wits_.rested - dt);

    // The sun, for anything it destroys. Read at the creature's own position and
    // through the solved light rather than off a clock, so standing in a doorway is
    // genuinely different from standing in the room behind it — and so a torch does
    // not save it, because a torch is not the sun.
    //
    // It kills outright rather than over time. A creature burning down over several
    // seconds needs a second timer, a second bar and a decision about whether the
    // player is credited for it; at dawn what is wanted is that the night is over.
    if (def.burnsInDaylight && world.Sky().Today().light > 0.55f) {
        const float above = world.LightLevelAt(Centre());

        if (above > 0.75f) {
            live_ = false;

            return false;
        }
    }

    const Vector2 centre = Centre();

    const float dx = quarry.x - centre.x;
    const float dy = quarry.y - centre.y;

    Sense sense;

    sense.def      = &def;
    sense.at       = body_.Position();
    sense.velocity = body_.Velocity();
    sense.grounded = body_.Grounded();
    sense.swimming = body_.Swimming();

    sense.stung     = stung_;
    sense.stungFrom = stungFrom_;

    sense.quarry   = quarry;
    sense.toQuarry = std::sqrt(dx * dx + dy * dy);

    // Distance first, then the walk along the line — in that order, because the
    // distance is three multiplies and the walk is a few dozen world lookups.
    sense.seesQuarry = quarryVisible && sense.toQuarry <= def.notices * 1.6f
                       && Sees(world, centre, quarry);

    sense.world = &world;
    sense.now   = now;
    sense.dt    = dt;

    const body::Intent intent = (brain_ != nullptr) ? brain_->Think(sense, wits_) : body::Intent{};

    // Cleared after the think and not before it: a blow that lands between two
    // frames has to survive until the brain has been asked once.
    stung_ = false;

    body_.Step(intent, world, dt);

    // Facing follows the direction of travel, and only while there is any. A
    // creature that turned to face its own zero velocity would flip about while
    // standing still.
    if (std::fabs(body_.Velocity().x) > 1.0f) facing_ = (body_.Velocity().x >= 0.0f) ? 1 : -1;

    const bool strikes = (brain_ != nullptr) && brain_->WouldStrike(sense, wits_);

    if (strikes) wits_.rested = def.rest;

    return strikes;
}

void mob::Mob::Draw() const {
    if (!live_) return;

    // Drawn at the world's own texel, so a creature standing beside a wall is drawn
    // on the lattice the wall is. See CLAUDE.md §12 and `figure::Draw`.
    figure::Draw(Made().look, {Centre().x, body_.Position().y}, static_cast<float>(config::kPixelSize), facing_,
                 health_.Stung() ? kStung : WHITE);
}

const char *mob::Mob::Mood() const {
    if (!live_) return "gone";

    return (brain_ != nullptr) ? brain_->Mood(wits_) : "-";
}
