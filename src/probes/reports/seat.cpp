#include "core/config.h"
#include "core/registry.h"
#include "entity/drop.h"
#include "entity/fixture.h"
#include "probes/report.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// `--seat [cells]` — whether what the player puts down is standing on the ground.
//
// The fault, reported from play twice over: *the chest is floating.* It was, and by up
// to a whole cell, and nothing in the code said so — a fixture was drawn standing on the
// bottom edge of its own cell, which is a line the build lattice rules across the
// hillside and is not where the ground is. The ground inside a cell is a contour that
// crosses wherever the field says (§13.7 is the same gap seen from the body's side), so
// the two agree only where the ground happens to end exactly on the grid.
//
// It could not be seen in a still, either, and that is why this exists. A chest on flat
// dug ground sits perfectly; a chest on a hillside hangs; and the difference is a
// property of the *column* it was put down in, so whether a screenshot shows the bug
// depends on where the player was standing when they took it.
//
// What it measures is deliberately not the seating arithmetic — asking `FootingUnder`
// what `FootingUnder` said is photographing the reproduction (§28.7). It asks the
// **picture**: is the world painted immediately under this thing's feet, and is it
// painted immediately above them. Those two are what "standing on the ground" means to
// the only witness that matters.
namespace {

// How many hillsides to try. Enough that the contour lands at every part of a cell,
// since a cell whose ground ends on the grid line is exactly the case the old code got
// right.
constexpr int kDefaultCells = 60;

// Where they are cut, and how far apart. Prime-ish spacing so the columns do not land in
// step with anything in the generator.
constexpr float kFrom  = 1200.0f;
constexpr float kEvery = 337.0f;

// How far under the feet the ground has to be painted, and how far over them it has to
// be clear.
//
// One texel each way. The seat is quantised onto the drawn grid by `DrawnTop`, so the
// first painted row is within a texel below it by construction — and anything painted a
// texel *above* it is ground the picture is standing inside.
constexpr float kTexel = static_cast<float>(config::kPixelSize);

struct Tally {
    int cells   = 0;
    int floated = 0;
    int sunk    = 0;

    // The worst gap seen between the feet and the ground, and where.
    float worst    = 0.0f;
    float worstAt  = 0.0f;

