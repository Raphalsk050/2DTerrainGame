#include "player.h"

#include <algorithm>
#include <cmath>

namespace {

// Moves `current` towards `target` by at most `maxDelta`, without overshooting.
float MoveTowards(float current, float target, float maxDelta) {
    if (std::fabs(target - current) <= maxDelta) return target;
    return current + std::copysign(maxDelta, target - current);
}

} // namespace

Player::Player(Vector2 spawn) : position_(spawn) {}

Rectangle Player::BodyRect(Vector2 position, bool crouched) const {
    const float height = crouched ? player_config::kCrouchHeight : player_config::kHeight;

    return {position.x - player_config::kWidth / 2.0f, position.y - height, player_config::kWidth, height};
}

Rectangle Player::Bounds() const {
    return BodyRect(position_, crouched_);
}

Vector2 Player::Centre() const {
    const Rectangle body = Bounds();
    return {body.x + body.width / 2.0f, body.y + body.height / 2.0f};
}

Rectangle Player::AttackHitbox() const {
    if (attackTimer_ <= 0.0f) return {position_.x, position_.y, 0.0f, 0.0f};

    const Vector2 centre = Centre();
    const float half     = player_config::kAttackSize / 2.0f;

    return {centre.x + aimDirection_.x * player_config::kAttackReach - half,
            centre.y + aimDirection_.y * player_config::kAttackReach - half, player_config::kAttackSize,
            player_config::kAttackSize};
}

void Player::UpdateAim(const PlayerInput &input) {
    const Vector2 centre = Centre();
    const Vector2 delta  = {input.aimWorld.x - centre.x, input.aimWorld.y - centre.y};
    const float length   = std::sqrt(delta.x * delta.x + delta.y * delta.y);

    // A cursor resting exactly on the body has no direction to offer, so the
    // previous one is kept rather than snapping to an arbitrary axis.
    if (length > 0.001f) aimDirection_ = {delta.x / length, delta.y / length};

    facing_ = (aimDirection_.x >= 0.0f) ? 1 : -1;
}

bool Player::CanStandUp(const World &terrain) const {
    return !terrain.OverlapsSolid(BodyRect(position_, false));
}

void Player::UpdateTimers(const PlayerInput &input, float dt) {
    coyoteTimer_     = std::max(0.0f, coyoteTimer_ - dt);
    jumpBufferTimer_ = std::max(0.0f, jumpBufferTimer_ - dt);
    attackTimer_     = std::max(0.0f, attackTimer_ - dt);
    attackCooldown_  = std::max(0.0f, attackCooldown_ - dt);

    // The press is recorded even when it cannot be honoured yet; TryJump
    // consumes it once the character is eligible to jump.
    if (input.jumpPressed) jumpBufferTimer_ = player_config::kJumpBuffer;
}

void Player::UpdateStance(const PlayerInput &input, const World &terrain) {
    if (input.crouchHeld && grounded_) {
        crouched_ = true;
        return;
    }

    // Standing up grows the body upwards, so it has to be checked against the
    // terrain first: releasing the key under a low ceiling keeps the crouch.
    if (crouched_ && CanStandUp(terrain)) crouched_ = false;
}

void Player::UpdateAttack(const PlayerInput &input) {
    // Cleared every frame, so it names the frame a swing began rather than the
    // frame the key happened to be read on.
    attackStarted_ = false;

    if (input.attackPressed && attackCooldown_ <= 0.0f) {
        attackTimer_    = player_config::kAttackDuration;
        attackCooldown_ = player_config::kAttackCooldown;
        attackStarted_  = true;
    }
}

void Player::UpdateVelocity(const PlayerInput &input, float dt) {
    // Walking or running on open ground, and neither once the stance or the water
    // has taken the choice away: a crouch is slow because it is a crouch, and a
    // stroke is as fast as a stroke is however hard the key is held.
    float topSpeed = input.sprintHeld ? player_config::kSprintSpeed : player_config::kRunSpeed;

    if (IsSwimming()) topSpeed = player_config::kSwimSpeed;
    if (crouched_) topSpeed = player_config::kCrouchSpeed;

    const float accel       = grounded_ ? player_config::kGroundAccel : player_config::kAirAccel;
    const float targetSpeed = input.moveX * topSpeed;

    velocity_.x = MoveTowards(velocity_.x, targetSpeed, accel * dt);

    // Buoyancy cancels gravity in proportion to how much of the body is under
    // the surface, and overtakes it once mostly submerged, so a body entering
    // the water sinks a little and then rises back to float.
    const float gravity = player_config::kGravity * (1.0f - submerged_ * player_config::kBuoyancy);
    velocity_.y += gravity * dt;

    if (submerged_ > 0.0f) {
        // Applied as exponential decay rather than a subtraction: subtracting
        // drag can overshoot past zero on a long frame and push the body
        // backwards.
        const float damping = std::exp(-player_config::kWaterDrag * submerged_ * dt);
        velocity_.x *= damping;
        velocity_.y *= damping;
    }

    const float maxFall = (submerged_ > 0.0f) ? player_config::kWaterMaxFall : player_config::kMaxFallSpeed;
    velocity_.y         = std::min(velocity_.y, maxFall);
}

