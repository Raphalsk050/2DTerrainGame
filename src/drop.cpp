#include "drop.h"

#include "config.h"

#include <algorithm>
#include <cmath>

namespace {

// Thrown out at about the speed a chip of wood leaves an axe, and no faster: the
// arc has to read as the tree giving something up, not as an explosion.
constexpr float kThrowSpeed = 120.0f;
constexpr float kThrowLift  = 150.0f;

constexpr float kGravity   = 620.0f;
constexpr float kMaxFall   = 520.0f;
constexpr float kBounce    = 0.32f;
constexpr float kFriction  = 7.0f;

// Distance the player draws a pickup from, and the distance at which it is
// taken. The reach is generous on purpose — hunting for the last acorn in the
// grass is nobody's idea of a reward.
constexpr float kReach   = 64.0f;
constexpr float kCollect = 13.0f;
constexpr float kPull    = 900.0f;

// How fast a pickup being drawn in loses whatever it was doing before. Without
// it the arc it was thrown on fights the pull all the way to the player and the
// thing arrives in a spiral.
constexpr float kHoming = 9.0f;

// Seconds before the pull takes hold, so an item visibly leaves the tree and
// lands before it comes to hand. Collected at the instant of the drop, a felling
// reads as a number going up rather than as a tree giving something up.
constexpr float kSettleDelay = 0.45f;

// How fast a pickup that found itself inside the ground climbs out, in pixels a
// second.
//
// A tree that falls against a hillside puts its wood inside the hill. Every axis
// of movement is then blocked, and without this the drop sits in the rock until
// it times out — visible, unreachable, and looking exactly like a bug because it
// is one.
constexpr float kDigOut = 90.0f;

// Seconds before a pickup nobody came for gives up. Long enough to fell a second
// tree and come back, short enough that a wood the player has worked through is
// not still littered an hour later.
constexpr float kLifetime = 90.0f;

// Seconds of the end of that spent blinking, so a pickup about to go says so.
constexpr float kWarning = 6.0f;



float Snap(float value) { return std::floor(value / config::kFloraPixel) * config::kFloraPixel; }

// A cheap spread, so a handful of wood does not leave the trunk as one clump.
float Spray(int index) {
    auto bits = static_cast<unsigned int>(index) * 2654435761u;
    bits ^= bits >> 15;
    bits *= 2246822519u;
    bits ^= bits >> 13;

    return static_cast<float>(bits & 0xffffu) / 65535.0f;
}

} // namespace

void Drops::Spawn(Item item, int count, Vector2 from, float away, float now) {
    for (int i = 0; i < count; i++) {
        // One full pass and no more. A pool this size only fills if something has
        // gone wrong upstream, and dropping the overflow is the right answer:
        // the alternative is taking a slot from a pickup the player can see.
        std::size_t slot = kSlots;

        for (std::size_t step = 0; step < kSlots; step++) {
            const std::size_t candidate = (next_ + step) % kSlots;

            if (pool_[candidate].live) continue;

            slot  = candidate;
            next_ = (candidate + 1) % kSlots;
            break;
        }

        if (slot >= kSlots) return;

        const float spread = Spray(static_cast<int>(slot) * 31 + i);

        Pickup &pickup = pool_[slot];

        pickup.at       = from;
        pickup.velocity = {away * kThrowSpeed * (0.35f + 0.65f * spread),
                           -kThrowLift * (0.6f + 0.5f * Spray(static_cast<int>(slot) * 17 + i + 5))};
        pickup.item     = item;
        pickup.bornAt   = now;
        pickup.settled  = false;
        pickup.live     = true;
    }
}

