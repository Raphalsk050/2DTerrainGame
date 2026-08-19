#pragma once

#include "entity/life/blow.h"
#include "entity/mob/mob.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class World;
class Drops;

namespace mob {

// Every creature alive at once, and the one thing the game loop talks to.
//
// A fixed pool with no allocation, on the model of `Drops` — and for the same
// reason given at the head of that file: a creature does not generate, it is not a
// pure function of anywhere, and it must never be asked to survive being walked
// away from. What is different is that a creature is *worth* keeping while it is
// near, so the pool has a streaming rule where `Drops` has none.
//
// **It is not an entity-component system and should not become one.** There is one
// kind of thing in this pool and it has a fixed shape. An ECS earns its keep when
// there are many kinds of entity with overlapping, unpredictable sets of parts;
// here there is a body, a brain and some health, every creature has all three, and
// a generalised store would buy nothing but indirection. If a creature ever needs a
// part the others do not, the honest first answer is a field on its row.
class Herd {
public:
    // What the world did to the player this frame, gathered as one blow.
    //
    // Reported rather than applied, so this class never learns that a `Player`
    // exists. The loop is what joins the two, which is the same arrangement the
    // editor already has with the inventory.
    struct Toll {
        int damage = 0;

        // Where the worst of it came from, for the knock-back.
        Vector2 from{};

        float knock = 0.0f;
        float lift  = 0.0f;

        bool Any() const { return damage > 0; }
    };

    // Puts a creature in the world outright, and reports whether there was room.
    // What the console's `/spawn` uses, and what the spawner uses underneath.
    bool Put(Kind kind, Vector2 at);

    // A frame: spawning, thinking, moving, striking, dying, and going quiet once
    // the view has left.
    //
    // `active` is the simulated region rather than the drawn one — a creature just
    // off screen has to keep walking, or the world reorganises itself every time
    // the player turns round.
    Toll Update(World &world, Rectangle active, Rectangle body, Vector2 quarry, float now, float dt, Drops &drops);

    // Everything the player's swing touched. Returns how many were hit, so a caller
    // can tell a blow that connected from one that met air.
    int Strike(Rectangle hitbox, int damage, Vector2 from, float knock, float lift);

    void Draw(Rectangle view) const;

    // The colliders and the noticing distance, under the debug toggle. The second
    // is the point: a creature's reach is invisible in play, and the only symptom
    // of it being wrong is an animal that reacts too early or not at all.
    void DrawCollision() const;

    void Clear();

    int Live() const;
    int LiveOf(Kind kind) const;

    // Every live creature, for the probe and the overlay. A view of the pool and
    // not a copy of it.
    const std::array<Mob, 128> &All() const { return pool_; }

private:
    // A hundred and twenty eight is a long way past anything a view can hold at
    // the crowd figures the rows carry. A pool that cannot be outrun is the whole
    // reason for a pool.
    static constexpr std::size_t kSlots = 128;

    Mob *Claim();

    std::array<Mob, kSlots> pool_{};

    // Where the next search for a free slot begins. Round-robin rather than from
    // the start, so a burst of spawns does not walk the whole pool per creature.
    std::size_t next_ = 0;

    // The herd's own stream, for choosing where and what to spawn. Seeded from the
    // world so that two runs of the same seed are not obliged to be identical —
    // creatures are the one thing in this project that deliberately are not. See
    // `brains/whim.h`.
    std::uint32_t seed_ = 0x5EED1234u;

    // Seconds until the spawner is asked again. Creatures do not arrive every
    // frame; asking every frame would cost the world lookups of a full attempt
    // sixty times a second to place, at most, one animal.
    float tryIn_ = 0.0f;
};

} // namespace mob
