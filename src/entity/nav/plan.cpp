#include "entity/nav/plan.h"

#include "entity/nav/reach.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {

// How far past the lip of a hole a jump may still be started, in probe steps.
//
// The scan reads the ground every `kProbeStep`, so the true lip is somewhere inside
// one step of where it was found. A margin under one would mean the jump is sometimes
// decided a step late, which at a run is a creature walking off the edge it had just
// worked out it could clear.
//
// A hole only. A ledge is timed against the arc rather than against the lip, which is
// the whole of the next section down.
constexpr float kAtTheLip = nav::kProbeStep * 1.2f;

// How long a hole has to be before it is worth leaving the ground for.
//
// Under this the body's own step and snap carry it across without anything being
// asked — `body::Body::SnapToGround` exists exactly for the gravel a terraced
// hillside is made of — and jumping every one of them is a creature that hops
// continuously across flat ground.
constexpr float kWorthAJump = 10.0f;

// How far past the face of a ledge the body has to get before its arc falls back below
// the top, as a multiple of its own half-width.
//
// It lands on its far edge, so clearing the face by nothing at all comes down with its
// feet against the lip and bounces off. Measured with `--nav` rather than chosen: at
// 1.4 and at 1.8 an ambling boar took two goes at an eighteen pixel step, and at 2.2
// it takes one. Two goes is the head-butting this file was rewritten to stop, in its
// smallest and most forgivable form — and it is still worth not doing.
constexpr float kLandingRoom = 2.2f;

// How far a body may travel before its reading of the ground is taken again.
//
// Half a probe step. Under it the scan would return the same columns and the same
// answer; over it a hole could open inside the step and go unseen. The effect is
// self-adjusting and in the right direction: a bolting animal covers this in one frame
// and rescans every frame, while one strolling covers it in six and pays a sixth as
// much — and it is the fast one whose information has to be fresh.
constexpr float kRescanAfter = nav::kProbeStep * 0.5f;

// Below this, a body that is trying to move is not moving.
constexpr float kStalled = 8.0f;

// How long a back-off lasts, in seconds.
//
// Long enough to make room for a run-up and no longer. At a boar's bolt this is about
// forty pixels, which is comfortably more than the thirty-seven a climb needs.
constexpr float kBackOff = 0.28f;

// How near the face counts as being against it, in world pixels.
constexpr float kNoseToFace = 4.0f;

// How many attempts at one ledge before it is given up on.
//
// The arithmetic says whether a jump *can* work on a stair; the ground here is a
// contour, and there are shapes it does not describe — a lip that rounds over, a
// riser with an overhang above it. Without a tally a creature meeting one of those
// jumps at it for ever, which is the fault this whole file was rewritten for.
constexpr int kGiveUpAfter = 3;

// How far apart two attempts have to be to count as being at different ledges.
constexpr float kSameLedge = nav::kProbeStep * 3.0f;

} // namespace

