#include "core/config.h"
#include "core/registry.h"
#include "entity/drop.h"
#include "entity/mob/herd.h"
#include "entity/mob/suits.h"
#include "probes/report.h"
#include "world/terrain.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// `--mobcheck [seconds] [away]` — that walking away and coming back changes nothing.
//
// The check the creature layer was rebuilt to be able to pass, and it answers yes or
// no rather than printing a table: it is meant to be run in a loop and to fail a
// build. `--sodcheck` is its precedent and it is the same shape of test — two ways of
// arriving at a state that have to agree.
//
// What it actually asserts, in four stages:
//
//   1. A stretch of country settles a population.
//   2. Walking far enough away that every one of them is put to sleep does not change
//      what is there — the same creatures, the same kinds, the same places.
//   3. Coming back does not change it either, and in particular does not *add*: the
//      old spawner would have rolled a fresh handful on every approach, which is the
//      complaint this replaced.
//   4. Killing them and doing it again leaves them dead. A cell that has settled a
//      kind is never asked for that kind twice, so there is nothing to bring them
//      back — and this is what proves it, because that is one line of code and it is
//      the line the whole design rests on.
//
// Stage 3 is the one that would have caught the old behaviour, and stage 4 is the one
// that catches a well-meaning change to `Patch::settled` later.
namespace {

// How far the view is moved to make everything sleep, in world pixels, unless the
// caller says otherwise. Well past `Warren::kSleepOut`.
constexpr float kAway = 20000.0f;

// The stretch a census covers. Narrower than the view, so that creatures wandering
// at the edge do not drift in and out of the count between stages and read as a
// population that changed.
constexpr float kCensusSpan = 900.0f;
constexpr float kCensusRise = 900.0f;

// One frame of the simulation, at a fixed step.
//
// The real step, not the frame time, because a probe that ran at whatever speed the
// machine managed would be a different test on a different computer.
constexpr float kStep = 1.0f / 60.0f;

Rectangle ViewAt(float x, float y) {
    return {x - 960.0f, y - 540.0f, 1920.0f, 1080.0f};
}

// What a census reduces to for comparison: how many of each kind, and where each
// one is to the nearest pixel.
//
// Positions are rounded because a creature asleep is stored as a float and read back
// as one — the comparison must not turn into a test of floating point, which is not
// what is being asked.
struct Roll {
    std::map<std::string, int> counts;
    std::vector<std::string> where;
};

Roll Take(const mob::Herd &herd, Rectangle box) {
    std::vector<mob::Life> lives;

    herd.Census(box, lives);

    Roll roll;

    for (const mob::Life &life : lives) {
        const char *name = mob::kinds::Of(life.kind).name;

        roll.counts[name]++;

        char at[96];

        std::snprintf(at, sizeof(at), "%s@%.0f,%.0f", name, life.at.x, life.at.y);

        roll.where.emplace_back(at);
    }

    std::sort(roll.where.begin(), roll.where.end());

    return roll;
}

std::string Describe(const Roll &roll) {
    std::string out;

    for (const auto &[name, count] : roll.counts) {
        if (!out.empty()) out += ", ";

        out += std::to_string(count) + " " + name;
    }

    return out.empty() ? "nothing" : out;
}

// Brings the world at one place up to a state the herd can be asked about.
//
// The light is the reason this exists. Half of what a creature's row says is about how
// bright a spot is, and the solver is iterative — one pass is not a lit world. Settling
// against an unsolved region is settling against a dark one, and what that looked like
// was a probe reporting no boars anywhere in twenty thousand pixels while `--mobs` said
// the country was full of them.
void Warm(World &world, Vector2 at) {
    const Rectangle view   = ViewAt(at.x, at.y);
    const Rectangle active = {view.x - config::kSimulationMargin, view.y - config::kSimulationMargin,
                              view.width + config::kSimulationMargin * 2.0f,
                              view.height + config::kSimulationMargin * 2.0f};

    world.Update(active);

    for (int pass = 0; pass < 12; pass++) world.StepLight(active);
}

// Runs the world and the herd forward with the view at one place.
void Play(World &world, mob::Herd &herd, Drops &drops, Vector2 at, float seconds, float &now) {
    const Rectangle view   = ViewAt(at.x, at.y);
    const Rectangle active = {view.x - config::kSimulationMargin, view.y - config::kSimulationMargin,
                              view.width + config::kSimulationMargin * 2.0f,
                              view.height + config::kSimulationMargin * 2.0f};

    const int frames = std::max(1, static_cast<int>(seconds / kStep));

    for (int f = 0; f < frames; f++) {
        world.Update(active);

        // Kept solved rather than left, because the region moves with the view and a
        // creature settled against a stale one is settled against somewhere else. It is
        // by far the dearest thing here, so it is not solved every frame.
        if ((f % 20) == 0) world.StepLight(active);

        now += kStep;

        herd.Update(world, active, {at.x - 6.0f, at.y - 26.0f, 12.0f, 26.0f}, at, now, kStep, drops);
    }
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const float seconds = (bench.argc >= 3) ? static_cast<float>(std::atof(bench.argv[2])) : 3.0f;
    const float away    = (bench.argc >= 4) ? static_cast<float>(std::atof(bench.argv[3])) : kAway;

    mob::Herd herd;

    herd.Memory().Configure(world.Settings().seed);

    Drops drops;

    float now = 0.0f;

    // Wound to the middle of the day before anything is asked.
    //
    // Not optional: half of what a creature's row says is about the light, and this
    // check has no opinion about the hour — it is about whether a place keeps what it
    // has. Leaving the sky wherever it started meant asking a daylight animal to settle
    // at whatever time the world happens to begin at, which is how it came to report an
    // empty county.
    world.StepWeather(kStep);

    for (int spin = 0; spin < 200000; spin++) {
        if (std::fabs(world.Sky().Today().phase - 0.5f) < 0.01f) break;

        world.StepWeather(1.0f);
    }

    // Somewhere with creatures in it, found rather than assumed.
    //
    // The origin used to be taken for granted, and the day the creature table changed
    // it stopped holding anything — the check then reported that it had nothing to say,
    // which is a check that has quietly stopped checking. Where the animals are is a
    // fact about the world and the world is allowed to change; what the probe needs is
    // *a* populated place, and there is no reason it has to be the first one tried.
    Vector2 home{};

    Rectangle box{};

    Roll settled;

    bool found = false;

    for (int t = 0; t < 12 && !found; t++) {
        const float x = static_cast<float>(t) * 1700.0f;

        home = {x, terrain::Height(x, world.Settings()) - 40.0f};

        box = {home.x - kCensusSpan * 0.5f, home.y - kCensusRise * 0.5f, kCensusSpan, kCensusRise};

        Warm(world, home);

        Play(world, herd, drops, home, seconds, now);

        settled = Take(herd, box);

        found = !settled.where.empty();
    }

    const Vector2 far = {home.x + away, terrain::Height(home.x + away, world.Settings()) - 40.0f};

    std::printf("\n%.1f s a stage, %.0f px away, census over %.0f x %.0f at x = %.0f\n\n", seconds, away,
                kCensusSpan, kCensusRise, home.x);

    std::printf("  settled       %s\n", Describe(settled).c_str());

    if (!found) {
        // What the warren holds, so that "nothing here" can be told from "nothing
        // anywhere". A count of cells with no rolls in them is a settling problem; rolls
        // with nothing in the census is a creature that walked out of the box.
        std::printf("\nno creatures in the census. %d cells, %d asked, %d spots tried, %d suited, %d rolled\n",
                    herd.Memory().Remembered(), herd.Memory().Asked(), herd.Memory().Tried(),
                    herd.Memory().Suited(), herd.Memory().Rolled());

        // The sky, because half of what a row asks for is the light and a probe that
        // did not say what hour it was measuring at has not said anything.
        std::printf("the sky is %s, phase %.2f, daylight %.2f; the ground at x = %.0f reads %.2f\n",
                    world.Sky().Today().name, world.Sky().Today().phase, world.Sky().Today().light, home.x,
                    world.LightLevelAt({home.x, home.y - 8.0f}));

        // And one spot taken apart, because a count of nought says only that everything
        // was refused and never which test did it.
        for (int r = 0; r < mob::kinds::Count(); r++) {
            const mob::Def &def = mob::kinds::Of(mob::Kind{r});

            float top = 0.0f;

            if (!world.SurfaceOf(home.x, top)) {
                std::printf("  %s: no surface at all under x = %.0f\n", def.name, home.x);

                continue;
            }

            const Vector2 spot = {home.x, top - 2.0f};

            const float half = def.build.width * 0.5f;

            std::printf("  %s at %.0f,%.0f: depth %.0f (wants %.0f..%.0f), room %s, floor %s, light %.2f "
                        "(wants %.2f..%.2f) -> %s\n",
                        def.name, spot.x, spot.y, spot.y - terrain::Height(spot.x, world.Settings()),
                        def.haunt.fromDepth, def.haunt.toDepth,
                        world.OverlapsSolid({spot.x - half, spot.y - def.build.height * mob::kHeadroom,
                                             def.build.width, def.build.height * mob::kHeadroom - 1.0f})
                            ? "blocked"
                            : "clear",
                        world.OverlapsSolid({spot.x - half, spot.y + 1.0f, def.build.width, 2.0f}) ? "solid"
                                                                                                  : "nothing",
                        world.LightLevelAt({spot.x, spot.y - def.build.height * 0.5f}), def.haunt.brighterThan,
                        def.haunt.darkerThan, mob::Suits(world, def, spot) ? "SUITS" : "refused");
        }

        std::printf("\n");

        return 2;
    }

    // Away, far enough that every one of them is asleep.
    const int rolledAtFirst = herd.Memory().RolledIn(box);

    Play(world, herd, drops, far, seconds, now);

    const Roll asleep = Take(herd, box);

    std::printf("  after leaving %s\n", Describe(asleep).c_str());

    // And back.
    Play(world, herd, drops, home, seconds, now);

    const Roll returned = Take(herd, box);

    const int rolledAfterReturn = herd.Memory().RolledIn(box);

    std::printf("  after coming back %s\n", Describe(returned).c_str());
    std::printf("  the world remembers %d cells, %d rolled, %d lost\n", herd.Memory().Remembered(),
                herd.Memory().Rolled(), herd.Memory().Lost());

    int wrong = 0;

    if (asleep.where != settled.where) {
        std::printf("\nDIFF walking away changed what is there\n");

        wrong++;
    }

    // Coming back must not *create* anything. Compared as rolls and not as bodies, for
    // the reason stage four already gives: a creature missing from the census may have
    // been unmade — the fault — or may simply have wandered a hundred pixels while the
    // stage ran, which is an animal doing what animals do.
    //
    // Counting bodies here reported a failure the first time a boar strolled over the
    // edge of the box, which is a check crying wolf about correct behaviour.
    if (rolledAfterReturn != rolledAtFirst) {
        std::printf("\nDIFF coming back rolled %d more into these cells\n", rolledAfterReturn - rolledAtFirst);

        wrong++;
    }

    // Now kill every one of them and do it again.
    //
    // The question is whether the *cells* roll a fresh population, not whether a
    // creature is standing there afterwards: one that walked in from next door is an
    // animal doing what animals do, and counting bodies cannot tell the two apart.
    // So what is compared is how many have ever been rolled into these cells.
    const int rolledBefore = herd.Memory().RolledIn(box);

    // Cleared far wider than the census, so that nothing left alive is near enough to
    // walk into it during the stages that follow. Without the margin the check is
    // still correct — it counts rolls, not bodies — but it reads as a failure the
    // first time an animal wanders in, and a check that cries wolf gets switched off.
    const Rectangle swept = {box.x - 3000.0f, box.y - 3000.0f, box.width + 6000.0f, box.height + 6000.0f};

    const int struck = herd.Strike(swept, 9999, {swept.x - 1000.0f, swept.y}, 0.0f, 0.0f);

    // One blow each, and the mercy window means one blow is all that lands this
    // frame — which is enough, at that damage.
    Play(world, herd, drops, home, 0.5f, now);

    const Roll killed = Take(herd, box);

    std::printf("\n  struck %d, left  %s\n", struck, Describe(killed).c_str());

    Play(world, herd, drops, far, seconds, now);
    Play(world, herd, drops, home, seconds, now);

    const Roll after = Take(herd, box);

    const int rolledAfter = herd.Memory().RolledIn(box);

    std::printf("  and after leaving and coming back  %s\n", Describe(after).c_str());
    std::printf("  rolled into these cells: %d before, %d after\n", rolledBefore, rolledAfter);

    if (rolledAfter != rolledBefore) {
        std::printf("\nDIFF the dead came back — these cells rolled %d more\n", rolledAfter - rolledBefore);

        wrong++;
    }

    std::printf("\n%s\n\n", (wrong == 0) ? "consistent: a place holds its creatures, and the dead stay dead"
                                         : "inconsistent");

    return (wrong == 0) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--mobcheck",
    .wants = 2,
    .shows = false,
    .blurb = "--mobcheck [seconds] [away] - that walking away and back changes nothing",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
