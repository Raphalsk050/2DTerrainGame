#pragma once

#include "inventory.h"
#include "raylib.h"
#include "stack.h"
#include "world.h"

#include <array>
#include <cstddef>

// Things lying on the ground waiting to be picked up.
//
// The first thing in this project that is an object rather than a field or a
// function of time, and it is deliberately the smallest one that could be: a
// fixed ring of slots, no allocation, no identity, nothing remembered past the
// moment a pickup is collected or times out. It exists because "the tree drops
// its wood" reads as nothing at all when the wood simply appears in a counter,
// and because the arc from the trunk to the ground is most of what makes felling
// a tree feel like it happened.
//
// A pickup is not a plant and not a material. It does not generate, it is not a
// pure function of anywhere, and it must never be asked to survive being walked
// away from — anything still lying on the ground when the view leaves is gone.
// That is what keeps this from becoming an entity system by accident.
class Drops {
public:
    // Throws a stack out of a point one at a time, spread over a small arc away
    // from the side the blow came from.
    //
    // One pickup per unit, which is the whole reason the class exists: the arc
    // from the trunk to the ground is most of what makes felling a tree feel
    // like it happened, and a single object carrying six wood does not read as
    // six of anything.
    void Scatter(Stack stack, Vector2 from, float away, float now);

    // Throws a whole stack out as one object, towards a point.
    //
    // The other half of the pair, and it has to be the other half. A player
    // emptying a full slot onto the ground would spend a quarter of the pool on
    // one gesture if this scattered, and would then have to walk over
    // sixty-four separate things to undo a misclick.
    void Toss(Stack stack, Vector2 from, Vector2 towards, float now);

    // Falls, settles, drifts to a nearby player and is collected into `into`.
    //
    // Whatever will not fit is left lying where it is. A full inventory is a
    // reason for something to stay on the ground, not a reason for it to stop
    // existing.
    void Update(const World &world, Vector2 player, float dt, float now, Inventory &into);

    // Drawn with the plants, and so inside the light. `now` is the same clock it
    // is aged by, so a pickup blinks in time with its own expiry.
    void Draw(float now) const;

    void Clear();

    int Live() const;

private:
    // Two hundred and fifty six is a felled oak's worth several times over. A
    // pool that cannot be outrun is the whole reason for a pool.
    static constexpr std::size_t kSlots = 256;

    struct Pickup {
        Vector2 at{};
        Vector2 velocity{};

        Stack stack{};

        float bornAt = 0.0f;

        // Seconds after it was born before the player may draw it in.
        //
        // Per pickup rather than one constant, because the two ways of being
        // born want opposite answers: something a tree gave up should come to
        // hand almost at once, and something the player threw away must not,
        // or it is back in the bar before the hand has let go of it.
        float holdFor = 0.0f;

        bool settled = false;
        bool live    = false;
    };

    // The next free slot, or nothing at all when the pool is full.
    //
    // One full pass and no more. A pool this size only fills if something has
    // gone wrong upstream, and refusing the overflow is the right answer: the
    // alternative is taking a slot from a pickup the player can see.
    Pickup *Claim();

    std::array<Pickup, kSlots> pool_{};

    // Where the next search for a free slot begins. Round-robin rather than from
    // the start, so a burst of drops does not walk the whole pool per item.
    std::size_t next_ = 0;
};