void Player::TryJump(const PlayerInput &input) {
    if (IsSwimming()) {
        // There is nothing to push off from, so the jump becomes a stroke that
        // can be held. Taking the minimum keeps a stroke from cancelling speed
        // already gained from a previous one.
        if (input.jumpHeld) velocity_.y = std::min(velocity_.y, -player_config::kSwimStroke);

        // Diving overrides buoyancy for as long as it is held, which is the
        // only way down: a floating body otherwise returns to the surface.
        if (input.crouchHeld) velocity_.y = std::max(velocity_.y, player_config::kSinkStroke);

        jumpBufferTimer_ = 0.0f;
        return;
    }

    const bool canJump = grounded_ || coyoteTimer_ > 0.0f;

    if (jumpBufferTimer_ > 0.0f && canJump && !crouched_) {
        velocity_.y      = -player_config::kJumpSpeed;
        jumpBufferTimer_ = 0.0f;
        coyoteTimer_     = 0.0f;
        grounded_        = false;
    }

    // Screen coordinates grow downwards, so a rising body has negative speed.
    if (!input.jumpHeld && velocity_.y < -player_config::kJumpCutSpeed) {
        velocity_.y = -player_config::kJumpCutSpeed;
    }
}

void Player::Fly(const PlayerInput &input, float dt) {
    const float topSpeed = input.sprintHeld ? player_config::kFlyBoostSpeed : player_config::kFlySpeed;

    // Normalised, so travelling diagonally is not faster than travelling along
    // an axis.
    const float length = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
    const float scale  = (length > 1.0f) ? (1.0f / length) : 1.0f;

    velocity_.x = MoveTowards(velocity_.x, input.moveX * scale * topSpeed, player_config::kFlyAccel * dt);
    velocity_.y = MoveTowards(velocity_.y, input.moveY * scale * topSpeed, player_config::kFlyAccel * dt);

    // Moved outright, with no collision test and no sub-stepping. Passing through
    // the rock is the point: a flight that stopped at a wall could not be used to
    // look at what is behind it.
    position_.x += velocity_.x * dt;
    position_.y += velocity_.y * dt;

    // Neither of these can be true in flight, and leaving the last ground reading
    // standing would have the character land the instant flight is switched off.
    grounded_ = false;
    crouched_ = false;
    state_    = State::Flying;
}

bool Player::StepOver(const World &terrain) {
    // Only from the ground, and only while not rising. A body in mid-air allowed
    // to lift itself out of a collision would climb every wall it brushed against.
    if (!grounded_ || velocity_.y < 0.0f || IsSwimming()) return false;

    // Probed at whole pixels rather than at lattice spacing. The ground is drawn
    // by interpolating between vertices, so its true height is not a multiple of
    // anything, and probing coarsely lifts the body a visible amount higher than
    // the ledge it is stepping onto.
    for (float lift = 1.0f; lift <= player_config::kStepHeight; lift += 1.0f) {
        const Vector2 raised = {position_.x, position_.y - lift};
        if (terrain.OverlapsSolid(BodyRect(raised, crouched_))) continue;

        // The first height that clears is taken, which is the lowest one, so the
        // body ends up on the ledge rather than above it.
        position_ = raised;
        return true;
    }

    return false;
}

void Player::SnapToGround(const World &terrain) {
    for (float drop = 1.0f; drop <= player_config::kSnapDistance; drop += 1.0f) {
        const Vector2 lowered = {position_.x, position_.y + drop};

        // Stopped at rock rather than pushed into it. Reaching solid ground on the
        // way down means the descent has gone one pixel too far, and the position
        // before it was already the answer.
        if (terrain.OverlapsSolid(BodyRect(lowered, crouched_))) return;

        if (terrain.OverlapsSolid(BodyRect({lowered.x, lowered.y + 1.0f}, crouched_))) {
            position_ = lowered;
            return;
        }
    }
}

