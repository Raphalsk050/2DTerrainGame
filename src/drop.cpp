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

// How near two settled pickups of one kind have to be before they become one, in
// world pixels.
//
// About two blocks, which is the spread a felled tree throws its wood over — so a
// felling gathers into one or two piles rather than staying as the handful of
// separate pieces that made the fall read. Wider and a pile reaches across a
// clearing and hoovers up something the player deliberately left somewhere else;
// narrower and a scatter never closes up at all.
constexpr float kGather = 32.0f;

// Nothing here expires.
//
// There was a ninety second lifetime, and it was simply a way of losing the
// player's material: a bag that fills while a hillside is being dug leaves the
// overflow on the ground, and the overflow is exactly what the player has to walk
// back for. Minecraft's five minutes is a concession to a server holding
// thousands of entities; this pool holds two hundred and fifty six, they gather
// into stacks as they settle, and they are gone the moment the view leaves them
// anyway. There is nothing left for a timer to buy.



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

void Drops::Spill(Stack &stack, Vector2 near) {
    if (stack.Empty()) return;

    // Nearest first, so what cannot have a slot of its own joins the pile it
    // would have landed in rather than one on the far side of the wood. A pass
    // per unit poured, over a pool that is by definition full — which only ever
    // happens once something upstream has gone very wrong, and is the one moment
    // it is worth being slow to avoid losing anything.
    while (!stack.Empty()) {
        Pickup *best   = nullptr;
        float nearest  = 0.0f;

        for (Pickup &pickup : pool_) {
            if (!pickup.live || !pickup.stack.Alike(stack) || pickup.stack.Room() <= 0) continue;

            const float dx = pickup.at.x - near.x;
            const float dy = pickup.at.y - near.y;

            const float away = dx * dx + dy * dy;

            if (best != nullptr && away >= nearest) continue;

            best    = &pickup;
            nearest = away;
        }

        if (best == nullptr) return;

        const int poured = std::min(best->stack.Room(), stack.count);

        best->stack.count += poured;
        stack.count -= poured;
    }
}

void Drops::Scatter(Stack stack, Vector2 from, float away, float now) {
    if (stack.Empty()) return;

    // How the throw is divided. One piece per unit up to the bound, and then the
    // remainder rides on the pieces evenly — so a tree's four to nine pieces of
    // wood arrive as four to nine objects, and a slot of stone arrives as eight
    // of eight rather than as sixty-four of one.
    const int pieces = std::min(stack.count, kPieces);
    const int each   = stack.count / pieces;
    int spare        = stack.count - each * pieces;

    for (int i = 0; i < pieces; i++) {
        const int count = each + ((spare > 0) ? 1 : 0);
        if (spare > 0) spare--;

        Pickup *pickup = Claim();

        // The pool is full. What is left goes into whatever of its own kind is
        // already lying here, because the one thing that must not happen is the
        // player's material quietly ceasing to exist.
        if (pickup == nullptr) {
            Stack rest = {.holds = stack.holds, .what = stack.what, .count = count + each * (pieces - i - 1) + spare};

            Spill(rest, from);
            return;
        }

        // Seeded off the pool index as well as the loop counter, so a second
        // handful thrown from the same place on the same frame does not land in
        // the first one's footprints.
        const int seed     = static_cast<int>(pickup - pool_.data()) * 31 + i;
        const float spread = Spray(seed);

        pickup->at       = from;
        pickup->velocity = {away * kThrowSpeed * (0.35f + 0.65f * spread),
                            -kThrowLift * (0.6f + 0.5f * Spray(seed + 5))};
        pickup->stack    = {.holds = stack.holds, .what = stack.what, .count = count};
        pickup->bornAt   = now;
        pickup->holdFor  = kSettleDelay;
        pickup->settled  = false;
        pickup->live     = true;
    }
}

void Drops::Toss(Stack stack, Vector2 from, Vector2 towards, float now) {
    if (stack.Empty()) return;

    Pickup *pickup = Claim();

    if (pickup == nullptr) {
        Spill(stack, from);
        return;
    }

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

void Drops::Merge() {
    for (std::size_t i = 0; i < kSlots; i++) {
        Pickup &into = pool_[i];

        // Only into something that has come to rest. A pickup still in the air
        // is mid-arc and part of what the throw is saying, and one being drawn
        // towards the player is on its way out of the world — swallowing either
        // makes a piece disappear in front of the eye that was following it.
        if (!into.live || !into.settled || into.stack.Room() <= 0) continue;

        for (std::size_t j = i + 1; j < kSlots; j++) {
            Pickup &from = pool_[j];

            if (!from.live || !from.settled || !from.stack.Alike(into.stack)) continue;

            const float dx = from.at.x - into.at.x;
            const float dy = from.at.y - into.at.y;

            if (dx * dx + dy * dy > kGather * kGather) continue;

            const int poured = std::min(into.stack.Room(), from.stack.count);
            if (poured <= 0) break;

            into.stack.count += poured;
            from.stack.count -= poured;

            // The younger of the two decides when the pile may be picked up, so
            // a stack thrown away and then landed on by a felling cannot come
            // back to hand before the throw's own hold is up.
            if (from.bornAt + from.holdFor > into.bornAt + into.holdFor) {
                into.bornAt  = from.bornAt;
                into.holdFor = from.holdFor;
            }

            if (from.stack.count <= 0) from.live = false;

            if (into.stack.Room() <= 0) break;
        }
    }
}

void Drops::Update(const World &world, Vector2 player, float dt, float now, Inventory &into) {
    Merge();

    for (Pickup &pickup : pool_) {
        if (!pickup.live) continue;

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

        // Whether there is anywhere for it to go at all.
        //
        // Asked before the pull rather than only at the moment of collection, and
        // that is the whole of what was wrong with a full bag: a pickup with no
        // room waiting for it was still drawn in, still could not be taken, and
        // so orbited the player for ever — a shoal of wood following them about
        // the world. What a full bag means is that the thing stays on the ground,
        // and staying on the ground includes staying where it is.
        const bool wanted = into.Room(pickup.stack) > 0;

        if (wanted && distance < kCollect && (now - pickup.bornAt) > pickup.holdFor) {
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
        const bool drawn = wanted && (now - pickup.bornAt) > pickup.holdFor && distance < kReach;

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

void Drops::Draw() const {
    for (const Pickup &pickup : pool_) {
        if (!pickup.live) continue;

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