void Drops::Update(const World &world, Vector2 player, float dt, float now, Harvest &into) {
    for (Pickup &pickup : pool_) {
        if (!pickup.live) continue;

        if (now - pickup.bornAt > kLifetime) {
            pickup.live = false;
            continue;
        }

        // Buried. A tree that goes over into a hillside leaves its wood inside
        // the hill, and every axis of movement is blocked from in there — so it
        // has to be lifted out before anything else is asked of it. Straight up,
        // because up is where the open air is when the ground is what you are in.
        if (world.IsSolidAt(pickup.at)) {
            pickup.at.y -= kDigOut * dt;
            pickup.velocity = {};
            pickup.settled  = false;
            continue;
        }

        const float dx = player.x - pickup.at.x;
        const float dy = player.y - pickup.at.y;

        const float distance = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);

        if (distance < kCollect) {
            into[ItemIndex(pickup.item)]++;
            pickup.live = false;
            continue;
        }

        // Drawn to a player who has come close, once it has had a moment to leave
        // the tree and land.
        //
        // Deliberately not conditioned on having settled, which is what it was and
        // what made collecting feel broken: the pull cleared the settled flag as
        // its first act, so the next frame fell through to gravity and the item
        // got one nudge per landing instead of a pull. What a player saw was a
        // thing stuttering towards them a hop at a time.
        const bool drawn = (now - pickup.bornAt) > kSettleDelay && distance < kReach;

        if (drawn) {
            const float share = 0.35f + 0.65f * (1.0f - distance / kReach);

            // Steered rather than nudged: the velocity is turned towards the
            // player as well as added to, so whatever arc it was on gives way
            // instead of fighting the pull the whole distance.
            const float wanted = kPull * share;

            const float toward = std::min(kHoming * dt, 1.0f);

            pickup.velocity.x += ((dx / distance) * wanted - pickup.velocity.x) * toward;
            pickup.velocity.y += ((dy / distance) * wanted - pickup.velocity.y) * toward;
        } else {
            pickup.velocity.y = std::min(pickup.velocity.y + kGravity * dt, kMaxFall);
        }

        // Moved one axis at a time, the same way a body is, so a pickup landing on
        // a slope slides along it instead of catching on the corner it hit.
        //
        // While it is being drawn in, the ground is ignored. A pickup that has to
        // climb a lip of rock to reach a player standing right beside it is the
        // other half of what made this feel bad, and nothing about an item on its
        // way to a hand is worth simulating.
        const float stepX = pickup.velocity.x * dt;
        const float stepY = pickup.velocity.y * dt;

        if (drawn) {
            pickup.at.x += stepX;
            pickup.at.y += stepY;
            pickup.settled = false;
            continue;
        }

        if (!world.IsSolidAt({pickup.at.x + stepX, pickup.at.y})) {
            pickup.at.x += stepX;
        } else {
            pickup.velocity.x = 0.0f;
        }

        if (!world.IsSolidAt({pickup.at.x, pickup.at.y + stepY})) {
            pickup.at.y += stepY;
            if (std::fabs(stepY) > 0.01f) pickup.settled = false;
        } else if (stepY > 0.0f) {
            // Landed. A small bounce and then it stays, which is enough to read as
            // something with weight without needing a settle test.
            pickup.velocity.y = -pickup.velocity.y * kBounce;

            if (std::fabs(pickup.velocity.y) < 30.0f) {
                pickup.velocity.y = 0.0f;
                pickup.settled    = true;
            }
        } else {
            pickup.velocity.y = 0.0f;
        }

        // Ground drag, so a pickup does not skate away down a hill.
        if (pickup.settled) {
            pickup.velocity.x -= pickup.velocity.x * std::min(kFriction * dt, 1.0f);
        }
    }
}

void Drops::Draw(float now) const {
    for (const Pickup &pickup : pool_) {
        if (!pickup.live) continue;

        // Blinking out its last few seconds, on the same clock it is aged by, so
        // it keeps time with itself under fast weather.
        const float remaining = kLifetime - (now - pickup.bornAt);

        if (remaining < kWarning && std::fmod(remaining, 0.36f) < 0.18f) continue;

        const ItemDef &def = Def(pickup.item);

        const float pixel = config::kFloraPixel;
        const float side  = kItemArt * pixel;

        const float left = Snap(pickup.at.x - side * 0.5f);
        const float top  = Snap(pickup.at.y - side * 0.5f);

        for (int row = 0; row < kItemArt; row++) {
            for (int col = 0; col < kItemArt; col++) {
                const char mark = def.art[row][col];
                if (mark < 'a' || mark >= static_cast<char>('a' + kItemTones)) continue;

                DrawRectangleV({left + static_cast<float>(col) * pixel, top + static_cast<float>(row) * pixel},
                               {pixel, pixel}, def.tone[static_cast<std::size_t>(mark - 'a')]);
            }
        }
    }
}

void Drops::Clear() {
    for (Pickup &pickup : pool_) pickup.live = false;

    next_ = 0;
}

int Drops::Live() const {
    int count = 0;

    for (const Pickup &pickup : pool_) count += pickup.live ? 1 : 0;

    return count;
}
