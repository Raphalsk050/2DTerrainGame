#include "drop.h"

#include "config.h"
#include "picture.h"

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

// And the same for a stack the player threw away, which needs far longer.
//
// The pull reaches kReach in every direction, and a stack thrown out of the
// inventory leaves from the player's own chest — so at the settling delay it
// would be back in the bar a third of a second after being let go of, and
// dropping something would simply not work. Two seconds is Minecraft's number
// and it is about how long it takes to turn and walk away from what you threw.
constexpr float kThrownDelay = 2.0f;

// How hard a thrown stack leaves the hand.
//
// Faster than a chip off an axe and flatter, because it has to clear the
// player's own body rather than arc off a trunk. Enough to land it a little way
// off, not enough to lose sight of it.
constexpr float kTossSpeed = 210.0f;
constexpr float kTossLift  = 80.0f;

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

Drops::Pickup *Drops::Claim() {
    for (std::size_t step = 0; step < kSlots; step++) {
        const std::size_t candidate = (next_ + step) % kSlots;

        if (pool_[candidate].live) continue;

        next_ = (candidate + 1) % kSlots;

        return &pool_[candidate];
    }

    return nullptr;
}

void Drops::Scatter(Stack stack, Vector2 from, float away, float now) {
    if (stack.Empty()) return;

    for (int i = 0; i < stack.count; i++) {
        Pickup *pickup = Claim();
        if (pickup == nullptr) return;

        // Seeded off the pool index as well as the loop counter, so a second
        // handful thrown from the same place on the same frame does not land in
        // the first one's footprints.
        const int seed     = static_cast<int>(pickup - pool_.data()) * 31 + i;
        const float spread = Spray(seed);

        pickup->at       = from;
        pickup->velocity = {away * kThrowSpeed * (0.35f + 0.65f * spread),
                            -kThrowLift * (0.6f + 0.5f * Spray(seed + 5))};
        pickup->stack    = {.holds = stack.holds, .what = stack.what, .count = 1};
        pickup->bornAt   = now;
        pickup->holdFor  = kSettleDelay;
        pickup->settled  = false;
        pickup->live     = true;
    }
}

void Drops::Toss(Stack stack, Vector2 from, Vector2 towards, float now) {
    if (stack.Empty()) return;

    Pickup *pickup = Claim();
    if (pickup == nullptr) return;

    const float dx = towards.x - from.x;
    const float dy = towards.y - from.y;

    const float distance = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);

    // Aimed along the throw but always given some lift, so a stack thrown at the
    // ground still leaves the hand on an arc. A throw that went straight down
    // would land under the player's feet, which is where it was already.
    pickup->at       = from;
    pickup->velocity = {(dx / distance) * kTossSpeed, (dy / distance) * kTossSpeed - kTossLift};
    pickup->stack    = stack;
    pickup->bornAt   = now;
    pickup->holdFor  = kThrownDelay;
    pickup->settled  = false;
    pickup->live     = true;
}

void Drops::Update(const World &world, Vector2 player, float dt, float now, Inventory &into) {
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

        if (distance < kCollect && (now - pickup.bornAt) > pickup.holdFor) {
            const int refused = into.Add(pickup.stack);

            if (refused <= 0) {
                pickup.live = false;
                continue;
            }

            // Part of it went and the rest could not. What is left keeps lying
            // there, still being drawn along, and goes in the moment a slot
            // frees up — which is what a player emptying a full bag over a pile
            // of ore expects to happen.
            pickup.stack.count = refused;
        }

        // Drawn to a player who has come close, once it has had a moment to leave
        // the tree and land.
        //
        // Deliberately not conditioned on having settled, which is what it was and
        // what made collecting feel broken: the pull cleared the settled flag as
        // its first act, so the next frame fell through to gravity and the item
        // got one nudge per landing instead of a pull. What a player saw was a
        // thing stuttering towards them a hop at a time.
        const bool drawn = (now - pickup.bornAt) > pickup.holdFor && distance < kReach;

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

        const float pixel = config::kFloraPixel;
        const float side  = kPictureSide * pixel;

        // Snapped to the world's own grid rather than to the pickup's position,
        // so a pickup lying still does not crawl a texel as the view scrolls
        // past it.
        const Vector2 corner = {Snap(pickup.at.x - side * 0.5f), Snap(pickup.at.y - side * 0.5f)};

        DrawPicture(PictureOf(pickup.stack), corner, pixel);
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
