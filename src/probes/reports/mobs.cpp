#include "core/registry.h"
#include "entity/mob/mob_def.h"
#include "probes/report.h"
#include "world/element.h"
#include "world/terrain.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// `--mobs x0 x1 [step] [hours]` — where each creature would actually appear.
//
// The companion to `--woods`, and it exists for the reason that one does, stated in
// CLAUDE.md §8: a placement rule written into a table is very easy to author into
// something that is never anywhere, and *nothing errors*. A creature is the worst
// case of it — one that never spawns looks exactly like one that spawns somewhere
// you have not walked, and the only way to tell them apart by eye is to walk the
// whole world.
//
// The startup checks in `mob_checks.cpp` catch a row that is self-contradictory: a
// band with its ends the wrong way round, a crowd smaller than a group. They cannot
// catch a row that is merely *wrong about this world* — a climate bell centred where
// no country is, a depth band under the deepest cave, a light band no lit place ever
// reaches. That is what this walks the ground to find out.
//
// **It sweeps the day by default, and that is the whole design of the verdict.** A
// creature of the dark is legitimately nowhere at noon, so a report at one hour
// cannot tell "this row is wrong" from "you asked at the wrong time". Run without an
// hour and every row is checked against midnight, dawn, noon and dusk; the status is
// about whether it appears at *any* of them, which is the question that has a right
// answer. Give an hour and that one is walked alone, with no verdict — that form is
// for looking at a particular time rather than for failing a build.
namespace {

// Where the surface is asked about, relative to it. A creature stands on the ground
// rather than at it, so the probe puts one at each sample the way the spawner would.
constexpr float kStandOn = 2.0f;

// How deep the underground band is walked, and how coarsely.
//
// Coarse: the point is whether a creature has anywhere at all, not a census. A step
// of a hundred and twenty over a two-thousand-pixel band is seventeen readings a
// column, and the light has to be solved for every one of them.
constexpr float kDeepStep = 120.0f;

// Below this a spot counts as the surface rather than as a cave.
constexpr float kUnderground = 64.0f;

struct Tally {
    long spots  = 0;
    long ground = 0;
    long caves  = 0;

