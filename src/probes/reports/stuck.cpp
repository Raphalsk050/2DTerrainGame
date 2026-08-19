#include "core/config.h"
#include "core/registry.h"
#include "probes/report.h"
#include "world/element.h"
#include "world/world.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// `--stuck [cells]` — ground that stops a body and draws nothing.
//
// The fault, reported from play: *sometimes I break a block and get stuck on something
// invisible.* The debug overlay showed a lattice vertex standing on its own in open
// space, and the player's own words are the specification — a block like that must not
// exist.
//
// It is one question asked twice with two different rules. A square is painted where the
// field **interpolated at its centre** is over the threshold; a body stops where the
// **nearest lattice vertex** is. Those agree in the middle of a hillside, part company by
// up to half a lattice step at every edge, and at a vertex standing alone they part
// company altogether: the nearest texel centre is a quarter of a cell away in each
// direction, so it reads nine sixteenths of the vertex's value, and every vertex between
// the threshold and nine fifths of it is a full square of solid ground with not one
// pixel painted anywhere in it.
//
// Which is §13.5's fault one layer down. That section is about a cell whose middle reads
// as sky while a corner still holds ground — invisible, impassable and immovable at once
// — and it fixed the hand's third of it, so such a vertex can at least be dug. This is
// the two thirds that remain: the player cannot see that there is anything to dig.
//
// So the sweep is in two halves, and they are different questions:
//
//   - **Ground nobody has touched.** Whether the generator makes them on its own. It has
//     no reason to — its field is smooth and a lone spike is not a shape noise makes —
//     but "has no reason to" is not a measurement.
//   - **Ground that has been dug.** Which is where they come from: a cell at the contour
//     edge holds one vertex of its nine, the player clears the cells around it because
//     those are the ones they can see, and the one that is left is the thing they walk
//     into.
namespace {

// The band either side of the surface that a body can be in. Below it is solid rock,
// where every vertex has painted neighbours; above it is sky.
constexpr float kAbove = 3.0f;
constexpr float kBelow = 8.0f;

// How big a hole the digging half opens. Six cells square is a doorway, which is the
// smallest thing a player actually digs and enough boundary to meet the fault.
constexpr int kHole = 6;

struct Tally {
    long vertices = 0;
    long solid    = 0;
    long stuck    = 0;

    // The worst one seen, so the report names a place rather than a count.
    Vector2 worst{};
};

// Every vertex in a rectangle of world, weighed against `World::Degenerate`.
//
// Through the world's own function and never a copy of it: what makes this a check of
// the game rather than of a second implementation of the game is that the collision
// test, the paint test and this all read the one answer. §28.7's rule.
void Sweep(const World &world, Rectangle over, Tally &into) {
    const auto step = static_cast<float>(config::kResolution);

    for (float x = std::floor(over.x / step) * step; x <= over.x + over.width; x += step) {
        for (float y = std::floor(over.y / step) * step; y <= over.y + over.height; y += step) {
            into.vertices++;

            if (!world.IsSolidAt({x, y})) continue;

            into.solid++;

            if (!world.Degenerate({x, y})) continue;

            if (into.stuck == 0) into.worst = {x, y};

            into.stuck++;
        }
    }
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const int wanted = (bench.argc >= 3) ? std::max(1, std::atoi(bench.argv[2])) : 400;

    const auto cellSide = static_cast<float>(config::kBuildCell);

    Tally virgin{};
    Tally dug{};
    Tally again{};

    long holes = 0;

    std::printf("\n");

    for (float x = 0.0f; holes < wanted && x < 60000.0f; x += cellSide * (kHole + 2)) {
        const Rectangle around = {x - 384.0f, -640.0f, 768.0f, 1600.0f};

        world.Update(around);

        float top = 0.0f;
        if (!world.SurfaceOf(x, top)) continue;

        // The band the surface runs through, which is the only place a cell holds two
        // materials or a fraction of one. Deep rock is solid and the sky is empty, and
        // neither can be degenerate.
        const Rectangle band = {x, top - cellSide * kAbove, cellSide * kHole, cellSide * (kAbove + kBelow)};

        Sweep(world, band, virgin);

        // And now dig, the way a player does: whole cells, through the world's own
        // spade. A probe that wrote the field directly would be measuring a state the
        // game cannot reach.
        int cx = 0;
        int cy = 0;
        World::ToCell({x, top}, cx, cy);

        for (int i = 0; i < kHole; i++) {
            for (int j = 0; j < kHole; j++) world.ExcavateCell(cx + i, cy + j);
        }

        Sweep(world, band, dug);

        // And then walked away from and come back to.
        //
        // The sweep clears vertices the *stroke* was not asked about, so each one has to
        // be written into the chunk's journal on its own account or the hole is only
        // clean until the chunk is dropped. An edited chunk is pinned, so a session
        // never shows it — §11.2's repro exactly, and the reason that section says to
        // walk forty thousand pixels away and back.
        world.Update({x + 40000.0f, 0.0f, 512.0f, 512.0f});
        world.Update(around);

        Sweep(world, band, again);

        holes++;
    }

    std::printf("%ld holes of %dx%d cells opened along the surface\n\n", holes, kHole, kHole);

    std::printf("  %-22s %10s %10s %10s\n", "", "vertices", "solid", "invisible");
    std::printf("  %-22s %10ld %10ld %10ld\n", "as the world made it", virgin.vertices, virgin.solid, virgin.stuck);
    std::printf("  %-22s %10ld %10ld %10ld\n", "after digging", dug.vertices, dug.solid, dug.stuck);
    std::printf("  %-22s %10ld %10ld %10ld\n", "and back from memory", again.vertices, again.solid, again.stuck);

    for (const Tally &tally : {virgin, dug, again}) {
        if (tally.stuck <= 0) continue;

        std::printf("\n  first was at %.0f, %.0f\n", static_cast<double>(tally.worst.x),
                    static_cast<double>(tally.worst.y));

        break;
    }

    // The chunk has to come back holding what it was left holding, or the hole is clean
    // only until it is dropped. Counted rather than only scanned for stranded vertices:
    // a chunk that came back with *more* ground than it went away with is the journal
    // having forgotten the sweep, which is a different fault with the same symptom, and
    // one a count of stranded vertices alone would report as a fresh set of them.
    const bool kept = again.solid == dug.solid;

    if (!kept) {
        std::printf("\n  FORGOTTEN: %ld solid vertices went away and %ld came back\n", dug.solid, again.solid);
    }

    const bool sound = virgin.stuck == 0 && dug.stuck == 0 && again.stuck == 0 && kept;

    std::printf("\n  %s\n\n", sound ? "nothing stops a body without drawing itself, and it stays that way"
                                    : "STUCK — a body can be stopped by ground nothing paints");

    // Nothing found is not a pass. A sweep that met no boundary has checked nothing, and
    // would go on reporting success after a change that removed every one of them —
    // §23.1's lesson about printing the count, and `--dig`'s own guard.
    if (holes == 0 || dug.solid == 0) {
        std::printf("no ground was found to dig — this checked nothing\n\n");

        return 1;
    }

    return sound ? 0 : 1;
}

const probes::Report row = {
    .name  = "--stuck",
    .wants = 2,
    .shows = false,
    .blurb = "--stuck [holes] - ground that stops a body and draws nothing",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
