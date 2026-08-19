#pragma once

#include "entity/life/blow.h"
#include "entity/mob/mob.h"
#include "entity/mob/warren.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class World;
class Drops;

namespace mob {

// Every creature alive at once, and the one thing the game loop talks to.
//
// A fixed pool with no allocation, on the model of `Drops`. Where it parts company
// with that file is the one thing `Drops` says about itself most firmly: a pickup
// **must never be asked to survive being walked away from**, and a creature must.
//
// So the pool is the *live* half only. Everything else — which creatures a place
// holds, where they got to, and the fact that the ones you killed are gone — is the
// `Warren`, and the herd is what pages between the two. Read `patch.h` for the
// design; the short of it is that walking away no longer unmakes anything.
//
// **It is not an entity-component system and should not become one.** There is one
// kind of thing in this pool and it has a fixed shape. An ECS earns its keep when
// there are many kinds of entity with overlapping, unpredictable sets of parts;
// here there is a body, a brain and some health, every creature has all three, and
// a generalised store would buy nothing but indirection. If a creature ever needs a
// part the others do not, the honest first answer is a field on its row.
class Herd {
public:
    // Big enough for everything the waking box can hand over at once.
    //
    // The box is the simulated region plus a cell each way, which at full screen is
    // some seven patches by seven; at four creatures a patch that is under two
    // hundred, and this is over it. A pool that cannot be outrun is the whole reason
    // for a pool — and here the overflow is not lost either, since anything that
    // cannot be revived stays in the record and is woken when a slot frees.
    static constexpr std::size_t kSlots = 256;

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
    // What the console's `/spawn` uses. Anything put down this way is remembered
    // like any other once the view leaves it.
    bool Put(Kind kind, Vector2 at);

    // Brings one back exactly as it was left — where it got to, what it had left,
    // and what it was in the middle of doing.
    bool Revive(const Life &life);

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

    // Every creature inside a region, awake or asleep. See `Warren::Census`.
    void Census(Rectangle where, std::vector<Life> &out) const;

    // What the world remembers, for the display and for `--mobcheck`.
    const Warren &Memory() const { return warren_; }
    Warren &Memory() { return warren_; }

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
    const std::array<Mob, kSlots> &All() const { return pool_; }

private:
    Mob *Claim();

    std::array<Mob, kSlots> pool_{};

    // Where the next search for a free slot begins. Round-robin rather than from
    // the start, so a burst of spawns does not walk the whole pool per creature.
    std::size_t next_ = 0;

    // Where the creatures of this world actually live. The pool above holds only
    // the ones near enough to be moving.
    Warren warren_;

    // Scratch for the waking list, so a frame that wakes nothing allocates nothing.
    std::vector<Life> waking_;

    // The herd's own stream, for the small things that are nobody's to remember —
    // which way a carcass throws its drops, and the seed a hand-placed creature
    // starts with. **Not** what decides a population: that is the warren's, and it is
    // a function of the cell rather than a running stream, or a place would hold
    // different animals depending on what had been rolled before it.
    std::uint32_t seed_ = 0x5EED1234u;
};

} // namespace mob