nav::Step nav::Plan(const World &world, const body::Build &build, Vector2 at, Vector2 velocity, bool grounded,
                    float dir, float throttle, bool hurrying, bool committed, Legs &legs, float dt) {
    Step step;

    const float share = std::clamp(throttle, 0.0f, 1.0f);

    step.moveX = dir * share;

    legs.leaping = std::max(0.0f, legs.leaping - dt);
    legs.backing = std::max(0.0f, legs.backing - dt);

    // Nothing gravity acts on. A floating creature goes where it points and there is
    // no arc to plan, no ledge to climb and no hole to clear.
    if (build.floats) return step;

    // Making room for a run-up, and nothing reconsiders it until it is done. The ledge
    // is still there the whole time saying "climb me", which is exactly why this comes
    // before the reading.
    if (legs.backing > 0.0f) {
        step.moveX = -dir;
        step.doing = Step::Doing::Back;

        return step;
    }

    // Mid-leap. The arc is held to its committed length and nothing is re-planned.
    if (legs.leaping > 0.0f) {
        step.hold  = true;
        step.doing = Step::Doing::Held;

        return step;
    }

    // Airborne without having decided to be — walked off something, or knocked. It
    // keeps its direction and takes no new decisions until it is standing again.
    if (!grounded) {
        step.doing = Step::Doing::Held;

        return step;
    }

    const float meant = share * (hurrying ? build.sprintSpeed : build.runSpeed);

    const Reach reach = Of(build, meant, committed);

    // The reading, fresh or shifted. See `Legs`.
    const float moved = std::fabs(at.x - legs.scannedAt.x) + std::fabs(at.y - legs.scannedAt.y);

    const bool stale = !legs.scanned || legs.scannedDir != dir || moved >= kRescanAfter;

    if (stale) {
        legs.ahead      = Read(world, build, reach, at, dir);
        legs.scannedAt  = at;
        legs.scannedDir = dir;
        legs.scanned    = true;
    }

    Ahead ahead = legs.ahead;

    if (!stale) {
        // Shifted by the ground covered since it was taken, so that a lip two pixels
        // nearer than it was reads as two pixels nearer.
        const float along = (at.x - legs.scannedAt.x) * dir;

        if (ahead.toGap >= 0.0f) ahead.toGap = std::max(0.0f, ahead.toGap - along);
        if (ahead.toClimb >= 0.0f) ahead.toClimb = std::max(0.0f, ahead.toClimb - along);
        if (ahead.toDrop >= 0.0f) ahead.toDrop = std::max(0.0f, ahead.toDrop - along);
    }

    if (!ahead.footed) {
        step.doing = Step::Doing::Blind;

        return step;
    }

    const float front = build.width * 0.5f;

    // Whichever of the three is nearest is the one to answer. They are told apart in
    // the reading precisely so that they can have different answers here.
    const float toGap   = (ahead.toGap >= 0.0f) ? ahead.toGap : 1e9f;
    const float toClimb = (ahead.toClimb >= 0.0f) ? ahead.toClimb : 1e9f;
    const float toDrop  = (ahead.toDrop >= 0.0f) ? ahead.toDrop : 1e9f;

    // ---- A hole ------------------------------------------------------------------
    if (toGap <= toClimb && toGap <= toDrop) {
        // Small enough that the body's own stride carries it.
        if (ahead.gapSpan <= kWorthAJump) return step;

        const bool crossable = ahead.gapEnds && (ahead.gapSpan - front) <= reach.gap;

        if (!crossable) {
            // Nowhere to land. Turned away from as soon as it is seen rather than at
            // the lip, so the creature never spends a frame walking towards a hole it
            // has already decided against.
            step.turn  = true;
            step.moveX = 0.0f;
            step.doing = Step::Doing::NoGap;

            return step;
        }

        // At full pelt for the leap itself, whatever it was strolling at. Nothing
        // jumps a hole at a saunter, and this can only overshoot: the gap was judged
        // clearable at the *ambling* pace, so leaving the ground faster lands it
        // further onto the far side rather than short of it.
        if (ahead.toGap - front <= kAtTheLip) {
            step.jump  = true;
            step.hold  = true;
            step.moveX = dir;
            step.doing = Step::Doing::Leap;

            legs.leaping = reach.airtime;
        }

        return step;
    }

    // ---- A ledge -----------------------------------------------------------------
    if (toClimb <= toDrop) {
        if (ahead.climbUp > reach.rise) {
            step.turn  = true;
            step.moveX = 0.0f;
            step.doing = Step::Doing::NoClimb;

            return step;
        }

        // An attempt that changed the floor under the creature worked, whatever else
        // happened afterwards, so the tally starts again.
        //
        // Without this a staircase is mistaken for one ledge tried three times: the
        // treads of a terrace are narrower than `kSameLedge`, so three successful
        // climbs in a row would exhaust the count and the creature would give up
        // halfway up a hill it was climbing perfectly well.
        if (std::fabs(ahead.floor - legs.triedFloor) > build.stepHeight) legs.tries = 0;

        // Given up on. See `kGiveUpAfter`: the arithmetic below says whether a jump
        // *can* work on a stair, and the ground here is not a stair.
        if (legs.tries >= kGiveUpAfter && std::fabs(at.x - legs.triedAt) < kSameLedge) {
            step.turn  = true;
            step.moveX = 0.0f;
            step.doing = Step::Doing::NoClimb;

            return step;
        }

        // Full pelt at it from the moment it is seen, which is the run-up. A creature
        // that kept ambling until the jump and only then sped up would leave the
        // ground at the amble, because a body accelerates over a tenth of a second and
        // the jump is this frame.
        step.moveX = dir;
        step.doing = Step::Doing::RunUp;

        const float gap = ahead.toClimb - front;

        // How fast it is **going**, not how fast it meant to go.
        const float going = std::fabs(velocity.x);

        // The slowest it could be going and still land on top rather than against the
        // face. This is the figure that was missing, and its absence is the whole of
        // the jump-spamming: an ambling boar covers fourteen pixels in its entire arc
        // and cannot climb a terrace at that pace however well the jump is timed.
        const float least = PaceToClimb(build, ahead.climbUp, front * kLandingRoom);

        if (going < least) {
            // Too slow. Keep running at it — and if there is no room left to build up
            // any speed, reverse and take another go.
            if (gap <= kNoseToFace && going < kStalled) {
                legs.backing = kBackOff;
                step.moveX   = -dir;
                step.doing   = Step::Doing::Back;
            }

            return step;
        }

        // And the moment. Arriving at the face at the top of the arc is the choice
        // with the most height to spare and the most room either side of it to be
        // wrong in — see `Reach::apexIn`.
        if (gap <= going * reach.apexIn) {
            step.jump  = true;
            step.hold  = true;
            step.doing = Step::Doing::Climb;

            legs.leaping = reach.airtime;

            legs.tries      = (std::fabs(at.x - legs.triedAt) < kSameLedge) ? legs.tries + 1 : 1;
            legs.triedAt    = at.x;
            legs.triedFloor = ahead.floor;
        }

        return step;
    }

    // ---- A drop ------------------------------------------------------------------
    //
    // Walked off, never jumped off. A jump at a descent throws the body further out
    // than it meant to go and lands it harder, and on a staircase it reads as an
    // animal bouncing downhill rather than walking down it.
    if (ahead.dropDown > reach.drop) {
        step.turn  = true;
        step.moveX = 0.0f;
        step.doing = Step::Doing::NoDrop;

        return step;
    }

    step.doing = Step::Doing::Down;

    return step;
}

body::Intent nav::Advance(const World &world, const body::Build &build, Vector2 at, Vector2 velocity,
                          bool grounded, float dir, float throttle, bool hurrying, bool committed, Legs &legs,
                          float dt, bool &outTurn) {
    const Step step = Plan(world, build, at, velocity, grounded, dir, throttle, hurrying, committed, legs, dt);

    outTurn = step.turn;

    body::Intent intent;

    intent.moveX       = step.moveX;
    intent.jumpPressed = step.jump;
    intent.jumpHeld    = step.hold;

    // Sprinting whenever the plan asked for more than the stroll it was given, so the
    // pace the climb was judged against is the pace the body can actually reach. A
    // creature that planned a run-up and then took it at its walking speed falls short
    // of every ledge it decided it could climb.
    intent.sprintHeld = hurrying || std::fabs(intent.moveX) > std::clamp(throttle, 0.0f, 1.0f) + 1e-3f;

    return intent;
}
