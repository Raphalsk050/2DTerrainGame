#include "cave.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdio>

namespace cave {
namespace {

// A distance no position is ever that far from anything, so a system that
// reaches nowhere near a query can say so without every caller having to ask
// whether it is empty.
constexpr float kFar = 1.0e9f;

// A walk stops rather than leaves its own cell's reach.
//
// The bound is not a tuning knob and must not be softened into one. It is what
// makes the neighbourhood a query searches *correct*: a position asks the nine
// cells around it and nothing else, and that is only right if no system can
// reach further than one cell away.
float Bound(const Settings &s) { return std::max(s.cellSpan, s.cellRise * 1.5f); }

// Deterministic bits from a cell index and a salt.
//
// The whole of the placement rests on this: a system's existence, origin and
// every number in it come from here, so the same cell yields the same system
// wherever and whenever it is asked about, and no chunk has to agree with its
// neighbours about anything.
std::uint32_t Hash(std::int64_t x, std::int64_t y, std::uint32_t salt, int seed) {
    auto bits = static_cast<std::uint64_t>(x) * 0x9e3779b97f4a7c15ull;

    bits ^= static_cast<std::uint64_t>(y) * 0xc2b2ae3d27d4eb4full;
    bits ^= static_cast<std::uint64_t>(salt) * 0x165667b19e3779f9ull;
    bits ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)) * 0x27d4eb2f165667c5ull;

    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdull;
    bits ^= bits >> 33;
    bits *= 0xc4ceb9fe1a85ec53ull;
    bits ^= bits >> 33;

    return static_cast<std::uint32_t>(bits);
}

// A stream of numbers from one seed, for the walk itself.
//
// A stream and not more hashing, because a walk is a sequence: each step needs a
// fresh number and the number of steps is not known in advance. Deterministic
// all the same, since the stream's seed came out of the cell.
struct Stream {
    std::uint32_t state;

    float Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;

        return static_cast<float>(state & 0xffffffu) / 16777216.0f;
    }

    float Between(float low, float high) { return low + (high - low) * Next(); }
};

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Distance from a point to a segment whose radius runs from one end to the
// other.
//
// The exact shape is a round cone, whose distance has a longer closed form; this
// is the standard approximation — nearest point on the axis, radius interpolated
// there — and it is exact wherever the two radii are equal, which along a walk
// they nearly are. Where they are not the wall is a few pixels off being a true
// tangent, which is smaller than the texel it is drawn on.
float Capsule(Vector2 p, Vector2 a, Vector2 b, float ra, float rb) {
    const Vector2 ab = {b.x - a.x, b.y - a.y};
    const Vector2 ap = {p.x - a.x, p.y - a.y};

    const float len = ab.x * ab.x + ab.y * ab.y;
    const float t   = (len > 1e-6f) ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len, 0.0f, 1.0f) : 0.0f;

    const float dx = ap.x - ab.x * t;
    const float dy = ap.y - ab.y * t;

    return std::sqrt(dx * dx + dy * dy) - Lerp(ra, rb, t);
}

} // namespace

float Reach(const Settings &s) { return Bound(s); }

bool Origin(std::int64_t cellX, std::int64_t cellY, const Settings &s, int seed, Vector2 &outOrigin) {
    const float roll = static_cast<float>(Hash(cellX, cellY, 1u, seed) & 0xffffffu) / 16777216.0f;
    if (roll >= std::clamp(s.chance, 0.0f, 1.0f)) return false;

    // Placed anywhere in its own cell rather than at the middle of it, or every
    // system in the world would sit on the same lattice and the underground would
    // read as a grid however irregular each system is.
    const float jx = static_cast<float>(Hash(cellX, cellY, 2u, seed) & 0xffffffu) / 16777216.0f;
    const float jy = static_cast<float>(Hash(cellX, cellY, 3u, seed) & 0xffffffu) / 16777216.0f;

    outOrigin = {(static_cast<float>(cellX) + jx) * s.cellSpan, (static_cast<float>(cellY) + jy) * s.cellRise};

    return true;
}

