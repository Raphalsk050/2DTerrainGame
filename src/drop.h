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
//
// What it must never do is *lose* anything. A block the player dug and could not
// carry is a block they earned, and a pool that quietly drops it on the floor —
// by filling up, or by timing out — takes it back. So there is no expiry here,
// the overflow of a full pool is poured into a pickup that is already lying
// there rather than discarded, and pickups of one kind gather into a single
// stack as they settle. That last one is also what keeps a wood the player has
// worked through affordable: a pickup is thirty-six squares to draw, and a
// hundred of them lying in the grass is a frame's worth of them.
class Drops {
public:
    // Throws a stack out of a point in a few pieces, spread over a small arc
    // away from the side the blow came from.
    //
    // Several pickups rather than one, which is much of the reason the class
    // exists: the arc from the trunk to the ground is most of what makes felling
    // a tree feel like it happened, and a single object carrying six wood does
    // not read as six of anything.
    //
    // Bounded at kPieces, and the bound is what a brush asks for rather than
    // what a tree does. Every drop table in the wood is under ten, so a felling
    // still throws one piece per unit; a full slot of stone spilled out of a
    // full bag is sixty-four, and sixty-four separate objects is a quarter of
    // the pool spent on one stroke.
    void Scatter(Stack stack, Vector2 from, float away, float now);

    // Throws a whole stack out as one object, towards a point.
    //
    // The other half of the pair, and it has to be the other half. A player
    // emptying a full slot onto the ground would spend a quarter of the pool on
    // one gesture if this scattered, and would then have to walk over
    // sixty-four separate things to undo a misclick.
    void Toss(Stack stack, Vector2 from, Vector2 towards, float now);

    // Falls, settles, gathers with its own kind, drifts to a nearby player and
    // is collected into `into`.
    //
    // Whatever will not fit is left lying where it is. A full inventory is a
    // reason for something to stay on the ground, not a reason for it to stop
    // existing — and not a reason for it to follow the player about either: a
    // pickup only comes to hand once there is a hand with room in it.
    void Update(const World &world, Vector2 player, float dt, float now, Inventory &into);

    // Drawn with the plants, and so inside the light.
    //
    // Takes no clock: nothing here ages, and a pickup drawn any differently for
    // being old would be saying something about a timer that no longer exists.
    void Draw() const;

    // Every live pickup as the rest of the game sees it: the square it is drawn
    // as, the distance at which it starts coming to the player, and the distance
    // at which it is taken.
    //
    // The two circles are the point. A pickup is one position with two radii
    // around it and no box at all, so what it collides with is nothing and what it
    // *reacts* to is a pair of numbers that are invisible in play — the only
    // symptom of either being wrong is an item that will not come to hand, and no
    // amount of looking at the item shows why.
    void DrawCollision(Vector2 player) const;

    void Clear();

    int Live() const;

private:
    // Two hundred and fifty six is a felled oak's worth several times over. A
    // pool that cannot be outrun is the whole reason for a pool.
    static constexpr std::size_t kSlots = 256;

    // Most pieces one throw is broken into. See Scatter.
    static constexpr int kPieces = 8;

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
    // gone wrong upstream, and taking a slot from a pickup the player can see is
    // never the answer — what the callers do instead is pour the overflow into
    // something already lying there. See Spill.
    Pickup *Claim();

    // Pours as much of `stack` as will go into pickups of its own kind already
    // on the ground, nearest first, and leaves the rest in it.
    //
    // The other half of Claim. Between them the only state that can still lose
    // anything is a full pool holding two hundred and fifty six *full* stacks of
    // other things — sixteen thousand items lying inside one view, with nothing
    // of this kind among them. That is worth stating plainly rather than claiming
    // it cannot happen: it is unreachable in play, and it is the one case left.
    void Spill(Stack &stack, Vector2 near);

    // Gathers settled pickups of one kind into single stacks.
    //
    // The reason it is worth doing at all is the draw: a pickup is a six by six
    // picture, so it costs up to thirty-six squares, and a wood the player has
    // felled their way through leaves hundreds of them lying about. Merging is
    // also what a player expects — a pile of wood is a pile, not a carpet.
    //
    // Only settled ones, and only within kGather of each other, so a stack does
    // not swallow something still in the air or something across the clearing.
    void Merge();

    std::array<Pickup, kSlots> pool_{};

    // Where the next search for a free slot begins. Round-robin rather than from
    // the start, so a burst of drops does not walk the whole pool per item.
    std::size_t next_ = 0;
};
