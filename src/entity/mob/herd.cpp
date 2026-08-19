#include "entity/mob/herd.h"

#include "entity/body/build.h"
#include "entity/drop.h"
#include "entity/mob/brains/whim.h"
#include "entity/mob/spawner.h"
#include "world/world.h"

#include <cmath>

namespace {

// How often the spawner is asked, in seconds.
//
// Not every frame. An attempt costs a handful of world lookups and a climate
// sample, and the most it can produce is one group — so sixty of them a second
// would be paying a frame's worth of work to place, at best, four animals.
constexpr float kTryEvery = 1.4f;

// How far outside the simulated region a creature is let go, as a multiple of that
// region's own width.
//
// A margin and not the edge itself, because the edge moves with the player and a
// creature dropped the instant it crossed would blink out while still visible at
// the corner of the screen. Wide enough that walking a few steps back does not
// bring it back — a creature that returned would be a different animal wearing the
// same one's place.
constexpr float kLetGo = 0.6f;

// How far apart a group's members are put down, in world pixels.
//
// A sounder arriving on exactly one spot is one animal standing in three others.
constexpr float kSpread = 26.0f;

} // namespace

mob::Mob *mob::Herd::Claim() {
    for (std::size_t n = 0; n < kSlots; n++) {
        Mob &slot = pool_[(next_ + n) % kSlots];

        if (slot.Live()) continue;

        next_ = (next_ + n + 1) % kSlots;

        return &slot;
    }

    return nullptr;
}

bool mob::Herd::Put(Kind kind, Vector2 at) {
    Mob *slot = Claim();

    if (slot == nullptr) return false;

    // Its own stream, mixed from the herd's rather than taken from it, so that four
    // creatures put down on one frame do not all step off in the same direction.
    // See `brains/whim.h`.
    slot->Wake(kind, at, Spread(seed_, Roll(seed_)));

    return true;
}

int mob::Herd::Live() const {
    int count = 0;

    for (const Mob &one : pool_) {
        if (one.Live()) count++;
    }

    return count;
}

int mob::Herd::LiveOf(Kind kind) const {
    int count = 0;

    for (const Mob &one : pool_) {
        if (one.Live() && one.Which() == kind) count++;
    }

    return count;
}

void mob::Herd::Clear() {
    for (Mob &one : pool_) one.Sleep();

    next_  = 0;
    tryIn_ = 0.0f;
}

int mob::Herd::Strike(Rectangle hitbox, int damage, Vector2 from, float knock, float lift) {
    int hit = 0;

    for (Mob &one : pool_) {
        if (!one.Live()) continue;
        if (!CheckCollisionRecs(hitbox, one.Bounds())) continue;

        // A refusal is the mercy window doing its job (see `life::Health`) and not a
        // miss, so it must not be counted as a hit — a caller that ignored the answer
        // would knock a creature about sixty times a second while dealing no damage.
        if (one.Take({.damage = damage, .from = from, .knock = knock, .lift = lift})) hit++;
    }

    return hit;
}

mob::Herd::Toll mob::Herd::Update(World &world, Rectangle active, Rectangle body, Vector2 quarry, float now,
                                  float dt, Drops &drops) {
    Toll toll;

    // Where a creature is let go. Measured off the simulated region rather than the
    // drawn one, so a creature just off screen keeps walking — otherwise the world
    // reorganises itself every time the player turns round.
    const float margin = active.width * kLetGo;

    const Rectangle keep = {active.x - margin, active.y - margin, active.width + margin * 2.0f,
                            active.height + margin * 2.0f};

    for (Mob &one : pool_) {
        if (!one.Live()) continue;

        // Out of the world's reach. Gone rather than frozen: nothing about a
        // creature is remembered, which is the rule at the head of `herd.h` and what
        // keeps this from becoming a second thing that has to be saved.
        if (!CheckCollisionRecs(keep, one.Bounds())) {
            one.Sleep();

            continue;
        }

        // Held before the step, because the creature may not survive it and a dead
        // slot is one that can be woken as something else before this loop ends.
        const Def &def = one.Made();

        const bool strikes = one.Update(world, quarry, true, now, dt);

        // Died this frame, of a blow or of the sun. Its drops are rolled here rather
        // than inside the creature, because what a creature leaves is something that
        // happens *in the world* and the creature does not have one.
        if (!one.Live()) {
            for (const Spoil &spoil : def.spoils.each) {
                if (spoil.item == nullptr) continue;

                const std::optional<Item> what = item::Named(spoil.item);

                if (!what.has_value()) continue;

                const int many = Count(seed_, spoil.least, spoil.most);

                if (many <= 0) continue;

                drops.Scatter(ItemsOf(*what, many), one.Centre(), (Chance(seed_) < 0.5f) ? -1.0f : 1.0f, now);
            }

            continue;
        }

        if (!strikes) continue;

        // It landed a blow. Whether that reaches the player is the loop's business:
        // the herd reports, and the character is never mentioned here.
        if (!CheckCollisionRecs(body, one.Bounds())) {
            // Reach is measured centre to centre and a body is a box, so a creature
            // can be within its reach and still not be touching. Both have to hold.
            const float gap = std::fabs(one.Centre().x - quarry.x);

            if (gap > def.reach) continue;
        }

        if (def.hits > toll.damage) {
            toll.damage = def.hits;
            toll.from   = one.Centre();
            toll.knock  = def.knock;
            toll.lift   = def.lift;
        }
    }

    // And then whether anything new arrives.
    tryIn_ -= dt;

    if (tryIn_ > 0.0f) return toll;

    tryIn_ = kTryEvery;

    const std::optional<spawn::Wish> wish = spawn::Try(world, active, quarry, seed_, *this);

    if (!wish.has_value()) return toll;

    for (int n = 0; n < wish->many; n++) {
        const float sideways = Between(seed_, -kSpread, kSpread) * static_cast<float>(n);

        const Vector2 at = {wish->at.x + sideways, wish->at.y};

        // Each member of a group is placed on its own terms rather than trusted
        // because the first one was. A sounder put down across a ledge would leave
        // half of it standing in the hillside.
        if (n > 0 && !spawn::Suits(world, kinds::Of(wish->kind), at)) continue;

        if (!Put(wish->kind, at)) break;
    }

    return toll;
}

void mob::Herd::Draw(Rectangle view) const {
    for (const Mob &one : pool_) {
        if (!one.Live()) continue;
        if (!CheckCollisionRecs(view, one.Bounds())) continue;

        one.Draw();
    }
}

void mob::Herd::DrawCollision() const {
    for (const Mob &one : pool_) {
        if (!one.Live()) continue;

        DrawRectangleLinesEx(one.Bounds(), 1.0f, YELLOW);

        // What it can notice, which is invisible in play and is the only thing that
        // explains an animal reacting too early or not at all.
        DrawCircleLinesV(one.Centre(), one.Made().notices, Fade(YELLOW, 0.25f));

        // And its reach, where it has one.
        if (one.Made().hits > 0) DrawCircleLinesV(one.Centre(), one.Made().reach, Fade(RED, 0.5f));
    }
}
