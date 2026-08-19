#pragma once

#include "entity/body/build.h"
#include "entity/nav/reach.h"
#include "raylib.h"

class World;

// What the ground does in front of a body, for the next few strides.
//
// A *reading*, with no opinion in it. Whether a 30 px riser is a step, a jump or a
// wall depends on what is trying to climb it, and that judgement lives in `plan.h`;
// this file only says the riser is 30 px. Keeping the two apart is what lets the same
// scan answer for creatures with different legs.
//
// It is a local scan and not a search. There is no graph here, no A*, no navmesh —
// the ground in this world is a contour over a lattice rather than a grid of tiles,
// so there are no nodes to search, and building some would mean a second description
// of the terrain to keep in step with the first. What a creature needs at this scale
// is to see one obstacle coming and know whether its own legs clear it, which is a
// handful of column probes.
//
// **Three kinds of thing are told apart, and that separation is the whole file.**
// A rise it cannot walk up, a fall it cannot walk down, and ground that is not there
// at all are three different problems with three different answers — and running them
// together is what made a creature jump when it meant to walk down a step.
namespace nav {

// How far apart the columns are probed, in world pixels.
//
// The lattice step. Finer buys nothing — the ground is interpolated between vertices,
// so there is no detail below it — and coarser steps over the lip of a hole narrow
// enough to fall into.
inline constexpr float kProbeStep = 6.0f;

// Most columns one scan reads.
inline constexpr int kMostProbes = 32;

struct Ahead {
    // Whether the body is standing on anything the scan could find. A scan from a
    // body in mid-air has no floor to measure the rest against, and everything below
    // is left at nothing.
    bool footed = false;

    // The ground under the body, in world Y.
    float floor = 0.0f;

    // A rise too high to be walked up, and how high it is. -1 where there is none
    // inside the horizon.
    float toClimb = -1.0f;
    float climbUp = 0.0f;

    // A fall too deep to be walked down without leaving the ground, and how deep.
    //
    // Told apart from a gap because the answer is opposite: a creature walks off a
    // drop and must never jump off one. A jump at a descent throws it further out
    // than it meant to go and lands it harder, and on a staircase it reads as an
    // animal bouncing downhill.
    float toDrop   = -1.0f;
    float dropDown = 0.0f;

    // Ground that is not there at all: no floor within reach of the body, at any
    // depth it would take.
    float toGap   = -1.0f;
    float gapSpan = 0.0f;

    // Whether the far side was found inside the horizon, and how far below the near
    // lip it sits. Positive is down, which is the easy direction — a jump that lands
    // lower has the whole fall to play with.
    bool gapEnds  = false;
    float gapFall = 0.0f;
};

// Reads the ground in `dir` (-1 or +1) from a body standing at `at`.
//
// `at` is the body's own anchor — its feet — which is what `body::Body::Position`
// holds, so the caller never has to work out where the bottom of the box is.
Ahead Read(const World &world, const body::Build &build, const Reach &reach, Vector2 at, float dir);

} // namespace nav
