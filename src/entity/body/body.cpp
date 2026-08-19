#include "entity/body/body.h"

#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {

// Moves `current` towards `target` by at most `maxDelta`, without overshooting.
float MoveTowards(float current, float target, float maxDelta) {
    if (std::fabs(target - current) <= maxDelta) return target;
    return current + std::copysign(maxDelta, target - current);
}

} // namespace

Rectangle body::Body::RectAt(Vector2 at, bool crouched) const {
    const float height = crouched ? build_.crouchHeight : build_.height;

    return {at.x - build_.width / 2.0f, at.y - height, build_.width, height};
}

Vector2 body::Body::Centre() const {
    const Rectangle box = Bounds();
    return {box.x + box.width / 2.0f, box.y + box.height / 2.0f};
}

bool body::Body::CanStandUp(const World &world) const {
    return !world.OverlapsSolid(RectAt(position_, false));
}

void body::Body::ReadStance(const Intent &intent, const World &world) {
    if (intent.crouchHeld && grounded_) {
        crouched_ = true;
        return;
    }

    // Standing up grows the body upwards, so it has to be checked against the
    // terrain first: releasing the key under a low ceiling keeps the crouch.
    if (crouched_ && CanStandUp(world)) crouched_ = false;
}

void body::Body::ReadSpeed(const Intent &intent, float dt) {
    // Walking or running on open ground, and neither once the stance or the water
    // has taken the choice away: a crouch is slow because it is a crouch, and a
    // stroke is as fast as a stroke is however hard the key is held.
    float topSpeed = intent.sprintHeld ? build_.sprintSpeed : build_.runSpeed;

    if (Swimming()) topSpeed = build_.swimSpeed;
    if (crouched_) topSpeed = build_.crouchSpeed;

    const float accel       = grounded_ ? build_.groundAccel : build_.airAccel;
    const float targetSpeed = intent.moveX * topSpeed;

    velocity_.x = MoveTowards(velocity_.x, targetSpeed, accel * dt);

    // Buoyancy cancels gravity in proportion to how much of the body is under the
    // surface, and overtakes it once mostly submerged, so a body entering the
    // water sinks a little and then rises back to float.
    const float gravity = build_.gravity * (1.0f - submerged_ * build_.buoyancy);
    velocity_.y += gravity * dt;

    if (submerged_ > 0.0f) {
        // Applied as exponential decay rather than a subtraction: subtracting drag
        // can overshoot past zero on a long frame and push the body backwards.
        const float damping = std::exp(-build_.waterDrag * submerged_ * dt);
        velocity_.x *= damping;
        velocity_.y *= damping;
    }

    const float maxFall = (submerged_ > 0.0f) ? build_.waterMaxFall : build_.maxFallSpeed;
    velocity_.y         = std::min(velocity_.y, maxFall);
}

void body::Body::TryJump(const Intent &intent) {
    if (Swimming()) {
        // There is nothing to push off from, so the jump becomes a stroke that can
        // be held. Taking the minimum keeps a stroke from cancelling speed already
        // gained from a previous one.
        if (intent.jumpHeld) velocity_.y = std::min(velocity_.y, -build_.swimStroke);

        // Diving overrides buoyancy for as long as it is held, which is the only
        // way down: a floating body otherwise returns to the surface.
        if (intent.crouchHeld) velocity_.y = std::max(velocity_.y, build_.sinkStroke);

        jumpBufferTimer_ = 0.0f;
        return;
    }

    const bool canJump = grounded_ || coyoteTimer_ > 0.0f;

    if (jumpBufferTimer_ > 0.0f && canJump && !crouched_) {
        velocity_.y      = -build_.jumpSpeed;
        jumpBufferTimer_ = 0.0f;
        coyoteTimer_     = 0.0f;
        grounded_        = false;
    }

    // Screen coordinates grow downwards, so a rising body has negative speed.
    if (!intent.jumpHeld && velocity_.y < -build_.jumpCutSpeed) {
        velocity_.y = -build_.jumpCutSpeed;
    }
}

// A creature gravity does not act on. It steers in both axes and it still
// collides with everything, which is the whole difference between this and the
// free flight below.
void body::Body::Drift(const Intent &intent, float dt) {
    // Normalised, so travelling diagonally is not faster than travelling along an
    // axis.
    const float length = std::sqrt(intent.moveX * intent.moveX + intent.moveY * intent.moveY);
    const float scale  = (length > 1.0f) ? (1.0f / length) : 1.0f;

    velocity_.x = MoveTowards(velocity_.x, intent.moveX * scale * build_.floatSpeed, build_.floatAccel * dt);
    velocity_.y = MoveTowards(velocity_.y, intent.moveY * scale * build_.floatSpeed, build_.floatAccel * dt);
}

