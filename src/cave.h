#pragma once

#include "raylib.h"

#include <cstdint>
#include <vector>

// Cave systems, dug rather than thresholded.
//
// The rest of the world is a stack of noise fields and the caves used to be one
// too: a band around the zero set of a field. That construction is defensible on
// paper — the zero set of a field is a family of long curves, so a band around
// it is a family of corridors — and it does not survive contact with the screen.
// Every irregularity such a corridor can have must come out of the field; the
// field is smooth at the scale a corridor is long; and no amount of domain
// warping, width modulation or wall roughness turns a level set into something
// that reads as rock. What it reads as is a ribbon.
//
// This does the other thing, which is what Terraria's TileRunner and Minecraft's
// pre-1.18 carvers both do, and what the literature on 2D caves recommends when
// connectivity has to be guaranteed rather than hoped for: an *agent* walks
// through the rock and takes it out as it goes. A path has a beginning, a
// heading it turns away from a little at a time, branches, places where it opens
// into a room and a place where it ends. None of that has to be coaxed out of a
// field. It is written down, which is what makes it a thing that can be
// designed.
//
// Two properties are kept from the old construction, because the rest of the
// project rests on them:
//
//   - **Placement is a pure function of the cell index**, exactly as
//     `flora::Grow`'s is. The world is divided into cells; whether a cell holds a
//     system, where it starts and every number describing it hash out of that
//     cell index alone. A walk is hard-bounded to its own cell's reach, so only a
//     fixed neighbourhood of cells can affect any position and no chunk has to
//     know what its neighbours generated.
//   - **What comes out is a signed distance in pixels**, positive in rock,
//     negative inside a passage, zero on the wall. It is a truer one than the
//     band ever was: the distance to a swept circle is exact, where the band's
//     was a field value divided by its own slope.
//
// This module knows nothing about the surface. Where the ground is, and how deep
// a system may lie, are handed in — which is what keeps it independent of
// `terrain` and therefore free of the circular include that owning its own
// settings would need.
namespace cave {

// Shape of the systems, in world pixels and radians.
struct Settings {
    // Side of one placement cell. Wide and short, because a system is: matching
    // the cell to the shape of what it holds is what lets the neighbourhood a
    // query has to search stay three by three.
    float cellSpan = 1600.0f;
    float cellRise = 700.0f;

    // Share of cells that hold a system at all.
    float chance = 0.55f;

    // The trunk: how many steps it walks, and how far each goes.
    int steps        = 240;
    float stepLength = 6.0f;

    // How far the heading may turn per step, and how hard that turn is damped
    // back towards zero afterwards.
    //
    // The damping is what makes a path sweep rather than jitter. An undamped
    // random turn each step is a drunkard's walk, which doubles back on itself
    // and covers no ground; a damped one holds a curve for a while and then
    // changes its mind. Minecraft's carvers do exactly this.
    float wander  = 0.34f;
    float damping = 0.72f;

    // Share of a step's length that goes into the vertical.
    //
    // Below one the walk travels further sideways than up and down, which is the
    // whole of what makes a passage walkable in a side-scroller. It is the
    // geometric form of what `NoiseShape::aspect` was doing for the old cave
    // fields, and unlike that one it says what it means.
    float squash = 0.42f;

    // Half-width of the passage where a system begins, and what it approaches far
    // underground, over that depth.
    float radius        = 16.0f;
    float radiusAtDepth = 24.0f;
    float growthDepth   = 1800.0f;

    // Share of the way along a walk over which it tapers to nothing at each end.
    //
    // Without it a walk stops on a flat disc the width of the passage, which
    // reads as a wall somebody built. With it the passage narrows to a crack and
    // pinches out, which is what a cave does. Minecraft's carvers taper by a sine
    // over the whole length; a share at each end leaves the middle at full width,
    // which suits a passage meant to be walked.
    float taper = 0.16f;

    // How many branches leave a trunk, how long they are as a share of it, and
    // how far from the parent's heading they set off.
    int branches       = 3;
    float branchLength = 0.42f;
    float branchAngle  = 1.05f;
    float branchRadius = 0.78f;

    // Chance per step that the passage opens into a room, how many steps a room
    // lasts, and how many times the half-width it reaches in the middle.
    float roomChance = 0.05f;
    int roomSteps    = 7;
    float roomSwell  = 3.2f;