bool Player::Sidestep(const World &terrain) {
    // Only while rising, and only into a corner. Falling onto a ledge should land
    // on it, not be shunted off the side of it.
    if (velocity_.y >= 0.0f) return false;

    // The direction of travel is tried first at each distance, so a character
    // moving right is carried around the corner rather than back off it. A body
    // going straight up has no preference and takes either.
    const float toward = (velocity_.x >= 0.0f) ? 1.0f : -1.0f;

    for (float nudge = 1.0f; nudge <= player_config::kCornerNudge; nudge += 1.0f) {
        for (const float side : {toward, -toward}) {
            const Vector2 shifted = {position_.x + side * nudge, position_.y};
            if (terrain.OverlapsSolid(BodyRect(shifted, crouched_))) continue;

            position_ = shifted;
            return true;
        }
    }

    return false;
}

bool Player::Unstick(const World &terrain) {
    // Whole pixels outwards, taking the first place that clears, so the body ends
    // up against the face of whatever closed over it rather than a stride clear
    // of it.
    for (float out = 1.0f; out <= player_config::kUnstickReach; out += 1.0f) {
        // Up first, and by a distance rather than in turn with the rest: ground
        // grows from below in this world — a floor laid under the feet, a hill
        // regenerated — so the sky is where the room is, and standing on top of
        // what appeared is what the character would have done had it arrived a
        // moment earlier.
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
            if (terrain.OverlapsSolid(BodyRect(at, crouched_))) continue;

            position_ = at;

            // Whatever speed the body had belonged to a move that never happened.
            // Carrying it out of the ground would fling the character off the
            // block it was just freed from.
            velocity_ = {};

            return true;
        }
    }

    return false;
}

void Player::MoveAndCollide(const World &terrain, float dt) {
    // Freed before anything is asked of the move, because a body inside the
    // ground has no move to make: every direction out of it is refused by the
    // same test that should have let it go.
    if (terrain.OverlapsSolid(Bounds())) Unstick(terrain);

    // Remembered before the move, because whether the body should be held against
    // the ground is a question about where it started, not where it ended up.
    // Walking off a step leaves it airborne by the time the move is over, which is
    // exactly the case the snap exists for.
    const bool wasGrounded = grounded_;

    const Vector2 delta = {velocity_.x * dt, velocity_.y * dt};

    const float maxStep  = terrain.Spacing() / 2.0f;
    const float distance = std::max(std::fabs(delta.x), std::fabs(delta.y));
    const int steps      = std::max(1, static_cast<int>(std::ceil(distance / maxStep)));

    Vector2 step = {delta.x / steps, delta.y / steps};

    for (int s = 0; s < steps; s++) {
        // An axis that has hit something is retired for the rest of the move, and
        // the other one carries on without it.
        //
        // This is the whole of what standing on the ground costs the character,
        // and it used to cost the run itself: a blocked descent left the loop
        // outright, and a body resting on the floor is blocked on its descent
        // every single frame. What that abandoned was every sub-step of the
        // horizontal move after the first — a run of two sub-steps delivered half
        // its distance, one of three delivered a third, and since the count comes
        // from the speed, asking the character to run faster bought almost
        // nothing. The velocity was right the whole time and the body was moving
        // at half of it, which is exactly what walking through glue feels like.
        if (step.x != 0.0f) {
            position_.x += step.x;

            // Blocked sideways, which on this terrain is usually a step rather
            // than a wall. Stopping the body outright is what made running across
            // open ground catch, so the ledge is tried first and only a genuine
            // wall takes the speed away.
            if (terrain.OverlapsSolid(Bounds()) && !StepOver(terrain)) {
                position_.x -= step.x;
                velocity_.x = 0.0f;
                step.x      = 0.0f;
            }
        }

        if (step.y != 0.0f) {
            position_.y += step.y;

            if (terrain.OverlapsSolid(Bounds()) && !Sidestep(terrain)) {
                position_.y -= step.y;
                velocity_.y = 0.0f;
                step.y      = 0.0f;
            }
        }

        // Nothing left to move on either axis, which is what the old loop left
        // early for and the only thing it was right to leave early for.
        if (step.x == 0.0f && step.y == 0.0f) break;
    }

    // Probed separately rather than inferred from the collision above: gravity
    // over a single frame may not close the sub-pixel gap to the ground, and a
    // character standing still would flicker between grounded and airborne.
    grounded_ = terrain.OverlapsSolid(BodyRect({position_.x, position_.y + 1.0f}, crouched_));

    // Held against the ground it was walking on a moment ago, whether it left it
    // by cresting a step or by running down one. Both cases are the same to the
    // character and neither should be a fall.
    if (!grounded_ && wasGrounded && velocity_.y >= 0.0f) {
        SnapToGround(terrain);
        grounded_ = terrain.OverlapsSolid(BodyRect({position_.x, position_.y + 1.0f}, crouched_));

        // The drop was walked down rather than fallen, so the speed gravity had
        // begun to build up over it is not owed to anybody.
        if (grounded_) velocity_.y = 0.0f;
    }

    if (grounded_) coyoteTimer_ = player_config::kCoyoteTime;
}