    // Why the rest were refused, so a row that fails says which of its own fields
    // did it. Without this the report says "nowhere" and leaves you guessing which
    // of five bands is the wrong one.
    long wrongDepth   = 0;
    long noRoom       = 0;
    long wrongLight   = 0;
    long wrongClimate = 0;
};

// The same order `spawn::Suits` uses, taken apart so that each refusal can be
// counted. Kept deliberately parallel to it: if the two ever disagree, this report
// is describing a spawner that does not exist.
void Judge(const World &world, const mob::Def &def, Vector2 at, Tally &tally) {
    const mob::Haunt &haunt = def.haunt;

    const float depth = at.y - terrain::Height(at.x, world.Settings());

    if (depth < haunt.fromDepth || depth > haunt.toDepth) {
        tally.wrongDepth++;

        return;
    }

    const float half = def.build.width / 2.0f;

    const Rectangle box = {at.x - half, at.y - def.build.height * 1.25f, def.build.width,
                           def.build.height * 1.25f};

    if (world.OverlapsSolid(box)) {
        tally.noRoom++;

        return;
    }

    if (!def.build.floats && !world.OverlapsSolid({at.x - half, at.y + 1.0f, def.build.width, 2.0f})) {
        tally.noRoom++;

        return;
    }

    const float lit = world.LightLevelAt({at.x, at.y - def.build.height * 0.5f});

    if (lit > haunt.darkerThan || lit < haunt.brighterThan) {
        tally.wrongLight++;

        return;
    }

    const terrain::Climate climate = terrain::ClimateAt(at.x, world.Settings());

    if (ClimateBell(haunt.climate, climate.temperature, climate.humidity) < haunt.climate.goneAt) {
        tally.wrongClimate++;

        return;
    }

    tally.spots++;

    if (depth > kUnderground) {
        tally.caves++;
    } else {
        tally.ground++;
    }
}

// One sweep of the world at whatever time of day it currently is.
void Walk(World &world, float x0, float x1, float step, std::vector<Tally> &tallies, long &columns) {
    const int rows = mob::kinds::Count();

    for (float x = x0; x < x1; x += step) {
        const float surface = terrain::Height(x, world.Settings());

        // The world has to be resident and lit before anything can be asked about it.
        // A region per column rather than one across the whole span, because a span
        // of a hundred thousand pixels is not something that fits in memory — which
        // is the same reason the game streams at all.
        const Rectangle region = {x - 480.0f, surface - 400.0f, 960.0f, 2900.0f};

        world.Update(region);
        world.StepLight(region);

        columns++;

        for (int r = 0; r < rows; r++) {
            const mob::Def &def = mob::kinds::Of(mob::Kind{r});

            Tally &tally = tallies[static_cast<std::size_t>(r)];

            // At the surface first, which is the one spot every ground creature
            // cares about.
            Judge(world, def, {x, surface - kStandOn}, tally);

            // Then down through the rock. Only where the row asks for it, so a
            // surface animal does not pay for seventeen readings it can never use.
            if (def.haunt.toDepth <= kUnderground) continue;

            for (float d = 96.0f; d <= def.haunt.toDepth; d += kDeepStep) {
                Judge(world, def, {x, surface + d}, tally);
            }
        }
    }
}

// Winds the sky until it says it is the hour asked for.
//
// Driven off `Daylight::phase` rather than off a number of seconds, and that is not
// fussiness: a day here is however long `weather` says it is, so winding "twelve
// hours" as forty-three thousand seconds is twelve hours of *some other clock*.
// Asking the sky is the only way to be sure the report's own header is telling the
// truth.
void WindTo(World &world, float hours) {
    const float wanted = std::fmod(std::max(hours, 0.0f), 24.0f) / 24.0f;

    // Bounded, so a phase that never arrives is a report that comes out slightly
    // wrong rather than a program that never returns.
    for (int spin = 0; spin < 200000; spin++) {
        const float phase = world.Sky().Today().phase;

        const float gap = std::fabs(phase - wanted);

        if (std::min(gap, 1.0f - gap) < 0.01f) return;

        world.StepWeather(1.0f);
    }
}

void Print(float hours, const char *when, long columns, const std::vector<Tally> &tallies) {
    // The hour is on every table, whether it was asked for or not.
    //
    // Half of what a creature's row says is about the light, so a reading of this
    // table without the time on it is a reading of a different world. It is the one
    // line that stops "the shade appears nowhere" being acted on when what it means
    // is "the shade appears nowhere at noon".
    std::printf("\nat %04.1fh (%s) - %ld columns\n\n", hours, when, columns);

    std::printf("%-10s %8s %8s %8s   %8s %8s %8s %8s\n", "mob", "spots", "surface", "cave", "depth", "room",
                "light", "climate");

    for (int r = 0; r < mob::kinds::Count(); r++) {
        const mob::Def &def = mob::kinds::Of(mob::Kind{r});
        const Tally &t      = tallies[static_cast<std::size_t>(r)];

        std::printf("%-10s %8ld %8ld %8ld   %8ld %8ld %8ld %8ld", def.name, t.spots, t.ground, t.caves,
                    t.wrongDepth, t.noRoom, t.wrongLight, t.wrongClimate);

        if (def.haunt.chance <= 0.0f) std::printf("   (never spawns on its own)");

        std::printf("\n");
    }
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const float x0   = static_cast<float>(std::atof(bench.argv[2]));
    const float x1   = static_cast<float>(std::atof(bench.argv[3]));
    const float step = (bench.argc >= 5) ? static_cast<float>(std::atof(bench.argv[4])) : 240.0f;

    if (x1 <= x0 || step <= 0.0f) {
        std::printf("--mobs x0 x1 [step] [hours]\n");

        return 2;
    }

    const int rows = mob::kinds::Count();

    const bool sweeping = bench.argc < 6;

    const float clock[] = {0.0f, 6.0f, 12.0f, 18.0f};

    const int howMany = sweeping ? 4 : 1;
    const float asked = sweeping ? 0.0f : static_cast<float>(std::atof(bench.argv[5]));

    // One frame first, because the daylight is worked out by StepWeather and is
    // nothing until it has run — the same order `--sky` uses.
    world.StepWeather(1.0f / 60.0f);

    std::printf("\n%.0f to %.0f px, every %.0f\n", x0, x1, step);

    // Where each row was found at all, across every hour walked.
    std::vector<long> anywhere(static_cast<std::size_t>(rows), 0);

    for (int h = 0; h < howMany; h++) {
        const float hours = sweeping ? clock[h] : asked;

        WindTo(world, hours);

        // Cold for each hour. Carrying the counts across would make the refusal
        // columns a sum over four different skies, which is a number about nothing.
        std::vector<Tally> tallies(static_cast<std::size_t>(rows));

        long columns = 0;

        Walk(world, x0, x1, step, tallies, columns);

        Print(hours, world.Sky().Today().name, columns, tallies);

        for (int r = 0; r < rows; r++) {
            anywhere[static_cast<std::size_t>(r)] += tallies[static_cast<std::size_t>(r)].spots;
        }
    }

    if (!sweeping) {
        std::printf("\none hour only - no verdict. Run without an hour to check the whole day.\n\n");

        return 0;
    }

    std::printf("\n");

    // A verdict and not just a table, on the argument CLAUDE.md §15 makes about
    // `probes::Run` returning a status: a check that cannot fail a build is a check
    // nobody runs twice.
    int lost = 0;

    for (int r = 0; r < rows; r++) {
        const mob::Def &def = mob::kinds::Of(mob::Kind{r});

        if (def.haunt.chance <= 0.0f) continue;
        if (anywhere[static_cast<std::size_t>(r)] > 0) continue;

        std::printf("'%s' appears NOWHERE in this stretch at any hour\n", def.name);

        lost++;
    }

    if (lost == 0) std::printf("every mob that spawns has somewhere to spawn\n");

    std::printf("\n");

    return (lost == 0) ? 0 : 1;
}

// The row, then the registrar that files it. Order inside one translation unit is
// top to bottom and is guaranteed, which is what lets the row be a plain object
// rather than something allocated.
const probes::Report row = {
    .name  = "--mobs",
    .wants = 4,
    .shows = false,
    .blurb = "--mobs x0 x1 [step] [hours] - where each creature would actually appear",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