// Free flight, which replaces the whole of the walk: velocity follows the wish in
// both axes and the body moves without being asked what it is moving through.
void body::Body::Soar(const Intent &intent, float dt) {
    const float topSpeed = intent.sprintHeld ? build_.flyBoostSpeed : build_.flySpeed;

    // Normalised, so travelling diagonally is not faster than travelling along an
    // axis.
    const float length = std::sqrt(intent.moveX * intent.moveX + intent.moveY * intent.moveY);
    const float scale  = (length > 1.0f) ? (1.0f / length) : 1.0f;

    velocity_.x = MoveTowards(velocity_.x, intent.moveX * scale * topSpeed, build_.flyAccel * dt);
    velocity_.y = MoveTowards(velocity_.y, intent.moveY * scale * topSpeed, build_.flyAccel * dt);

    // Moved outright, with no collision test and no sub-stepping. Passing through
    // the rock is the point: a flight that stopped at a wall could not be used to
    // look at what is behind it.
    position_.x += velocity_.x * dt;
    position_.y += velocity_.y * dt;

    // Neither of these can be true in flight, and leaving the last ground reading
    // standing would have the body land the instant flight is switched off.
    grounded_ = false;
    crouched_ = false;
}

bool body::Body::StepOver(const World &world) {
    // Only from the ground, and only while not rising. A body in mid-air allowed
    // to lift itself out of a collision would climb every wall it brushed against.
    if (!grounded_ || velocity_.y < 0.0f || Swimming()) return false;

    // Probed at whole pixels rather than at lattice spacing. The ground is drawn
    // by interpolating between vertices, so its true height is not a multiple of
    // anything, and probing coarsely lifts the body a visible amount higher than
    // the ledge it is stepping onto.
    for (float lift = 1.0f; lift <= build_.stepHeight; lift += 1.0f) {
        const Vector2 raised = {position_.x, position_.y - lift};
        if (world.OverlapsSolid(RectAt(raised, crouched_))) continue;

        // The first height that clears is taken, which is the lowest one, so the
        // body ends up on the ledge rather than above it.
        position_ = raised;
        return true;
    }

    return false;
}

void body::Body::SnapToGround(const World &world) {
    for (float drop = 1.0f; drop <= build_.snapDistance; drop += 1.0f) {
        const Vector2 lowered = {position_.x, position_.y + drop};

        // Stopped at rock rather than pushed into it. Reaching solid ground on the
        // way down means the descent has gone one pixel too far, and the position
        // before it was already the answer.
        if (world.OverlapsSolid(RectAt(lowered, crouched_))) return;

        if (world.OverlapsSolid(RectAt({lowered.x, lowered.y + 1.0f}, crouched_))) {
            position_ = lowered;
            return;
        }
    }
}

bool body::Body::Sidestep(const World &world) {
    // Only while rising, and only into a corner. Falling onto a ledge should land
    // on it, not be shunted off the side of it.
    if (velocity_.y >= 0.0f) return false;

    // The direction of travel is tried first at each distance, so a body moving
    // right is carried around the corner rather than back off it. One going
    // straight up has no preference and takes either.
    const float toward = (velocity_.x >= 0.0f) ? 1.0f : -1.0f;

    for (float nudge = 1.0f; nudge <= build_.cornerNudge; nudge += 1.0f) {
        for (const float side : {toward, -toward}) {
            const Vector2 shifted = {position_.x + side * nudge, position_.y};
            if (world.OverlapsSolid(RectAt(shifted, crouched_))) continue;

            position_ = shifted;
            return true;
        }
    }

    return false;
}

bool body::Body::Unstick(const World &world) {
    // Whole pixels outwards, taking the first place that clears, so the body ends
    // up against the face of whatever closed over it rather than a stride clear of
    // it.
    for (float out = 1.0f; out <= build_.unstickReach; out += 1.0f) {
        // Up first, and by a distance rather than in turn with the rest: ground
        // grows from below in this world — a floor laid under the feet, a hill
        // regenerated — so the sky is where the room is, and standing on top of
        // what appeared is what the body would have done had it arrived a moment
        // earlier.
        //
        // Then sideways, then down, which is the order of how much of a surprise
        // each one is to somebody watching.
        const Vector2 tries[] = {
            {position_.x, position_.y - out},
            {position_.x - out, position_.y},
            {position_.x + out, position_.y},
            {position_.x, position_.y + out},
        };

        for (const Vector2 &at : tries) {
            if (world.OverlapsSolid(RectAt(at, crouched_))) continue;

            position_ = at;

            // Whatever speed the body had belonged to a move that never happened.
            // Carrying it out of the ground would fling it off the block it was
            // just freed from.
            velocity_ = {};

            return true;
        }
    }

    return false;
}

