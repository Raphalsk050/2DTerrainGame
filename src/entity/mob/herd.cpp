#include "entity/mob/herd.h"

#include "entity/drop.h"
#include "entity/mob/brains/whim.h"
#include "core/config.h"
#include "core/profile.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

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
    // creatures put down on one frame do not all step off in the same direction. See
    // `brains/whim.h`.
    slot->Wake(kind, at, Spread(seed_, Roll(seed_)));

    return true;
}

bool mob::Herd::Revive(const Life &life) {
    Mob *slot = Claim();

    if (slot == nullptr) return false;

    slot->Wake(life.kind, life.at, life.wits.seed);
    slot->Restore(life);

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

    next_ = 0;

    warren_.Clear();
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

    // What counts as being looked at: the drawn view, which is the simulated region
    // less the margin it was grown by.
    //
    // It was `active` itself first, and that quietly switched the whole throttle off —
    // the margin is 256 px against a screen of nineteen hundred, so very nearly every
    // creature awake was inside it and every one of them thought sixty times a second.
    // The zone said 0.46 ms and the reason was not the number of creatures.
    const Rectangle watched = {active.x + config::kSimulationMargin, active.y + config::kSimulationMargin,
                               std::max(0.0f, active.width - config::kSimulationMargin * 2.0f),
                               std::max(0.0f, active.height - config::kSimulationMargin * 2.0f)};

    // Everything the view has come to cover. Settling and waking are the warren's;
    // what comes back is a list of creatures to bring to life.
    waking_.clear();

    {
        // Zoned apart from the thinking below, because the two grow for different
        // reasons and a single figure cannot say which. Settling grows with how much
        // ground the view covers; thinking grows with how many creatures are in it.
        PROFILE_ZONE("herd.Wake");

        warren_.Wake(world, active, now, waking_);
    }

    for (const Life &life : waking_) {
        // A pool with no room is the one case where the promise breaks, and it breaks
        // in the right direction: the creature stays in the record and is woken the
        // next time there is a slot, rather than being lost.
        if (!Revive(life)) {
            warren_.Rest(life);
        }
    }

    PROFILE_ZONE("herd.Think");

    for (Mob &one : pool_) {
        if (!one.Live()) continue;

        // Out past the sleeping edge. Written back into the ground rather than thrown
        // away, which is the whole difference between this and what was here before:
        // walking away no longer unmakes anything.
        if (warren_.Sleeping(active, one.Centre())) {
            warren_.Rest(one.Remember());

            one.Sleep();

            continue;
        }

        // Held before the step, because the creature may not survive it and a dead
        // slot is one that can be woken as something else before this loop ends.
        const Def &def = one.Made();

        const Vector2 was = one.Centre();

        const bool strikes = one.Update(world, quarry, true, CheckCollisionRecs(watched, one.Bounds()), now, dt);

        // Died this frame, of a blow or of the sun.
        //
        // Nothing is written down about the death, and that is the design rather than
        // an omission: its cell will never be asked for that kind again, so it does
        // not come back. See `patch.h`. The count is kept only for the display.
        if (!one.Live()) {
            warren_.Lose(was);

            // Its drops are rolled here rather than inside the creature, because what
            // a creature leaves is something that happens *in the world*, and the
            // creature does not have one.
            for (const Spoil &spoil : def.spoils.each) {
                if (spoil.item == nullptr) continue;

                const std::optional<Item> what = item::Named(spoil.item);

                if (!what.has_value()) continue;

                const int many = Count(seed_, spoil.least, spoil.most);

                if (many <= 0) continue;

                drops.Scatter(ItemsOf(*what, many), was, (Chance(seed_) < 0.5f) ? -1.0f : 1.0f, now);
            }

            continue;
        }

        if (!strikes) continue;

        // It landed a blow. Whether that reaches the player is the loop's business:
        // the herd reports, and the character is never mentioned here.
        if (!CheckCollisionRecs(body, one.Bounds())) {
            // Reach is measured centre to centre and a body is a box, so a creature
            // can be within its reach and still not be touching. Both have to hold.
            if (std::fabs(one.Centre().x - quarry.x) > def.reach) continue;
        }

        if (def.hits > toll.damage) {
            toll.damage = def.hits;
            toll.from   = one.Centre();
            toll.knock  = def.knock;
            toll.lift   = def.lift;
        }
    }

    // And last, because a patch closed while its creatures are still in the herd is a
    // patch that will wake an empty version of itself next time.
    warren_.Close(active);

    return toll;
}

void mob::Herd::Census(Rectangle where, std::vector<Life> &out) const {
    for (const Mob &one : pool_) {
        if (!one.Live()) continue;
        if (!CheckCollisionPointRec(one.At(), where)) continue;

        out.push_back(one.Remember());
    }

    warren_.Census(where, out);
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
