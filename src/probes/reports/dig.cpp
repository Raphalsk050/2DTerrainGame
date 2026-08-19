#include "core/registry.h"
#include "core/stack.h"
#include "core/tool.h"
#include "probes/report.h"
#include "world/element.h"
#include "world/world.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

// `--dig [cells]` — what a cell made of two materials is worth.
//
// The fault it was written for, reported from play: *sometimes I break a block and
// nothing drops*. It is not sometimes and it is not random. The ground is a field
// and a build cell is nine samples of it, so a cell sitting on the line between soil
// and rock genuinely holds five of one and four of the other — and `Editor::Bank`
// pays a block per `kVerticesPerBlock` **of the same material**. Five ninths in one
// ledger and four ninths in another is nought blocks and nought blocks, so the cell
// broke and the player got nothing, with the debt owed to them in two places neither
// of which was ever going to reach nine on its own.
//
// So this walks real ground, finds the cells that straddle a boundary, and reports
// what each one pays under both rules. The old figure is computed here beside the
// new one deliberately: a probe that only asserted the fix would say nothing about
// how often the fault fired, and "how often" is the whole reason it was worth
// fixing rather than explaining.
//
// The verdict is one-sided and narrow, which is what makes it worth having:
//
//   - **A cell that is full and drops nothing is a fault.** Nine vertices are a
//     whole block by construction — see `kVerticesPerBlock` — so there is no reading
//     of the economy under which it is right.
//   - **A cell that is not full may drop nothing, and that is not a fault.** Three
//     vertices are a third of a block; `Editor::owed_` banks the fraction and pays it
//     out on the next cell. Calling that a failure would be a check against the
//     ledger existing.
namespace {

// How much of a cell is filled, and by what.
struct Cell {
    World::Yield holds{};

    int solid = 0; // Vertices held by anything that occupies.
    int kinds = 0; // How many different occupying materials are in it.
};

Cell Read(const World &world, int cx, int cy) {
    Cell cell{};

    world.CellHolds(cx, cy, cell.holds);

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (cell.holds[e] <= 0 || !kElements[e].rules.occupies) continue;

        cell.solid += cell.holds[e];
        cell.kinds++;
    }

    return cell;
}

// Blocks paid out under the rule as it was: each material's own vertices, floored to
// whole blocks, with the remainder carried. The carry is what made the fault hard to
// see from inside a session — dig enough boundary cells and the fractions do
// eventually pay — so this counts the *immediate* yield, which is what the player
// watching one block break actually sees.
int Scattered(const World::Yield &freed) {
    int blocks = 0;

    for (std::size_t e = 0; e < kElementCount; e++) {
        blocks += static_cast<int>(static_cast<float>(freed[e]) / kVerticesPerBlock);
    }

    return blocks;
}

// And under the rule as it is: everything that occupies goes to the one material the
// cell counted as.
int Collapsed(const World::Yield &freed) {
    const std::optional<Element> chief = World::ChiefOf(freed);

    World::Yield onto{};

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (freed[e] <= 0) continue;

        const bool part = chief.has_value() && kElements[e].rules.occupies;

        onto[part ? ElementIndex(*chief) : e] += freed[e];
    }

    return Scattered(onto);
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const int wanted = (bench.argc >= 3) ? std::max(1, std::atoi(bench.argv[2])) : 400;

    const auto cellSide = static_cast<float>(config::kBuildCell);

    long walked  = 0;
    long mixed   = 0;
    long full    = 0;
    long starved = 0; // Full, mixed, and paying nothing under the old rule.
    long wrong   = 0; // Paying nothing under the new one, which must never happen.
    long moved   = 0; // Where the two rules disagree at all.

    long chiefWrong = 0;

    std::printf("\n%-9s %-9s %6s  %-22s %5s %5s\n", "at x", "at y", "solid", "holds", "was", "now");

    // Along the surface, where two materials meet: soil over rock is the commonest
    // boundary in this world and is exactly the one the report came from.
    for (float x = 0.0f; walked < wanted && x < 60000.0f; x += cellSide) {
        const Rectangle around = {x - 256.0f, -512.0f, 512.0f, 1400.0f};

        world.Update(around);

        float top = 0.0f;
        if (!world.SurfaceOf(x, top)) continue;

        // A band either side of the surface, which is where a cell can hold two
        // things at once. Deep rock is one material and open sky is none.
        for (float y = top - cellSide * 2.0f; y < top + cellSide * 6.0f; y += cellSide) {
            int cx = 0;
            int cy = 0;
            World::ToCell({x, y}, cx, cy);

            const Cell cell = Read(world, cx, cy);
            if (cell.solid <= 0) continue;

            walked++;

            if (cell.kinds < 2) continue;

            mixed++;

            const std::optional<Element> chief = World::ChiefOf(cell.holds);

            // What ChiefOf claims, checked against a plain scan of the same counts.
            int most = 0;
            for (std::size_t e = 0; e < kElementCount; e++) {
                if (kElements[e].rules.occupies) most = std::max(most, cell.holds[e]);
            }

            if (!chief || cell.holds[ElementIndex(*chief)] != most) chiefWrong++;

            const int was = Scattered(cell.holds);
            const int now = Collapsed(cell.holds);

            if (was != now) moved++;

            const bool brimming = cell.solid >= static_cast<int>(kVerticesPerBlock);

            if (brimming) {
                full++;

                if (was <= 0) starved++;
                if (now <= 0) wrong++;
            }

            // A handful of them printed, because the shape of the fault is easier to
            // believe from three real cells than from a percentage.
            if (mixed <= 8) {
                char what[128] = {};
                int at         = 0;

                for (std::size_t e = 0; e < kElementCount && at < 100; e++) {
                    if (cell.holds[e] <= 0) continue;

                    at += std::snprintf(what + at, sizeof(what) - static_cast<std::size_t>(at), "%s%d %s",
                                        (at > 0) ? " + " : "", cell.holds[e], kElements[e].name);
                }

                std::printf("%-9.0f %-9.0f %6d  %-22s %5d %5d\n", x, y, cell.solid, what, was, now);
            }
        }
    }

    std::printf("\n%ld cells of ground walked, %ld of them holding two materials or more (%.1f%%)\n", walked, mixed,
                100.0 * static_cast<double>(mixed) / std::max(walked, 1L));
    std::printf("%ld of those were full cells; %ld of them used to pay nothing at all\n", full, starved);
    std::printf("%ld cells pay differently under the two rules\n\n", moved);

    const bool sound = wrong == 0 && chiefWrong == 0;

    if (chiefWrong > 0) std::printf("CHIEF WRONG: %ld cells named a material that is not the commonest\n", chiefWrong);
    if (wrong > 0) std::printf("STARVED: %ld full cells still pay nothing\n", wrong);

    std::printf("%s\n\n", sound ? "every full cell pays a whole block, of the material most of it was"
                                : "a full cell can still break for nothing");

    // Nothing found is not a pass. A sweep that met no boundary has checked nothing,
    // and would go on reporting success after a change that removed every one of
    // them — which is §23.1's lesson about printing the count.
    if (mixed == 0) {
        std::printf("no cell in %ld held two materials — this checked nothing\n\n", walked);

        return 1;
    }

    return sound ? 0 : 1;
}

const probes::Report row = {
    .name  = "--dig",
    .wants = 2,
    .shows = false,
    .blurb = "--dig [cells] - what a cell made of two materials pays out",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