void body::Body::MoveAndCollide(const World &world, float dt) {
    // Freed before anything is asked of the move, because a body inside the ground
    // has no move to make: every direction out of it is refused by the same test
    // that should have let it go.
    if (world.OverlapsSolid(Bounds())) Unstick(world);

    // Remembered before the move, because whether the body should be held against
    // the ground is a question about where it started, not where it ended up.
    // Walking off a step leaves it airborne by the time the move is over, which is
    // exactly the case the snap exists for.
    const bool wasGrounded = grounded_;

    const Vector2 delta = {velocity_.x * dt, velocity_.y * dt};

    const float maxStep  = world.Spacing() / 2.0f;
    const float distance = std::max(std::fabs(delta.x), std::fabs(delta.y));
    const int steps      = std::max(1, static_cast<int>(std::ceil(distance / maxStep)));

    Vector2 step = {delta.x / steps, delta.y / steps};

    for (int s = 0; s < steps; s++) {
        // An axis that has hit something is retired for the rest of the move, and
        // the other one carries on without it.
        //
        // This is the whole of what standing on the ground costs, and it used to
        // cost the run itself: a blocked descent left the loop outright, and a body
        // resting on the floor is blocked on its descent every single frame. What
        // that abandoned was every sub-step of the horizontal move after the first
        // — a run of two sub-steps delivered half its distance, one of three
        // delivered a third, and since the count comes from the speed, asking for
        // more speed bought almost nothing. The velocity was right the whole time
        // and the body was moving at half of it, which is exactly what walking
        // through glue feels like.
        if (step.x != 0.0f) {
            position_.x += step.x;

            // Blocked sideways, which on this terrain is usually a step rather than
            // a wall. Stopping the body outright is what made running across open
            // ground catch, so the ledge is tried first and only a genuine wall
            // takes the speed away.
            if (world.OverlapsSolid(Bounds()) && !StepOver(world)) {
                position_.x -= step.x;
                velocity_.x = 0.0f;
                step.x      = 0.0f;
            }
        }

        if (step.y != 0.0f) {
            position_.y += step.y;

            if (world.OverlapsSolid(Bounds()) && !Sidestep(world)) {
                position_.y -= step.y;
                velocity_.y = 0.0f;
                step.y      = 0.0f;
            }
        }

        // Nothing left to move on either axis, which is what the old loop left
        // early for and the only thing it was right to leave early for.
        if (step.x == 0.0f && step.y == 0.0f) break;
    }

    // Probed separately rather than inferred from the collision above: gravity over
    // a single frame may not close the sub-pixel gap to the ground, and a body
    // standing still would flicker between grounded and airborne.
    grounded_ = world.OverlapsSolid(RectAt({position_.x, position_.y + 1.0f}, crouched_));

    // Held against the ground it was walking on a moment ago, whether it left it by
    // cresting a step or by running down one. Both cases are the same to the body
    // and neither should be a fall.
    if (!grounded_ && wasGrounded && velocity_.y >= 0.0f) {
        SnapToGround(world);
        grounded_ = world.OverlapsSolid(RectAt({position_.x, position_.y + 1.0f}, crouched_));

        // The drop was walked down rather than fallen, so the speed gravity had
        // begun to build up over it is not owed to anybody.
        if (grounded_) velocity_.y = 0.0f;
    }

    if (grounded_) coyoteTimer_ = build_.coyoteTime;
}

void body::Body::Step(const Intent &intent, const World &world, float dt) {
    coyoteTimer_     = std::max(0.0f, coyoteTimer_ - dt);
    jumpBufferTimer_ = std::max(0.0f, jumpBufferTimer_ - dt);

    // The press is recorded even when it cannot be honoured yet; TryJump consumes
    // it once the body is eligible to jump.
    if (intent.jumpPressed) jumpBufferTimer_ = build_.jumpBuffer;

    // Measured before anything moves, so buoyancy, drag and the choice between
    // walking and swimming all act on the same reading.
    //
    // Left at zero in free flight: the body passes through water as it does
    // through rock, and reading it would put a swimming stroke on something that
    // is only passing over a flooded cavern.
    submerged_ = ghost_ ? 0.0f : world.SubmergedFraction(Bounds());

    if (ghost_) {
        Soar(intent, dt);
        return;
    }

    ReadStance(intent, world);

    if (build_.floats) {
        Drift(intent, dt);
    } else {
        ReadSpeed(intent, dt);
        TryJump(intent);
    }

    MoveAndCollide(world, dt);
}
