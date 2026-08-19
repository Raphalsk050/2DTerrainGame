#pragma once

#include "entity/body/build.h"
#include "entity/body/intent.h"
#include "raylib.h"

class World;

// A thing with a size and a speed, moving against the ground.
//
// Everything in this world that walks does it through here: it falls, it swims,
// it is carried over a ledge rather than stopped by one, it is nudged past a
// corner it caught its head on, and it is lifted out of rock that closed over
// it. Each of those is a decision with a reason, most of them were found by
// watching the character snag on something, and none of them is worth
// discovering twice.
//
// It was the private half of `Player`, which is where it would have stayed if
// nothing else ever had to walk. What made it a class is that a second walking
// thing is the one change that cannot be made without it — a mob written beside
// the player rather than on top of it is a second copy of the four helpers
// below, and a copy is a place for the two to disagree about what a hillside is.
//
// What is deliberately *not* here: health, damage, aim, attack, art, and any
// notion of who is driving. A body is a shape with momentum. Everything that
// makes it a player or a pig is composed on top.
namespace body {

class Body {
public:
    // Default-constructible so that a pool can hold a fixed array of them. The
    // default `Build` is the character's own, which is a sane body rather than a
    // degenerate one — a pool slot that has never been woken is not drawn and not
    // stepped, and a zero-sized collider would be a much worse thing to find there
    // if that ever stopped being true.
    Body() = default;

    Body(const Build &build, Vector2 at) : build_(build), position_(at) {}

    // A whole frame of motion: the stance, the speed, the jump and the collision
    // that resolves it.
    //
    // The order is load-bearing and it is the character's own. Submersion is read
    // before anything moves, so buoyancy, drag and the choice between walking and
    // swimming all act on the same reading; the jump is applied after the speed
    // and before the move, so a press is spent on the frame it is honoured.
    void Step(const Intent &intent, const World &world, float dt);

    // The box the world is asked about, at the current position and stance.
    Rectangle Bounds() const { return RectAt(position_, crouched_); }

    // The box this body would occupy somewhere else, for testing a move before
    // committing to it.
    Rectangle RectAt(Vector2 at, bool crouched) const;

    Vector2 Centre() const;

    Vector2 Position() const { return position_; }
    Vector2 Velocity() const { return velocity_; }

    // Puts the body somewhere outright, clearing whatever it was doing on the
    // way. Momentum goes with it: arriving somewhere new still carrying the speed
    // of the fall that was interrupted is how a teleport ends underground.
    void PlaceAt(Vector2 at) {
        position_ = at;
        velocity_ = {};
    }

    // Sets the speed directly, for the one thing that is not a wish — a blow
    // landing, which is something done *to* a body rather than by it.
    void Shove(Vector2 velocity) { velocity_ = velocity; }

    bool Grounded() const { return grounded_; }
    bool Crouched() const { return crouched_; }

    // Share of the body under liquid, in [0,1], and the test the walk gives way
    // to a stroke at.
    float Submerged() const { return submerged_; }
    bool Swimming() const { return submerged_ >= build_.swimThreshold; }

    // No gravity, no collision, no liquid: for looking at the world rather than
    // living in it. Deliberately suspends every rule that would keep the body out
    // of the rock, which is what makes it useful and what makes it a debug
    // control rather than a way of playing.
    //
    // Distinct from Build::floats, which is a creature that flies and still
    // collides with the mountain.
    bool Ghost() const { return ghost_; }
    void SetGhost(bool on) {
        ghost_ = on;

        // Dropped rather than carried across, in both directions: entering with a
        // fall already in progress would sink the body, and leaving at boost speed
        // would fling it across the world.
        velocity_ = {};
    }

    const Build &Made() const { return build_; }

    // Whether the body could stand up where it is. Public because standing up is
    // a decision an owner makes — the player releases the key, a brain stops
    // hiding — and the ceiling is the only thing that can refuse it.
    bool CanStandUp(const World &world) const;

private:
    void ReadStance(const Intent &intent, const World &world);
    void ReadSpeed(const Intent &intent, float dt);
    void TryJump(const Intent &intent);
    void Drift(const Intent &intent, float dt);
    void Soar(const Intent &intent, float dt);

    // Movement resolves one axis at a time, horizontal then vertical. Moving both
    // at once cannot tell which axis caused an overlap, and the body catches on
    // flat ground it should slide along.
    //
    // The move is split into sub-steps no longer than half a grid cell so that a
    // fast fall cannot pass through thin ground between two frames.
    void MoveAndCollide(const World &world, float dt);

    // Lifts the body over a small ledge that blocked it sideways, and reports
    // whether it found a height that cleared. The four helpers here are what
    // separate walking over ground from being stopped by it; each one answers a
    // different way the lattice interrupts a move that should have carried on.
    bool StepOver(const World &world);

    // Pulls the body back down onto the ground it just walked off.
    void SnapToGround(const World &world);

    // Shifts the body sideways past a corner that blocked it going up.
    bool Sidestep(const World &world);

    // Carries a body already inside the ground to the nearest place it is not.
    //
    // The three above are about a move that was interrupted; this one is about a
    // body with nowhere to move from. Collision alone cannot answer it — every
    // direction out of solid ground starts in solid ground, so every move is
    // refused and the body is held where it stands for good.
    bool Unstick(const World &world);

    Build build_{};

    // Anchored at the feet: bottom edge, horizontally centred. Ground contact and
    // crouching both act on the bottom, so the anchor stays put when the height
    // changes.
    Vector2 position_{};
    Vector2 velocity_{};

    bool grounded_ = false;
    bool crouched_ = false;
    bool ghost_    = false;

    float submerged_ = 0.0f;

    float coyoteTimer_     = 0.0f;
    float jumpBufferTimer_ = 0.0f;
};

} // namespace body