System Build(std::int64_t cellX, std::int64_t cellY, const Settings &s, int seed, Vector2 origin, float originDepth,
             float ceilingY, float surfaceY, const Neighbour neighbours[4]) {
    System system;

    Stream rng{Hash(cellX, cellY, 4u, seed) | 1u};

    // Everything scales off how deep the system starts. A passage well down is
    // wider than one near the crust, which is the whole of what makes descending
    // worth doing.
    const float grown =
        Lerp(s.radius, s.radiusAtDepth, std::clamp(originDepth / std::max(s.growthDepth, 1.0f), 0.0f, 1.0f));

    const float bound = Bound(s);

    float minX = origin.x;
    float maxX = origin.x;
    float minY = origin.y;
    float maxY = origin.y;
    float widest = grown;

    // A walk waiting to be dug: everything needed to start one.
    struct Pending {
        Vector2 at;
        float heading;
        int steps;
        float radius;
        bool rooms;

        // Where the walk is trying to get to, and how hard it is pulled there.
        // An aim of zero is a walk that wanders wherever it likes, which is every
        // trunk and every branch; the corridors between systems are the only
        // walks that have somewhere to be.
        Vector2 target{};
        float aim    = 0.0f;
        float squash = 1.0f;
    };

    std::vector<Pending> queue;

    // One walk, dug whole.
    //
    // Into its own list and appended at the end, rather than straight into the
    // system. A branch leaves the middle of a trunk, so digging it inline would
    // put its nodes between two consecutive nodes of the trunk — and the segments
    // are built from consecutive nodes, which would draw a passage from the end
    // of the branch back to wherever the trunk had got to.
    // **By value, and it has to be.** A branch is queued from inside this, and
    // `queue` is a vector: a push_back that grows it moves everything in it, and a
    // reference handed in from `queue[i]` is left pointing at freed memory. The walk
    // then reads its own step count out of whatever the allocator has since put
    // there — measured at 1065353216, which is the bit pattern of the float 1.0f
    // lying in the recycled block — and asks for a path of a billion nodes.
    //
    // It shows as either of two things and neither points here: the reserve throws
    // `std::length_error` and the game dies, or the reserve succeeds and a walk that
    // should take two hundred steps takes four hundred million, which is a window
    // that stops answering. Which of the two happens depends on the allocator, so it
    // is a bug that comes and goes.
    //
    // A Pending is forty bytes. Copying it is cheaper than one node of the path it
    // is about to dig.
    const auto dig = [&](Pending start) {
        std::vector<Node> path;
        path.reserve(static_cast<std::size_t>(start.steps));

        Vector2 at    = start.at;
        float heading = start.heading;
        float turn    = 0.0f;

        int roomLeft = 0;
        float swell  = 1.0f;

        // Where along this walk a branch leaves it, decided up front so the
        // branches are spread along the trunk rather than clustered wherever the
        // stream happened to run high.
        int branchAt[8]{};
        const int branches = start.rooms ? std::clamp(s.branches, 0, 8) : 0;

        for (int b = 0; b < branches; b++) {
            branchAt[b] = static_cast<int>(rng.Between(0.15f, 0.85f) * static_cast<float>(start.steps));
        }

        for (int i = 0; i < start.steps; i++) {
            // A damped random turn: the heading holds a curve and then changes
            // its mind. See Settings::damping.
            turn += (rng.Next() - 0.5f) * s.wander;
            turn *= s.damping;

            heading += turn;

            // A walk with somewhere to be is turned back towards it, by the
            // shorter way round. Gently: the corridor still wanders, it just
            // arrives.
            if (start.aim > 0.0f) {
                const float want = std::atan2(start.target.y - at.y, start.target.x - at.x);

                float off = want - heading;

                while (off > 3.14159265f) off -= 6.28318531f;
                while (off < -3.14159265f) off += 6.28318531f;

                heading += off * start.aim;

                if (std::fabs(start.target.x - at.x) + std::fabs(start.target.y - at.y) < s.stepLength * 2.0f) break;
            }

            at.x += std::cos(heading) * s.stepLength;
            at.y += std::sin(heading) * s.stepLength * start.squash;

            // Held under the crust. Reflected rather than clamped, so a walk that
            // meets the ceiling turns away from it and carries on instead of
            // running along it and leaving a passage with a dead flat roof.
            if (at.y < ceilingY) {
                at.y    = ceilingY + (ceilingY - at.y);
                heading = -heading;
                turn    = 0.0f;
            }

            // And inside its own cell, which is what makes the three-by-three
            // neighbourhood a query searches correct rather than merely likely.
            // A link is allowed the extra reach it needs to meet its target, since
            // the target is at most half a cell away and the walk has to be able
            // to arrive; everything else stops at its own cell.
            const float reachX = (start.aim > 0.0f) ? s.cellSpan : s.cellSpan;
            const float reachY = (start.aim > 0.0f) ? s.cellRise * 1.5f : s.cellRise;

            if (std::fabs(at.x - origin.x) > reachX || std::fabs(at.y - origin.y) > reachY) break;

            if (start.rooms) {
                if (roomLeft == 0 && rng.Next() < s.roomChance) roomLeft = std::max(s.roomSteps, 1);

                const float target = (roomLeft > 0) ? s.roomSwell : 1.0f;

                if (roomLeft > 0) roomLeft--;

                swell += (target - swell) * 0.35f;
            }

            // Tapered at both ends, so the passage pinches out.
            const float t    = static_cast<float>(i) / static_cast<float>(std::max(start.steps - 1, 1));
            const float ends = std::clamp(std::min(t, 1.0f - t) / std::max(s.taper, 1e-3f), 0.0f, 1.0f);

            const float r = std::max(start.radius * swell * ends, 1.0f);

            path.push_back({at, r, (swell > 1.4f) ? std::clamp(s.roomFloor, 0.0f, 1.0f) : 0.0f});

            for (int b = 0; b < branches; b++) {
                if (branchAt[b] != i) continue;

                const float side = (rng.Next() < 0.5f) ? -1.0f : 1.0f;

                queue.push_back({at, heading + side * s.branchAngle,
                                 static_cast<int>(static_cast<float>(start.steps) * s.branchLength),
                                 start.radius * s.branchRadius, false, {}, 0.0f, s.squash});
            }
        }

        if (path.size() < 2) return;

        system.breaks.push_back(static_cast<int>(system.nodes.size()));

        for (const Node &node : path) {
            system.nodes.push_back(node);

            minX   = std::min(minX, node.at.x - node.radius);
            maxX   = std::max(maxX, node.at.x + node.radius);
            minY   = std::min(minY, node.at.y - node.radius);
            maxY   = std::max(maxY, node.at.y + node.radius);
            widest = std::max(widest, node.radius);
        }
    };

    // The trunk, set off roughly sideways: a system is something to walk along.
    const float aim = (rng.Next() < 0.5f) ? 0.0f : 3.14159265f;

    queue.push_back({origin, aim + rng.Between(-0.45f, 0.45f), std::max(s.steps, 2), grown, true, {}, 0.0f,
                     s.squash});

    // And a corridor towards each neighbour that has something to meet.
    //
    // To the point halfway between the two origins rather than to the neighbour
    // itself, so that each side digs half of one corridor and the two halves join
    // in the middle. Both sides compute the same midpoint out of the same two
    // cell indices, so neither has to have seen the other's system.
    for (int n = 0; n < 4; n++) {
        if (!neighbours[n].has) continue;

        const Vector2 meet = {(origin.x + neighbours[n].origin.x) * 0.5f,
                              (origin.y + neighbours[n].origin.y) * 0.5f};

        const float heading = std::atan2(meet.y - origin.y, meet.x - origin.x);

        // Long enough to get there even wandering, since a corridor that runs out
        // of steps short of the mark is a dead end where a route was promised.
        const float far   = std::fabs(meet.x - origin.x) + std::fabs(meet.y - origin.y);
        const int steps   = static_cast<int>(far / std::max(s.stepLength, 1.0f) * 2.2f) + 12;


        queue.push_back({origin, heading, steps, grown * s.linkRadius, false, meet, s.linkAim, s.linkSquash});
    }

    for (std::size_t i = 0; i < queue.size(); i++) dig(queue[i]);

    // And the way in, from the shallowest point the system reached.
    if (rng.Next() < std::clamp(s.entranceChance, 0.0f, 1.0f) && !system.nodes.empty()) {
        const Node *highest = &system.nodes.front();

        for (const Node &node : system.nodes) {
            if (node.at.y < highest->at.y) highest = &node;
        }

        // Only if daylight is close enough to be reached in the steps it has. A
        // shaft that stops in the rock is an entrance to nothing.
        if (highest->at.y - surfaceY < static_cast<float>(s.entranceSteps) * s.stepLength * 0.9f) {
            Vector2 at    = highest->at;
            float heading = -1.5707963f;
            float turn    = 0.0f;

            system.breaks.push_back(static_cast<int>(system.nodes.size()));

            for (int i = 0; i < s.entranceSteps; i++) {
                turn += (rng.Next() - 0.5f) * s.entranceWander;
                turn *= s.damping;

                heading += turn;

                at.x += std::cos(heading) * s.stepLength;
                at.y += std::sin(heading) * s.stepLength;

                // Full width until it is out, then a couple of steps past the
                // surface so the mouth is a hole in the ground and not a dome
                // stopping just under it.
                const float r = s.entranceRadius;

                system.nodes.push_back({at, r, 0.0f});

                minX = std::min(minX, at.x - r);
                maxX = std::max(maxX, at.x + r);
                minY = std::min(minY, at.y - r);
                maxY = std::max(maxY, at.y + r);
                widest = std::max(widest, r);

                if (at.y < surfaceY - r) break;
            }
        }
    }

    if (system.nodes.size() < 2) {
        system.nodes.clear();
        system.breaks.clear();

        return system;
    }

    system.breaks.push_back(static_cast<int>(system.nodes.size()));
    system.bounds = {minX, minY, maxX - minX, maxY - minY};

    // The bins. A segment is filed under every column it or its radius touches,
    // so a query looks at one column and finds everything that could reach it.
    system.binOrigin = minX;

    const int columns = std::max(static_cast<int>((maxX - minX) / system.binSpan) + 1, 1);

    system.bins.assign(static_cast<std::size_t>(columns), {});

    std::size_t next = 1;

    for (std::size_t i = 0; i + 1 < system.nodes.size(); i++) {
        // Never across the join between two walks.
        if (next < system.breaks.size() && static_cast<int>(i + 1) == system.breaks[next]) {
            next++;
            continue;
        }

        const Node &a = system.nodes[i];
        const Node &b = system.nodes[i + 1];

        const float wide = std::max(a.radius, b.radius);

        const int from = std::clamp(static_cast<int>((std::min(a.at.x, b.at.x) - wide - minX) / system.binSpan), 0,
                                    columns - 1);
        const int to = std::clamp(static_cast<int>((std::max(a.at.x, b.at.x) + wide - minX) / system.binSpan), 0,
                                  columns - 1);

        for (int c = from; c <= to; c++) system.bins[static_cast<std::size_t>(c)].push_back(static_cast<int>(i));
    }

    return system;
}