    // Share of a room's height that is filled in from the floor.
    //
    // A swept circle is round, and a round room reads as a bubble. Cutting the
    // bottom off one gives it somewhere to stand, which is the same thing the
    // rubble term did for the old chamber fields and the reason a real chamber
    // has a flat floor: whatever came off the ceiling is lying on it.
    float roomFloor = 0.3f;

    // Half-width of the corridor that joins one system to the next, as a share
    // of the trunk's, and how much of a step of one goes into the vertical.
    //
    // `linkSquash` is the figure itself and not a multiple of `squash` above,
    // and that distinction is the difference between a route down and no route
    // down at all: a link scaled by the trunk's own squash descends two pixels a
    // step and runs out of steps a long way short of the system below it, which
    // is exactly how an underground that is joined up on paper comes out sealed.
    //
    // Two systems in neighbouring cells each dig towards the point halfway
    // between their origins, so they meet there. Both sides work it out from the
    // same two cell indices, so neither has to know what the other did — and
    // because the meeting point is *decided* rather than hoped for, the
    // underground is connected by construction instead of by density.
    //
    // This is the thing a dug generator can do that a thresholded field cannot.
    // A field has no idea where the next cave is; a digger can be aimed at it.
    float linkRadius = 0.62f;
    float linkSquash = 1.0f;

    // How hard a link's heading is pulled back towards its target each step, in
    // [0,1]. Low enough that the corridor wanders on its way rather than running
    // straight at the mark.
    float linkAim = 0.34f;

    // Share of systems that send a shaft up to daylight, and the half-width of
    // one.
    //
    // Deliberately well under one. Most caves have no way in from above; it is
    // the ones that do that are worth finding, and an entrance every screen is a
    // landscape with its lid off rather than a find.
    float entranceChance = 0.3f;
    float entranceRadius = 22.0f;

    // How far a shaft may wander from vertical, and how many steps it gets to
    // reach daylight before it gives up and stays a chimney.
    float entranceWander = 0.16f;
    int entranceSteps    = 90;
};

// One node of a walk: where it is, and how wide the passage is there.
struct Node {
    Vector2 at;
    float radius;

    // Share of the radius filled in from the floor at this node, in [0,1]. Non
    // zero only inside a room.
    float floor;
};

// A built system.
//
// The nodes of every walk in it, end to end, with `breaks` marking where one
// walk ends and the next begins so that no segment is ever drawn between them.
struct System {
    std::vector<Node> nodes;
    std::vector<int> breaks;

    // Which nodes fall in each column of `binSpan` pixels, so that a query tests
    // the handful of segments near it rather than every one. A system is long and
    // thin and lies along x, so binning on x alone is most of what a quadtree
    // would buy for none of the cost.
    std::vector<std::vector<int>> bins;
    float binOrigin = 0.0f;
    float binSpan   = 96.0f;

    // What the walks actually covered, radius included. Queries outside it are
    // rejected before anything is measured.
    Rectangle bounds{};

    bool Empty() const { return nodes.size() < 2; }
};

// Whether a cell holds a system at all, and where its first step lands.
//
// Split out from Build so that a caller can work out how deep the origin is —
// which is what every size in the walk scales against — without building the
// walk to find out.
bool Origin(std::int64_t cellX, std::int64_t cellY, const Settings &s, int seed, Vector2 &outOrigin);

// The system a cell holds.
//
// `originDepth` is how far the origin lies below the ground, and sets the scale
// of everything. `ceilingY` is the shallowest world Y any walk may reach, which
// is what keeps a system under the crust; the entrance walk is the one exception
// and is given `surfaceY` to climb to.
//
// A pure function of the cell index and the seed, given the same three numbers.
// Where a neighbouring cell's system starts, for the corridors that join them.
// `has` false means that cell holds nothing and no corridor is dug towards it.
struct Neighbour {
    bool has = false;
    Vector2 origin{};
};

System Build(std::int64_t cellX, std::int64_t cellY, const Settings &s, int seed, Vector2 origin, float originDepth,
             float ceilingY, float surfaceY, const Neighbour neighbours[4]);

// Signed distance into the rock, in pixels: positive in rock, negative inside a
// passage. A very large positive number where the system reaches nothing.
float Carve(Vector2 world, const System &system);

// How far from its own origin a walk can possibly get, which is what makes the
// three-by-three neighbourhood a query searches correct rather than merely
// likely. Walks are hard-stopped at it.
float Reach(const Settings &s);

} // namespace cave
