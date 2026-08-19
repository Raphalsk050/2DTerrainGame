#include "core/registry.h"
#include "core/stack.h"
#include "entity/body/body.h"
#include "entity/mob/mob_def.h"
#include "entity/mob/brain.h"
#include "entity/nav/plan.h"
#include "entity/nav/reach.h"
#include "probes/report.h"
#include "world/element.h"
#include "world/terrain.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// `--nav [mob]` — that a creature clears exactly the holes and ledges its legs allow,
// in one attempt, at every pace it travels at.
//
// A course is built out of blocks rather than looked for in the landscape, and that is
// the whole point: a hillside has no holes of a known width in it, so a test against
// the world can only ever say "it got somewhere", which is what watching it does and
// is not a check. Here the gap is four cells because it was built four cells wide.
//
// It drives `nav::Plan` and `body::Body` directly and never a brain. What a creature
// *wants* is a behaviour and belongs to `--mobs`; whether a body asked to go right can
// get right is a question with one answer, and this is it.
//
// **Three things are checked, and the third is why the file exists.**
//
//   - `FELL SHORT` — the planner promised a jump the body could not make. A fault: the
//     creature is now at the bottom of the hole it was told it could clear.
//   - `spare` — the planner refused something the body could in fact have done. Not a
//     fault. `nav::kSpanUsable` holds it to under three quarters of the arc the
//     physics allows, deliberately, so the body will always out-reach the planner near
//     the limit. A check that called that a disagreement would be a check against the
//     margin existing.
//   - `SPAMMED` — more than a couple of attempts at one obstacle. A fault whether or
//     not it eventually got past: what it looks like on screen is an animal
//     head-butting a ledge, and it is what this probe was extended to catch.
//
// **Both paces are run, and the slow one is the point.** A creature spends most of its
// life ambling, and that is where the navigator fell apart — a boar at a third of its
// run covers fourteen pixels in its whole arc and cannot climb a terrace at all. A
// probe that only tested the bolt said everything was fine.
namespace {

// One frame, at the fixed step. Never the real frame time: a probe that ran at
// whatever the machine managed would be a different test on a different computer.
constexpr float kStep = 1.0f / 60.0f;

// How long a creature is given to get past one obstacle, as a multiple of the time the
// course would take at its own pace on flat ground.
//
// A multiple and not a figure, and the fixed figure it replaced was quietly ruining
// the slow half of the report: six seconds is generous at a bolt and not enough to
// walk the course at all at an amble, so every ambling row failed for want of time and
// read as a navigator that could not do anything. The trace is what showed it —
// `walk:359` is a creature walking happily and running out of clock.
constexpr float kPatienceOver = 4.0f;

// And a floor under it, for the courses where the pace is nearly zero to begin with.
constexpr float kPatienceLeast = 4.0f;

// Where the courses are built, in world Y above the surface at the origin.
//
// Open air, so the landscape underneath cannot be walked along instead — a creature
// that fell off and carried on over the hillside would read as a success.
constexpr float kOverhead = 700.0f;

// How long the run-up is, in build cells, and how much floor there is to land on.
constexpr int kRunUp  = 14;
constexpr int kRunOut = 10;

// How many cell rows apart the courses are stacked.
//
// Generous. Each one is two floors and a creature reads the ground a good way above
// and below itself, so courses that are merely adjacent can see each other — and a
// creature that refuses to walk because of a floor belonging to the next test is a
// failure of the probe reported as a failure of the navigator.
constexpr int kApart = 12;

// What counts as head-butting rather than climbing.
constexpr int kSpamAt = 2;

// The clearance a climb needs past the face, matching `nav::plan.cpp`'s own figure.
//
// Measured, not chosen: at 1.4 and 1.8 an ambling boar took two goes at an eighteen
// pixel step and at 2.2 it takes one. The first attempt was landing with its feet on
// the lip, bouncing back, and going again — which is the head-butting this was all
// rewritten to stop, in its smallest form.
constexpr float kLandingRoom = 2.2f;

// Lays one run of cells, and returns the world Y of their top surface.
float Floor(World &world, int fromCell, int toCell, int row) {
    for (int cx = fromCell; cx <= toCell; cx++) world.PlaceCell(Element::Cobblestone, cx, row);

    return World::CellBounds(0, row).y;
}

struct Course {
    Vector2 from{};
    float finish = 0.0f;
};

// A flat run with a hole of `gap` cells in it.
Course Hole(World &world, int row, int gap) {
    const float top = Floor(world, 0, kRunUp, row);

    Floor(world, kRunUp + 1 + gap, kRunUp + 1 + gap + kRunOut, row);

    return {.from = {World::CellBounds(1, row).x, top}, .finish = World::CellBounds(kRunUp + 2 + gap, row).x};
}

// A flat run with a `rise` cell step up in it.
Course Step(World &world, int row, int rise) {
    const float top = Floor(world, 0, kRunUp, row);

    for (int up = 1; up <= rise; up++) Floor(world, kRunUp + 1, kRunUp + 1 + kRunOut, row - up);

    return {.from = {World::CellBounds(1, row).x, top}, .finish = World::CellBounds(kRunUp + 3, row).x};
}

// A flat run with a `fall` cell step down in it.
//
// Its own course because descending has its own rule — walked off, never jumped off —
// and the check for it is the count of jumps rather than whether it got there.
Course Drop(World &world, int row, int fall) {
    const float top = Floor(world, 0, kRunUp, row);

    Floor(world, kRunUp + 1, kRunUp + 1 + kRunOut, row + fall);

    return {.from = {World::CellBounds(1, row).x, top}, .finish = World::CellBounds(kRunUp + 4, row).x};
}

struct Gait {
    const char *name;
    float throttle;
    bool hurrying;
};

// What the planner decided, frame by frame, as a tally.
//
// Printed only when a row fails, and it is what turns "it did not get there" into
// something actionable: a run that is all `Stop` was refused, one that is all `RunUp`
// never got fast enough, one alternating `Climb` and `Walk` is head-butting.
constexpr int kDoings = 11;

struct Tally {
    int doing[kDoings] = {};
};

const char *kDoing[kDoings] = {"walk",  "runup", "climb",   "leap",     "down",
                               "held",  "back",  "blind",   "no-gap",   "no-climb",
                               "no-drop"};

void Show(const Tally &tally) {
    std::printf("        ");

    for (int d = 0; d < kDoings; d++) {
        if (tally.doing[d] == 0) continue;

        std::printf("%s:%d ", kDoing[d], tally.doing[d]);
    }

    std::printf("\n");
}

bool Cross(World &world, const mob::Def &def, const Course &course, const Gait &gait, int &outJumps,
           float &outGot, Tally &tally) {
    body::Body me(def.build, course.from);

    // The one thing that has to be true before anything else is worth running: the
    // course is there. It was not, the first time — the chunks had never been streamed,
    // so every cell went into a chunk that did not exist and every creature stepped off
    // the start into open air. What that produced was a report full of failures with no
    // jumps in it, which reads as a broken navigator rather than as a missing floor.
    if (!world.IsSolidAt({course.from.x, course.from.y + 4.0f})) {
        std::printf("  (no floor under the start — the course was not built)\n");

        outJumps = 0;
        outGot   = 0.0f;

        return false;
    }

    nav::Legs legs;

    int jumps = 0;

    float best = course.from.x;

    const float pace = std::max(1.0f, gait.throttle * (gait.hurrying ? def.build.sprintSpeed : def.build.runSpeed));

    const float patience = std::max(kPatienceLeast, (course.finish - course.from.x) / pace * kPatienceOver);

    for (float t = 0.0f; t < patience; t += kStep) {
        const nav::Step step = nav::Plan(world, def.build, me.Position(), me.Velocity(), me.Grounded(), 1.0f,
                                         gait.throttle, gait.hurrying, true, legs, kStep);

        if (step.jump) jumps++;

        tally.doing[static_cast<int>(step.doing)]++;

        body::Intent wish;

        wish.moveX       = step.moveX;
        wish.jumpPressed = step.jump;
        wish.jumpHeld    = step.hold;

        // The same rule `nav::Advance` uses: a plan that asked for more than the stroll
        // it was given is asking to run, and the pace the climb was judged against has
        // to be the pace the body can reach.
        wish.sprintHeld = gait.hurrying || std::fabs(step.moveX) > gait.throttle + 1e-3f;

        me.Step(wish, world, kStep);

        best = std::max(best, me.Position().x);

        // Fell off the course. Anything below it is open air down to a hillside that is
        // not what is being tested.
        if (me.Position().y > course.from.y + 200.0f) break;

        if (me.Position().x >= course.finish) {
            outJumps = jumps;
            outGot   = me.Position().x - course.from.x;

            return true;
        }
    }

    outJumps = jumps;
    outGot   = best - course.from.x;

    return false;
}

// A frightened creature over broken ground, driven by its own brain.
//
// Everything above tests `nav::Plan` with the direction forced, which is the right way
// to check the arithmetic and cannot see the fault the player actually reported: a
// boar that had just been hit climbed onto a block, **stood perfectly still** until
// the fright wore off, and then went back to grazing. That is a behaviour, and it only
// appears when the brain is the thing being driven.
//
// The course is a step up followed by a step down, which is the shape the complaint
// described. What is measured is the longest stretch the creature spends not moving:
// a frightened animal may hesitate, and it may not stop.
int Flees(World &world, const mob::Def &def, int row) {
    // Up one cell, along, down one cell — and then a wall three cells high, which is
    // past anything the boar can climb.
    //
    // The wall is the half that matters. Without something it actually **refuses**,
    // the creature never turns, the guard that used to freeze it is never reached, and
    // the check passes whatever that guard is set to. Found by setting the old
    // half-second wait back and watching this say nothing.
    const float top = Floor(world, 0, kRunUp, row);

    Floor(world, kRunUp + 1, kRunUp + 6, row - 1);
    Floor(world, kRunUp + 7, kRunUp + 16, row);

    for (int up = 1; up <= 3; up++) Floor(world, kRunUp + 17, kRunUp + 18, row - up);

    const Vector2 from = {World::CellBounds(1, row).x, top};

    if (!world.IsSolidAt({from.x, from.y + 4.0f})) {
        std::printf("\n  (the flight course was not built)\n");

        return 1;
    }

    body::Body me(def.build, from);

    mob::Wits wits;

    wits.seed = 0x9E3779B9u;

    const mob::Brain *brain = mob::brain::Find(def.temper);

    if (brain == nullptr) return 1;

    // Struck from behind, so it runs the way the course goes.
    mob::Sense sense;

    sense.def       = &def;
    sense.world     = &world;
    sense.stung     = true;
    sense.stungFrom = {from.x - 40.0f, from.y};

    float still   = 0.0f;
    float longest = 0.0f;

    int jumps = 0;

    int turns = 0;

    float wasGoing = 1.0f;

    for (float t = 0.0f; t < 9.0f; t += kStep) {
        sense.at       = me.Position();
        sense.velocity = me.Velocity();
        sense.grounded = me.Grounded();
        sense.swimming = me.Swimming();
        sense.quarry   = {from.x - 40.0f, from.y};
        sense.toQuarry = std::fabs(sense.at.x - sense.quarry.x);
        sense.now      = t;
        sense.dt       = kStep;

        const body::Intent wish = brain->Think(sense, wits);

        // The blow lands once. Everything after it is the fright running its course,
        // which is the part that used to end in a creature standing still.
        sense.stung = false;

        if (wish.jumpPressed) jumps++;

        me.Step(wish, world, kStep);

        // Only counted while it is on the ground: a body at the top of an arc is
        // momentarily still sideways and that is not a freeze.
        // Which way it is actually travelling, and how often that changes. A creature
        // that never turned never met the wall, and a check it never reached is a check
        // that passes for the wrong reason.
        if (std::fabs(me.Velocity().x) > 10.0f) {
            const float going = (me.Velocity().x > 0.0f) ? 1.0f : -1.0f;

            if (going != wasGoing) turns++;

            wasGoing = going;
        }

        // Counted only while it is actually frightened, which is the whole of what is
        // being tested.
        //
        // Measured over the whole run first, and the run reported half a second of
        // stillness that turned out to be the animal *resting* — the fright wears off
        // after a few seconds and a calm drifter stands about for up to three of them
        // by design. A check that cannot tell a frozen creature from a grazing one is
        // a check that fails on correct behaviour, which is the fastest way to get a
        // check switched off.
        const bool fleeing = TextIsEqual(brain->Mood(wits), "flee");

        if (fleeing && me.Grounded() && std::fabs(me.Velocity().x) < 4.0f) {
            still += kStep;

            longest = std::max(longest, still);
        } else {
            still = 0.0f;
        }
    }

    // A quarter of a second is a hesitation. Anything past it is the animal that
    // stopped, and the whole point of the section.
    const bool froze = longest > 0.25f;

    // It has to have met the wall, or the check passed without ever running. A test
    // that can be satisfied by not reaching the thing it is testing is worse than no
    // test — it is a green light for a fault.
    const bool met = turns > 0;

    std::printf("\n%s fleeing over a step, a drop and a wall: %d jumps, %d turns, longest still %.2f s%s\n",
                def.name, jumps, turns, longest,
                froze ? "   <-- FROZE" : (met ? "" : "   <-- NEVER REACHED THE WALL"));

    return (froze || !met) ? 1 : 0;
}

// One line of the report, and the whole verdict rule in one place.
void Say(int cells, float px, bool says, bool did, int jumps, const Tally &tally, int &wrong) {
    const bool broke = says && !did;
    const bool spare = !says && did;
    const bool spam  = jumps > kSpamAt;

    const char *note = broke ? "   <-- FELL SHORT" : (spam ? "   <-- SPAMMED" : (spare ? "   spare" : ""));

    std::printf("  %-6d %8.0f %8s %8s %8d%s\n", cells, px, says ? "yes" : "no", did ? "yes" : "no", jumps, note);

    if (broke || spam) {
        Show(tally);

        wrong++;
    }
}

int Run(const probes::Bench &bench) {
    World &world = *bench.world;

    const char *wanted = (bench.argc >= 3) ? bench.argv[2] : nullptr;

    const float over = terrain::Height(0.0f, world.Settings()) - kOverhead;

    int cellX = 0;
    int row   = 0;

    World::ToCell({0.0f, over}, cellX, row);

    // The ground has to be resident before anything can be built into it. A probe that
    // builds has to stream first, exactly as the game does.
    // Tall enough for every course. They are stacked well apart — see `kApart` — so the
    // region has to reach the top of the highest one, and a course built into a chunk
    // that was never streamed is simply not there.
    world.Update({-400.0f, over - 6200.0f, 1400.0f, 6800.0f});

    std::printf("\ncourses built %d cells up, one blank row apart\n", -row);

    const Gait gaits[] = {{"amble", 0.45f, false}, {"bolt", 1.0f, true}};

    int wrong = 0;

    for (int r = 0; r < mob::kinds::Count(); r++) {
        const mob::Def &def = mob::kinds::Of(mob::Kind{r});

        if (wanted != nullptr && !TextIsEqual(wanted, def.name)) continue;

        // Nothing gravity does not act on has a hole to clear or a ledge to climb.
        if (def.build.floats) {
            std::printf("\n%s floats — no course to run\n", def.name);

            continue;
        }

        for (const Gait &gait : gaits) {
            const float pace = gait.throttle * (gait.hurrying ? def.build.sprintSpeed : def.build.runSpeed);

            const nav::Reach reach = nav::Of(def.build, pace, true);

            std::printf("\n%s at a %s: %.0f px/s, jump %.0f px high, %.0f px across, apex at %.2f s\n", def.name,
                        gait.name, pace, reach.rise, reach.gap, reach.apexIn);

            std::printf("  %-6s %8s %8s %8s %8s\n", "gap", "px", "says", "crossed", "jumps");

            for (int gap = 1; gap <= 6; gap++) {
                // Each course on its own row, so one cannot be walked into from
                // another.
                const Course course = Hole(world, row - gap * kApart, gap);

                const float span = static_cast<float>(gap) * kBlockSide;

                // The planner's own rule, deliberately, and not an independent guess at
                // the physics. What is checked is that the *body* does what the planner
                // promised — a second guess here would only be testing the guess.
                const bool says = (span + nav::kProbeStep - def.build.width * 0.5f) <= reach.gap;

                int jumps  = 0;
                float went = 0.0f;

                Tally tally;

                const bool crossed = Cross(world, def, course, gait, jumps, went, tally);

                Say(gap, span, says, crossed, jumps, tally, wrong);
            }

            std::printf("  %-6s %8s %8s %8s %8s\n", "up", "px", "says", "climbed", "jumps");

            for (int rise = 1; rise <= 4; rise++) {
                const Course course = Step(world, row - 100 - rise * kApart, rise);

                const float up = static_cast<float>(rise) * kBlockSide;

                // Two things have to hold for a climb and only one of them is the jump
                // height. The other is that the body is going fast enough to still be
                // over the face while it is up there — `nav::PaceToClimb`, whose
                // absence is what caused the spam.
                const float least = nav::PaceToClimb(def.build, up, def.build.width * 0.5f * kLandingRoom);

                // Against its **sprint** and not the pace it was ambling at, because
                // the planner always runs at a ledge: seeing one is what puts a
                // creature at full pelt, so the pace a climb is judged by is the pace
                // it will have by the time it gets there. Predicting on the stroll
                // marks every ambling climb as `spare`, which is a report crying wolf.
                const bool says = up <= reach.rise && def.build.sprintSpeed >= least;

                int jumps  = 0;
                float went = 0.0f;

                Tally tally;

                const bool climbed = Cross(world, def, course, gait, jumps, went, tally);

                Say(rise, up, says, climbed, jumps, tally, wrong);
            }

            std::printf("  %-6s %8s %8s %8s %8s\n", "down", "px", "says", "walked", "jumps");

            for (int fall = 1; fall <= 4; fall++) {
                const Course course = Drop(world, row - 200 - fall * kApart, fall);

                const float down = static_cast<float>(fall) * kBlockSide;

                const bool says = down <= reach.drop;

                int jumps  = 0;
                float went = 0.0f;

                Tally tally;

                const bool walked = Cross(world, def, course, gait, jumps, went, tally);

                // A descent must cost **no** jumps at all. Going down is walking off,
                // and a jump at a drop throws the body further out than it meant to go
                // and lands it harder — on a staircase it reads as an animal bouncing
                // downhill rather than walking down it.
                if (says && jumps > 0) {
                    std::printf("  %-6d %8.0f %8s %8s %8d   <-- JUMPED DOWNHILL\n", fall, down, "yes",
                                walked ? "yes" : "no", jumps);

                    wrong++;

                    continue;
                }

                Say(fall, down, says, walked, jumps, tally, wrong);
            }
        }

        wrong += Flees(world, def, row - 280);
    }

    std::printf("\n%s\n\n", (wrong == 0) ? "one jump per obstacle, none downhill, and every promise kept"
                                         : "the navigator is wasting jumps or promising ones the body cannot make");

    return (wrong == 0) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--nav",
    .wants = 2,
    .shows = false,
    .blurb = "--nav [mob] - that a creature clears what its legs allow, in one attempt, at every pace",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
