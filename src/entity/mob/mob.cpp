#include "entity/mob/mob.h"

#include "core/config.h"
#include "entity/mob/wardrobe.h"
#include "world/world.h"

#include <algorithm>
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

// How often a creature nobody is looking at makes up its mind, in seconds.
//
// Five times a second. Slow enough to cost nothing and fast enough that a boar off
// screen still turns at a cliff before walking off it — its own nav plan looks a
// stride and a half ahead, and at its bolt that is over a fifth of a second of
// travel.
//
// A creature that is being watched thinks every frame, because a hunter that decided
// five times a second reads as one that keeps changing its mind.
constexpr float kFarThink = 0.2f;

// How many frames of the idle a second.
//
// Slow. An idle is a breath and a twitch of the ear, and the four frames of it are
// nearly the same picture — run it fast and a resting animal reads as a nervous one.
constexpr float kIdleFps = 5.0f;

// Above this it is running rather than walking, as a share of its own walking speed.
//
// Its own and not its sprint, because a creature pushed past a walk *is* running
// whatever it happens to be doing it for — fleeing, or hurrying at a ledge.
constexpr float kRunsAt = 1.05f;

// Below this it is standing still. A shade over the pixel or two a body drifts by
// while settling on uneven ground.
constexpr float kStandsAt = 3.0f;

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

mob::Life mob::Mob::Remember() const {
    Life life;

    life.kind   = kind_;
    life.at     = body_.Position();
    life.health = health_.now;
    life.wits   = wits_;

    return life;
}

void mob::Mob::Restore(const Life &life) {
    body_.PlaceAt(life.at);

    // Clamped rather than trusted. A row whose `hardy` was lowered between one visit
    // and the next would otherwise hand back a creature with more health than its
    // kind can have, which nothing downstream is written to expect.
    health_.now = std::clamp(life.health, 0, health_.most);

    wits_ = life.wits;

    // Whatever it was in the middle of, it is not in the middle of a leap: it has
    // been standing still since the view left. Carrying the timer across would have
    // it hold a jump it never took.
    wits_.legs = {};
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

bool mob::Mob::Update(const World &world, Vector2 quarry, bool quarryVisible, bool watched, float now, float dt) {
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
    //
    // And not at all where the brain never reads the answer, which is two of the three
    // behaviours. See `Brain::Notices`: this was the most expensive thing a creature
    // did, and most creatures were paying it to produce a number nothing looked at.
    sense.seesQuarry = quarryVisible && brain_ != nullptr && brain_->Notices()
                       && sense.toQuarry <= def.notices * 1.6f && Sees(world, centre, quarry);

    sense.world = &world;
    sense.now   = now;
    sense.dt    = dt;

    thinkIn_ -= dt;

    // Asked again, or carrying on with what it last decided. A creature being watched
    // is asked every frame; one that is not is asked a few times a second and walks
    // on its last wish in between. See `kFarThink`.
    //
    // Being hurt always breaks the wait: an animal that took a blow and then spent a
    // fifth of a second finishing its stroll is an animal that did not notice.
    if (watched || thinkIn_ <= 0.0f || stung_) {
        // The dt handed to the brain is the whole stretch it is deciding for, not the
        // frame — otherwise every timer inside a behaviour runs at a fifth speed for
        // a creature off screen, and a fright that should last three seconds lasts
        // fifteen.
        sense.dt = watched ? dt : std::max(dt, kFarThink - thinkIn_);

        wish_    = (brain_ != nullptr) ? brain_->Think(sense, wits_) : body::Intent{};
        thinkIn_ = watched ? 0.0f : kFarThink;
    }

    // Cleared after the think and not before it: a blow that lands between two frames
    // has to survive until the brain has been asked once.
    stung_ = false;

    // A jump is a press and a press is one frame. Left standing, the wish a distant
    // creature is carrying would ask for a jump on every frame until it is asked
    // again, which at a fifth of a second is twelve jumps for one decision.
    const body::Intent intent = wish_;

    wish_.jumpPressed = false;

    body_.Step(intent, world, dt);

    // Facing follows the direction of travel, and only while there is any. A
    // creature that turned to face its own zero velocity would flip about while
    // standing still.
    if (std::fabs(body_.Velocity().x) > 1.0f) facing_ = (body_.Velocity().x >= 0.0f) ? 1 : -1;

    // The animation clocks. Ground covered for the legs, seconds for the breath — and
    // both of them here rather than in `Draw`, because a creature off screen still has
    // to arrive somewhere in its cycle rather than starting from the first frame the
    // moment it is looked at.
    walked_ += std::fabs(body_.Velocity().x) * dt;
    breath_ += dt;

    const bool strikes = (brain_ != nullptr) && brain_->WouldStrike(sense, wits_);

    if (strikes) wits_.rested = def.rest;

    return strikes;
}

void mob::Mob::Draw() const {
    if (!live_) return;

    const Def &def = Made();

    const Color tint = health_.Stung() ? kStung : WHITE;

    // Its feet, which is where a body's own position is.
    const Vector2 feet = {Centre().x, body_.Position().y};

    const Wardrobe &worn = Dressed(def);

    if (worn.Any()) {
        const float going = std::fabs(body_.Velocity().x);

        // Which of the three, and the order matters: a creature in the air keeps
        // whichever gait carried it there rather than snapping to a standing pose
        // halfway over a hole.
        const bool moving  = going > kStandsAt || !body_.Grounded();
        const bool running = going > def.build.runSpeed * kRunsAt;

        const sheet::Strip &clip = !moving         ? worn.idle
                                   : running       ? worn.run
                                                   : worn.walk;

        // The walk and the run are counted in ground covered, the idle in seconds. See
        // `walked_` and `breath_`.
        const int frame = moving ? static_cast<int>(walked_ / std::max(def.stride, 1.0f))
                                 : static_cast<int>(breath_ * kIdleFps);

        // One art pixel to one world pixel. The whole of what makes the sprite come out
        // the size the hand-drawn figure did — see `mob::Def::art`.
        sheet::Draw(clip.Ready() ? clip : worn.idle, frame, feet, 1.0f, facing_, tint);

        return;
    }

    // No art for this one. Drawn from its own row at the world's own texel, so a
    // creature standing beside a wall is drawn on the lattice the wall is. See
    // CLAUDE.md §12 and `figure::Draw`.
    figure::Draw(def.look, feet, static_cast<float>(config::kPixelSize), facing_, tint);
}

const char *mob::Mob::Mood() const {
    if (!live_) return "gone";

    return (brain_ != nullptr) ? brain_->Mood(wits_) : "-";
}