float Carve(Vector2 world, const System &system) {
    if (system.Empty()) return kFar;

    if (world.x < system.bounds.x || world.x > system.bounds.x + system.bounds.width || world.y < system.bounds.y
        || world.y > system.bounds.y + system.bounds.height) {
        return kFar;
    }

    const int column = static_cast<int>((world.x - system.binOrigin) / system.binSpan);
    if (column < 0 || column >= static_cast<int>(system.bins.size())) return kFar;

    float nearest = kFar;

    for (const int i : system.bins[static_cast<std::size_t>(column)]) {
        const Node &a = system.nodes[static_cast<std::size_t>(i)];
        const Node &b = system.nodes[static_cast<std::size_t>(i) + 1];

        float d = Capsule(world, a.at, b.at, a.radius, b.radius);

        // The floor of a room, cut straight across. Intersecting the swept circle
        // with everything above a line gives the room somewhere to stand, and the
        // line is drawn from the node's own radius so a wider part of the room
        // has a floor further down rather than every room sharing one level.
        const float fill = std::max(a.floor, b.floor);

        if (fill > 0.0f) {
            const float mid   = (a.at.y + b.at.y) * 0.5f;
            const float wide  = std::max(a.radius, b.radius);
            const float floor = mid + wide * (1.0f - 2.0f * fill);

            d = std::max(d, world.y - floor);
        }

        nearest = std::min(nearest, d);
    }

    return nearest;
}

} // namespace cave
