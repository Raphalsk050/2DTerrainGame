#include "entity/nav/ahead.h"

#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {

// How far above its feet a body starts looking down for the floor.
//
// A little over what it can climb, so a ledge at the very top of its reach is still
// *found* — whether it is worth climbing is `plan.h`'s decision, and a scan that could
// not see the ledge would take that decision away by silently reporting a wall.
constexpr float kLookUp = 1.15f;

// And how far below, likewise a little past what it will drop.
constexpr float kLookDown = 1.15f;

// The top of the ground in one column, inside the band a body could reach.
//
// `World::FootingUnder` is the right question and not `Skyline` or `SurfaceProfile`:
// those two are read from the terrain function and are deliberately blind to digging
// and building, which is right for growing a wood and exactly wrong here. A creature
// has to walk over a bridge somebody built and refuse to walk into a hole somebody
// dug — see the head of `World::FootingUnder`.
bool FloorAt(const World &world, float x, float from, float reach, float &outTop) {
    return world.FootingUnder({x, from}, reach, outTop);
}

} // namespace

nav::Ahead nav::Read(const World &world, const body::Build &build, const Reach &reach, Vector2 at, float dir) {
    Ahead ahead;

    const float up   = reach.rise * kLookUp;
    const float down = reach.drop * kLookDown;

    const float band = up + down;

    if (!FloorAt(world, at.x, at.y - up, band, ahead.floor)) return ahead;

    ahead.footed = true;

    // How far to look before anything is found: the gap it could clear plus a couple
    // of strides, which is enough to see one coming and decide about it.
    const float horizon = std::min(reach.gap + kProbeStep * 3.0f, kProbeStep * kMostProbes);

    int probes = std::max(2, static_cast<int>(horizon / kProbeStep));

    // Where the ground was last found, so an obstacle is measured against the profile
    // rather than against where the body happens to be standing.
    float lastFloor = ahead.floor;

    bool inGap = false;

    for (int p = 1; p <= probes; p++) {
        const float out = static_cast<float>(p) * kProbeStep;
        const float x   = at.x + dir * out;

        float top = 0.0f;

        // Each column is read from the band around the *last* floor rather than around
        // the body's own, so a staircase is followed up or down instead of being read
        // as one enormous riser at the far end of it. That is the whole difference
        // between a creature that climbs a hillside and one that stands at the bottom
        // of it deciding the hill is a wall.
        const bool found = FloorAt(world, x, lastFloor - up, band, top);

        if (!found) {
            if (!inGap) {
                inGap         = true;
                ahead.toGap   = out - kProbeStep;
                ahead.gapSpan = 0.0f;
            }

            ahead.gapSpan = out - ahead.toGap;

            // Once a hole is found, look far enough past it to see the far side.
            //
            // The horizon above is measured from the *body*, so a hole eight strides
            // ahead used the whole of it getting there and had nothing left to see
            // over the hole with — the far side fell outside the scan, `gapEnds`
            // stayed false, and the planner correctly refused a jump into what it had
            // been told was bottomless. What the reading has to cover is the distance
            // to the near lip *plus* what the creature could clear from it.
            const float needed = ahead.toGap + reach.gap + kProbeStep * 2.0f;

            probes = std::min(kMostProbes, std::max(probes, static_cast<int>(needed / kProbeStep)));

            continue;
        }

        if (inGap) {
            ahead.gapEnds = true;
            ahead.gapFall = top - lastFloor;
            ahead.gapSpan = out - ahead.toGap;

            // Nothing past the far side of a hole matters until the hole is dealt
            // with, so this is where the reading stops.
            break;
        }

        const float rise = lastFloor - top; // Positive is up.

        // A rise it cannot simply walk up. The scan stops: what is behind a wall does
        // not matter, and a creature that saw past one would plan a route through it.
        if (rise > build.stepHeight) {
            ahead.toClimb = out - kProbeStep;
            ahead.climbUp = rise;

            break;
        }

        // A fall it cannot simply walk down. Recorded, and the scan **carries on**,
        // because a drop is not an obstacle — it is walked off — and what matters is
        // whether there is a hole just past the bottom of it. A scan that stopped here
        // would step a creature down onto a ledge over a chasm it never saw.
        if (-rise > build.stepHeight && ahead.toDrop < 0.0f) {
            ahead.toDrop   = out - kProbeStep;
            ahead.dropDown = -rise;
        }

        lastFloor = top;
    }

    return ahead;
}
