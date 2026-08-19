#pragma once

#include "entity/body/build.h"

#include <cmath>
#include <limits>

// What a body can actually get to, worked out from what it is made of.
//
// Every one of these is *derived* and none of them is tuned, and that is the whole
// point of the file: a creature's jump is already three numbers on its row —
// `jumpSpeed`, `gravity`, `sprintSpeed` — and how high a ledge it can climb is not a
// fourth fact to be written down beside them and kept in step. It is arithmetic on
// the three.
//
// The arithmetic is checkable against figures this project already had. For the
// character: apex is 480² / (2 x 1600) = 72 px, which is the jump height
// `player_config` has always claimed; the arc lasts 2 x 480 / 1600 = 0.6 s, which is
// what `sprintSpeed`'s comment says; and a sprint carries it 380 x 0.6 = 228 px,
// which is the figure that comment gives for clearing a hole. Nothing here was
// invented — it is the same numbers read forwards.
namespace nav {

struct Reach {
    // The highest ledge it can land *on top of*, in world pixels.
    float rise = 0.0f;

    // The widest hole it can clear, at whatever pace it is going.
    float gap = 0.0f;

    // The deepest step down it will willingly take.
    float drop = 0.0f;

    // How long its jump keeps it in the air, in seconds. What a committed leap is
    // held for, so a re-plan mid-arc cannot cancel it.
    float airtime = 0.0f;

    // And how long until the top of the arc.
    //
    // The one figure the timing of a climb rests on: a body that arrives at a wall at
    // the top of its arc has the most height to spare and the most room either side
    // of the moment to be wrong in. See `plan.cpp`.
    float apexIn = 0.0f;
};

// How much of the apex is usable.
//
// A body has to land *on* the ledge, not merely touch its lip at the top of the arc
// with no speed left. Four fifths leaves room for the landing and for the fact that
// the ground is a contour rather than a step, so its true height is not a multiple of
// anything.
inline constexpr float kApexUsable = 0.8f;

// And how much of the arc's span. Less generous, because the two ends of a gap are
// found by probing on the lattice and the far lip can be up to a step further away
// than it was measured at.
inline constexpr float kSpanUsable = 0.72f;

// How much further a creature with a reason will drop than one merely wandering.
//
// A wandering animal must never step off anything it could not climb back, or the
// world slowly fills its ravines with creatures that are alive and stuck — which is
// worse than either dying or staying up top, because nothing on screen says what
// happened. One that is fleeing or hunting has a reason to take the risk, and being
// stuck later beats being caught now.
inline constexpr float kDesperate = 3.0f;

// The two moments a jump is above a given height, in seconds from leaving the ground.
//
// This is the window a climb has to happen inside, and having both ends of it is what
// separates a navigator that knows when to jump from one that jumps hopefully. Below
// `first` the body has not risen far enough; past `last` it has come back down. A body
// that arrives at a wall outside that window does not climb it — it hits the face,
// loses its horizontal speed to the collision, and slides back down where it started.
//
// Returns false where the jump does not reach the height at all.
inline bool Above(const body::Build &build, float height, float &first, float &last) {
    const float v = build.jumpSpeed;
    const float g = build.gravity;

    if (g <= 0.0f) return false;

    const float under = v * v - 2.0f * g * height;

    if (under <= 0.0f) return false;

    const float root = std::sqrt(under);

    first = (v - root) / g;
    last  = (v + root) / g;

    return true;
}

// `pace` is how fast it will actually be travelling, in world pixels per second —
// not its top speed. A creature ambling at half its walk clears half the hole, and a
// navigator handed the row's figure instead would decide every one of them was
// jumpable and then fall short of all of them.
//
// `committed` is whether it has a reason to take a drop it could not climb back out
// of. See `kDesperate`.
inline Reach Of(const body::Build &build, float pace, bool committed) {
    Reach reach;

    // Nothing gravity does not act on has any of this to work out. A floating
    // creature goes where it points; there is no arc and no ledge.
    if (build.floats || build.gravity <= 0.0f) {
        reach.rise    = build.height * 4.0f;
        reach.gap     = build.width * 8.0f;
        reach.drop    = build.height * 4.0f;
        reach.airtime = 0.0f;
        reach.apexIn  = 0.0f;

        return reach;
    }

    const float apex = (build.jumpSpeed * build.jumpSpeed) / (2.0f * build.gravity);

    reach.apexIn  = build.jumpSpeed / build.gravity;
    reach.airtime = 2.0f * reach.apexIn;

    reach.rise = apex * kApexUsable;
    reach.gap  = pace * reach.airtime * kSpanUsable;

    // Equal to the climb by default, which is the rule that keeps an animal out of
    // holes it cannot get out of again.
    reach.drop = committed ? reach.rise * kDesperate : reach.rise;

    return reach;
}

// The slowest a body can be going and still climb a ledge of this height.
//
// Not "can it jump that high" — that is `Reach::rise` — but "will it still be up
// there when it arrives, and will it get far enough past the face to land on top".
// The window at height `h` lasts `last - first` seconds, and in that time the body
// covers `pace x window`; it needs that to be at least its own half-width plus a
// little, or it comes down with its feet against the face rather than on the ledge.
//
// **This is what a slow creature gets wrong.** A boar ambling at a third of its run
// covers fourteen pixels in its whole arc; it cannot climb a terrace at that pace
// however well the jump is timed, and a navigator that did not know it would jump,
// fall short, jump again, and go on doing that for ever. See CLAUDE.md §22.7.
inline float PaceToClimb(const body::Build &build, float height, float clearance) {
    float first = 0.0f;
    float last  = 0.0f;

    if (!Above(build, height, first, last)) return std::numeric_limits<float>::infinity();

    const float window = last - first;

    if (window <= 1e-4f) return std::numeric_limits<float>::infinity();

    return clearance / window;
}

} // namespace nav