void Player::UpdateState() {
    if (attackTimer_ > 0.0f) {
        state_ = State::Attacking;
    } else if (IsSwimming()) {
        state_ = State::Swimming;
    } else if (!grounded_) {
        state_ = (velocity_.y < 0.0f) ? State::Jumping : State::Falling;
    } else if (crouched_) {
        state_ = State::Crouching;
    } else if (std::fabs(velocity_.x) > 1.0f) {
        state_ = State::Running;
    } else {
        state_ = State::Idle;
    }
}

void Player::Update(const PlayerInput &input, const World &terrain, float dt) {
    if (input.flyToggled) {
        flying_ = !flying_;

        // Dropped rather than carried across, in both directions: entering flight
        // with a fall already in progress would sink the body, and leaving it at
        // boost speed would fling it across the world.
        velocity_ = {};
    }

    // Measured before anything moves, so buoyancy, drag and the choice between
    // walking and swimming all act on the same reading.
    //
    // Left at zero in flight: the body passes through water as it does through
    // rock, and reading it would put the character into a swimming stroke inside
    // a flooded cavern it is only passing over.
    submerged_ = flying_ ? 0.0f : terrain.SubmergedFraction(Bounds());

    if (flying_) {
        UpdateTimers(input, dt);
        UpdateAim(input);
        UpdateAttack(input);
        Fly(input, dt);
        return;
    }

    UpdateTimers(input, dt);
    UpdateAim(input);
    UpdateStance(input, terrain);
    UpdateAttack(input);
    UpdateVelocity(input, dt);
    TryJump(input);
    MoveAndCollide(terrain, dt);
    UpdateState();
}

void Player::Draw() const {
    const Rectangle body = Bounds();

    DrawRectangleRec(body, flying_ ? Color{150, 110, 200, 255} : IsSwimming() ? DARKBLUE : DARKGREEN);

    // Outlined while flying, so the body stays findable inside the rock it is
    // passing through instead of reading as a patch of odd-coloured stone.
    if (flying_) {
        const Rectangle halo = {body.x - 2.0f, body.y - 2.0f, body.width + 4.0f, body.height + 4.0f};

        DrawRectangleLinesEx(halo, 2.0f, {235, 220, 255, 255});
    }

    // The arm: where the hand is pointing, and the swing it makes getting there.
    //
    // Placeholder art, and it is doing two jobs while there is no sprite to carry
    // the pose. The line is the aim; the arc is the blow.
    //
    // What used to say a blow had been struck was the strike box, drawn in
    // translucent orange — a debug overlay doing an animation's job, and one that
    // stopped being true the moment the blow started landing where the cursor is
    // rather than in front of the body. It is still drawn by the overlay that owns
    // it, under the collider toggle, where a box belongs.
    const Vector2 centre = Centre();

    // How far through the swing the arm is, in [0,1], and nought when nothing is
    // being swung — so the same two lines draw the still arm and the swinging one.
    const float through = (attackTimer_ > 0.0f)
                            ? 1.0f - std::clamp(attackTimer_ / player_config::kAttackDuration, 0.0f, 1.0f)
                            : 1.0f;

    // Raised behind and brought down through the aim. Which way *behind* is depends
    // on which way the hand is pointing: Y grows downward, so the arm is lifted by
    // turning it towards the top of the screen, and that is one way round aiming
    // right and the other aiming left.
    const float side = (aimDirection_.x >= 0.0f) ? -1.0f : 1.0f;
    const float turn = (1.0f - through) * player_config::kSwingArc * side;

    const float sine   = std::sin(turn);
    const float cosine = std::cos(turn);

    const Vector2 arm = {aimDirection_.x * cosine - aimDirection_.y * sine,
                         aimDirection_.x * sine + aimDirection_.y * cosine};

    const Vector2 muzzle = {centre.x + arm.x * player_config::kAttackReach,
                            centre.y + arm.y * player_config::kAttackReach};

    DrawLineEx(centre, muzzle, 2.0f, DARKBROWN);
    DrawCircleV(muzzle, 2.0f, DARKBROWN);
}
