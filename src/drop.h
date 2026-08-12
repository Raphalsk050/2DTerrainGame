#pragma once

#include "item.h"
#include "raylib.h"
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
    // Throws `count` of an item out of a point, spread over a small arc away from
    // the side the blow came from.
    void Spawn(Item item, int count, Vector2 from, float away, float now);

    // Falls, settles, drifts to a nearby player and is collected. What was
    // gathered this step is added to `into`.
    void Update(const World &world, Vector2 player, float dt, float now, Harvest &into);

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

        Item item = Item::Wood;

        float bornAt  = 0.0f;
        bool settled  = false;
        bool live     = false;
    };

    std::array<Pickup, kSlots> pool_{};

    // Where the next search for a free slot begins. Round-robin rather than from
    // the start, so a burst of drops does not walk the whole pool per item.
    std::size_t next_ = 0;
};
