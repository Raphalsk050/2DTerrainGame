#pragma once

#include "entity/body/build.h"
#include "entity/body/intent.h"
#include "entity/nav/ahead.h"
#include "raylib.h"

class World;

// What to do about what is ahead.
//
// The judgement half of the navigator: `ahead.h` reads the ground and has no opinion
// about it, and this decides whether a 30 px riser is a step, a jump or a wall. The
// split is what lets one scan answer for creatures with different legs.
//
// It returns a wish rather than moving anything — see `body/intent.h`. Everything
// here comes out as `moveX`, a jump press and a jump hold, which is exactly what a
// hand at a keyboard would produce, and the body decides what it can do about them.
namespace nav {

struct Step {
    float moveX = 0.0f;

    // Pressed this frame, and held for the height of the arc. Two fields because they
    // are two decisions: a press starts a jump and a hold decides how big it is, and a
    // creature that only ever pressed would make the smallest hop it can.
    bool jump = false;
    bool hold = false;

    // This way is no good. What to do about it is the brain's — a wandering animal
    // turns round, a frightened one takes the other way out.
    bool turn = false;

    // What it decided, for `--nav` and the debug overlay. A behaviour is the one thing
    // here that cannot be checked by comparing two pictures.
    enum class Doing {
        Walk,   // nothing in the way
        RunUp,  // a ledge ahead, building up the speed to take it
        Climb,  // leaving the ground for it
        Leap,   // clearing a hole
        Down,   // walking off a drop, deliberately without jumping
        Held,   // mid-arc, not re-deciding
        Back,   // too close to a ledge to jump it, making room
        Blind,  // standing on nothing the scan can find

        // Refused, and by which of the three. One enumerator each rather than a
        // single `Stop`, because "it would not go" is the least useful thing a trace
        // can say — every one of the three has a different cause and a different fix,
        // and telling them apart is the difference between a diagnosis and a shrug.
        NoGap,   // a hole with nowhere to land
        NoClimb, // a ledge too high, or one it has already failed at
        NoDrop,  // a fall it will not take
    } doing = Doing::Walk;
};

// Everything the navigator needs to remember between frames, held by the creature.
//
// Four things, and each is there because a plan made afresh every frame gets one
// particular case wrong.
//
// **The leap.** Re-planning mid-arc unmakes a jump halfway through it: the moment the
// body leaves the lip, the gap it was clearing is behind it, the scan reports open
// ground, the wish to hold the jump goes away — and the shortened arc drops the
// creature into the hole.
//
// **The back-off.** A body that has walked into the face of a ledge has no room left
// to build the speed a climb needs, and no amount of re-deciding creates any. It has
// to reverse first, and that has to survive the frames in which the ledge is still
// right there saying "climb me".
//
// **The tally.** Three failed attempts at the same ledge is a ledge this creature is
// not going to climb, whatever the arithmetic says — the ground is a contour, not a
// stair, and there are shapes the numbers do not describe. Without it a creature can
// jump at the same spot for ever, which is exactly what it did.
//
// **The last reading**, which is the one thing here that exists for speed. Reading the
// ground is up to thirty-two `World::FootingUnder` calls; a body that has moved two
// pixels would get the same answer. The reading is kept and its distances shifted by
// how far the creature has travelled, which leaves the timing exact while a strolling
// animal scans once every several frames. The *decision* is deliberately not cached —
// a cached decision would repeat a jump press every frame until it expired.
struct Legs {
    float leaping = 0.0f;
    float backing = 0.0f;

    float triedAt    = 0.0f;
    float triedFloor = 0.0f;
    int tries        = 0;

    Ahead ahead{};

    Vector2 scannedAt{};
    float scannedDir = 0.0f;
    bool scanned     = false;
};

// The next stride towards `dir` (-1 or +1).
//
// `velocity` is the body's own, and it is not a convenience: how fast it is **going**
// is what decides whether a jump will work, and the intended pace is not that. A
// creature that has just set off, or has been stopped by the very wall it is trying to
// climb, is going nowhere at all — and timing a jump against what it *meant* to be
// doing is how it ends up jumping on the spot for ever.
//
// `throttle` is how much of its speed it intends to use, in [0,1] — an amble is under
// a half — and `hurrying` whether that is against its sprint or its walk.
//
// `committed` is whether it has a reason to take a drop it could not climb back out
// of. See `nav::Of` in reach.h.
Step Plan(const World &world, const body::Build &build, Vector2 at, Vector2 velocity, bool grounded, float dir,
          float throttle, bool hurrying, bool committed, Legs &legs, float dt);

// The stride as a wish the body will take directly.
body::Intent Advance(const World &world, const body::Build &build, Vector2 at, Vector2 velocity, bool grounded,
                     float dir, float throttle, bool hurrying, bool committed, Legs &legs, float dt,
                     bool &outTurn);

} // namespace nav