    // What the old rule would have left, so the report says what was actually bought
    // rather than only that it is now nought.
    float wasWorst = 0.0f;
};

// The cell over the ground in this column that will actually hold a chest, or false
// where there is no ground or nothing in the column will hold one.
//
// Walked up rather than worked out, and that is the point of it: the cell the surface
// falls in is usually *not* the one a fixture goes in, because a cell is eighteen pixels
// and the ground drawn anywhere inside it fills the cell as far as a body is concerned.
// The one the player clicks is the first clear cell above that, and it is the cell whose
// floor can be a whole cell above the ground -- which is the fault this file is about.
bool CellOver(const World &world, const fixture::Fixtures &chests, float x, int &outCx, int &outCy) {
    float top = 0.0f;

    if (!world.SurfaceOf(x, top)) return false;

    int cx = 0;
    int cy = 0;

    World::ToCell({x, top}, cx, cy);

    for (int up = 0; up < 4; up++) {
        if (chests.Holds(world, fixture::Kind::Chest, cx, cy - up)) {
            outCx = cx;
            outCy = cy - up;

            return true;
        }
    }

    return false;
}

void Sweep(World &world, fixture::Fixtures &chests, Drops &drops, int cells, Tally &into) {
    for (int i = 0; i < cells; i++) {
        const float x = kFrom + static_cast<float>(i) * kEvery;

        // Streamed first, exactly as the game does. A probe that builds into chunks that
        // are not resident writes nothing and reports a wall of failures — §22.6's
        // lesson, learned by `--mobcheck`.
        world.Update({x - 400.0f, -1200.0f, 800.0f, 2400.0f});

        int cx = 0;
        int cy = 0;

        if (!CellOver(world, chests, x, cx, cy)) continue;
        if (!chests.Place(fixture::Kind::Chest, cx, cy)) continue;

        chests.Settle(world, drops, 0.0f);

        const std::optional<float> seat = chests.SeatAt(cx, cy);

        if (!seat.has_value()) continue;

        const Rectangle cell = World::CellBounds(cx, cy);
        const float middle   = cell.x + cell.width * 0.5f;

        into.cells++;

        // Ground under the feet, within a texel. Walked down rather than sampled once,
        // so the report can say *how far* it is floating instead of only that it is.
        float gap = 0.0f;

        while (gap < config::kBuildCell * 2 && !world.PaintedAt({middle, *seat + gap + 1.0f})) gap += 1.0f;

        if (gap > kTexel) {
            into.floated++;

            if (gap > into.worst) {
                into.worst   = gap;
                into.worstAt = x;
            }
        }

        // And sky over them. A seat below the drawn surface is a chest with its feet in
        // the hillside, which is the other way this can be wrong and the one a fix aimed
        // only at floating would introduce.
        if (world.PaintedAt({middle, *seat - kTexel})) into.sunk++;

        // How far over the ground the grid line would have left it, which is what this
        // used to draw on. Downwards is positive here, as everywhere: the seat is *below*
        // the cell's own floor by however much the contour crosses inside the cell under
        // it, and that difference is the whole of what the fix bought.
        const float wasGap = *seat - (cell.y + cell.height);

        into.wasWorst = std::max(into.wasWorst, wasGap);
    }
}

// A chest on a chest, which is the second half of what was asked for: *fixed to the
// ground, or on top of another chest.*
//
// Checked through `Holds` and `Settle` rather than by reasoning about `kAtop`, and in
// both directions — a stack that stands is only half the rule, and the half that is easy
// to get by accident is the one where everything stands, anywhere, including in the air.
bool Stacks(World &world, fixture::Fixtures &chests, Drops &drops, float x) {
    world.Update({x - 400.0f, -1200.0f, 800.0f, 2400.0f});

    int cx = 0;
    int cy = 0;

    if (!CellOver(world, chests, x, cx, cy)) {
        std::printf("  stacking: no ground found at %.0f — this checked nothing\n", x);

        return false;
    }

    if (!chests.Place(fixture::Kind::Chest, cx, cy)) return false;

    // One directly above it, which nothing in the world holds up.
    const bool held = chests.Holds(world, fixture::Kind::Chest, cx, cy - 1);

    if (!held || !chests.Place(fixture::Kind::Chest, cx, cy - 1)) {
        std::printf("  stacking: a chest is refused the lid of another\n");

        return false;
    }

    // And one a cell above *that*, standing in open air with nothing under it at all.
    const bool floats = chests.Holds(world, fixture::Kind::Chest, cx, cy - 3);

    chests.Settle(world, drops, 0.0f);

    const std::optional<float> under = chests.SeatAt(cx, cy);
    const std::optional<float> over  = chests.SeatAt(cx, cy - 1);

    if (!under.has_value() || !over.has_value()) {
        std::printf("  stacking: a stacked chest was not seated\n");

        return false;
    }

    const float step = *under - *over;

    std::printf("  stacking: on a chest yes, in the air %s, a cell apart at %.1f px\n", floats ? "YES" : "no", step);

    // Exactly a cell apart, which is what keeps a tower glued together whatever the
    // ground under its base is doing.
    return !floats && std::fabs(step - config::kBuildCell) < 0.5f;
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const int cells = (bench.argc > 2) ? std::atoi(bench.argv[2]) : kDefaultCells;

    fixture::Fixtures chests;
    Drops drops;

    Tally tally;

    Sweep(world, chests, drops, std::max(cells, 1), tally);

    std::printf("\n--seat: what is put down, against the ground it is drawn over\n\n");
    std::printf("  hillsides:  %d\n", tally.cells);
    std::printf("  floating:   %d  (worst %.0f px, at x %.0f)\n", tally.floated, tally.worst, tally.worstAt);
    std::printf("  sunk in:    %d\n", tally.sunk);
    std::printf("  on the grid line it would have been up to %.0f px over the ground\n\n", tally.wasWorst);

    const bool stacks = Stacks(world, chests, drops, kFrom - 900.0f);

    std::printf("\n");

    if (tally.cells == 0) {
        // §23.1's lesson, and `--dig`'s own guard: a sweep that met no ground has checked
        // nothing, and would go on reporting success after a change that removed every
        // hillside in it.
        std::printf("no ground was found to stand anything on — this checked nothing\n\n");

        return 1;
    }

    const bool seated = tally.floated == 0 && tally.sunk == 0;

    std::printf("%s\n", seated ? "  every fixture is standing on ground that is drawn under it"
                               : "  FAILED: something is standing on the grid rather than on the ground");
    std::printf("%s\n\n", stacks ? "  a chest stands on a chest, a cell up, and on nothing else"
                                 : "  FAILED: the rule about standing on another chest does not hold");

    return (seated && stacks) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--seat",
    .wants = 2,
    .shows = false,
    .blurb = "--seat [cells] - whether what is put down stands on the ground it is drawn over",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
