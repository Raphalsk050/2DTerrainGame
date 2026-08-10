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
    if (input.attackPressed && attackCooldown_ <= 0.0f) {
        attackTimer_    = player_config::kAttackDuration;
        attackCooldown_ = player_config::kAttackCooldown;
    }
}

void Player::UpdateVelocity(const PlayerInput &input, float dt) {
    const float topSpeed    = crouched_ ? player_config::kCrouchSpeed : player_config::kRunSpeed;
    const float accel       = grounded_ ? player_config::kGroundAccel : player_config::kAirAccel;
    const float targetSpeed = input.moveX * topSpeed;

    velocity_.x = MoveTowards(velocity_.x, targetSpeed, accel * dt);

    velocity_.y = std::min(velocity_.y + player_config::kGravity * dt, player_config::kMaxFallSpeed);
}

void Player::TryJump(const PlayerInput &input) {
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

void Player::MoveAndCollide(const World &terrain, float dt) {
    const Vector2 delta = {velocity_.x * dt, velocity_.y * dt};

    const float maxStep  = terrain.Spacing() / 2.0f;
    const float distance = std::max(std::fabs(delta.x), std::fabs(delta.y));
    const int steps      = std::max(1, static_cast<int>(std::ceil(distance / maxStep)));

    const Vector2 step = {delta.x / steps, delta.y / steps};

    for (int s = 0; s < steps; s++) {
        position_.x += step.x;
        if (terrain.OverlapsSolid(Bounds())) {
            position_.x -= step.x;
            velocity_.x = 0.0f;
        }

        position_.y += step.y;
        if (terrain.OverlapsSolid(Bounds())) {
            position_.y -= step.y;
            velocity_.y = 0.0f;
            break;
        }
    }

    // Probed separately rather than inferred from the collision above: gravity
    // over a single frame may not close the sub-pixel gap to the ground, and a
    // character standing still would flicker between grounded and airborne.
    grounded_ = terrain.OverlapsSolid(BodyRect({position_.x, position_.y + 1.0f}, crouched_));

    if (grounded_) coyoteTimer_ = player_config::kCoyoteTime;
}

void Player::UpdateState() {
    if (attackTimer_ > 0.0f) {
        state_ = State::Attacking;
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
    DrawRectangleRec(body, DARKGREEN);

    // Aim indicator, readable while there is no sprite to carry the pose.
    const Vector2 centre = Centre();
    const Vector2 muzzle = {centre.x + aimDirection_.x * player_config::kAttackReach,
                            centre.y + aimDirection_.y * player_config::kAttackReach};

    DrawLineEx(centre, muzzle, 2.0f, DARKBROWN);
    DrawCircleV(muzzle, 2.0f, DARKBROWN);

    if (attackTimer_ > 0.0f) DrawRectangleRec(AttackHitbox(), Fade(ORANGE, 0.6f));
}
