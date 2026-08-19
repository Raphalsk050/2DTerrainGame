#include "entity/player/player.h"

#include "world/world.h"

#include <algorithm>
#include <cmath>

Player::Player(Vector2 spawn) : body_(player_config::Build(), spawn) {}

Rectangle Player::AttackHitbox() const {
    if (attackTimer_ <= 0.0f) return {};

    const Vector2 centre = Centre();

    const Vector2 middle = {centre.x + aimDirection_.x * player_config::kAttackReach,
                            centre.y + aimDirection_.y * player_config::kAttackReach};

    return {middle.x - player_config::kAttackSize / 2.0f, middle.y - player_config::kAttackSize / 2.0f,
            player_config::kAttackSize, player_config::kAttackSize};
}

void Player::UpdateAim(const PlayerInput &input) {
    const Vector2 centre = Centre();
    const Vector2 delta  = {input.aimWorld.x - centre.x, input.aimWorld.y - centre.y};
    const float length   = std::sqrt(delta.x * delta.x + delta.y * delta.y);

    // A cursor resting exactly on the body has no direction to offer, so the previous
    // one is kept rather than snapping to an arbitrary axis.
    if (length > 0.001f) aimDirection_ = {delta.x / length, delta.y / length};

    facing_ = (aimDirection_.x >= 0.0f) ? 1 : -1;
}

void Player::UpdateAttack(const PlayerInput &input, float dt) {
    attackTimer_    = std::max(0.0f, attackTimer_ - dt);
    attackCooldown_ = std::max(0.0f, attackCooldown_ - dt);

    // Cleared every frame, so it names the frame a swing began rather than the frame
    // the key happened to be read on.
    attackStarted_ = false;

    if (input.attackPressed && attackCooldown_ <= 0.0f) {
        attackTimer_    = player_config::kAttackDuration;
        attackCooldown_ = player_config::kAttackCooldown;
        attackStarted_  = true;
    }
}

bool Player::Hurt(const life::Blow &blow) {
    // Nothing touches a body that is passing through the rock. Free flight suspends
    // every other rule that would keep the character out of the world; being hit by
    // something it is flying through would be the one that got through.
    if (body_.Ghost()) return false;

    if (!health_.Hurt(blow.damage)) return false;

    const float away = (blow.from.x > Centre().x) ? -1.0f : 1.0f;

    body_.Shove({away * blow.knock, -blow.lift});

    return true;
}

void Player::UpdateState() {
    if (body_.Ghost()) {
        state_ = State::Flying;

        return;
    }

    if (attackTimer_ > 0.0f) {
        state_ = State::Attacking;
    } else if (IsSwimming()) {
        state_ = State::Swimming;
    } else if (!body_.Grounded()) {
        state_ = (body_.Velocity().y < 0.0f) ? State::Jumping : State::Falling;
    } else if (body_.Crouched()) {
        state_ = State::Crouching;
    } else if (std::fabs(body_.Velocity().x) > 1.0f) {
        state_ = State::Running;
    } else {
        state_ = State::Idle;
    }
}

void Player::Update(const PlayerInput &input, const World &terrain, float dt) {
    if (input.flyToggled) body_.SetGhost(!body_.Ghost());

    health_.Tick(dt);

    UpdateAim(input);
    UpdateAttack(input, dt);

    // The whole of the walk, in one call, and exactly the call a boar makes. See
    // `body::Body::Step` for the order inside it and why that order is load-bearing.
    body_.Step(input.motion, terrain, dt);

    UpdateState();
}

void Player::Draw() const {
    const Rectangle body = Bounds();

    const bool flying = body_.Ghost();

    DrawRectangleRec(body, flying ? Color{150, 110, 200, 255} : IsSwimming() ? DARKBLUE : DARKGREEN);

    // Flashed on being hit, over the body rather than as a tint of it: the character
    // is a flat rectangle until there is a sprite, and multiplying a flat colour by a
    // red says almost nothing. See `figure::Draw` for the creature's version, which
    // can tint because it has tones to tint.
    if (health_.Stung()) DrawRectangleRec(body, Fade(RED, 0.55f));

    // Outlined while flying, so the body stays findable inside the rock it is passing
    // through instead of reading as a patch of odd-coloured stone.
    if (flying) {
        const Rectangle halo = {body.x - 2.0f, body.y - 2.0f, body.width + 4.0f, body.height + 4.0f};

        DrawRectangleLinesEx(halo, 2.0f, {235, 220, 255, 255});
    }

    // The arm: where the hand is pointing, and the swing it makes getting there.
    //
    // Placeholder art, and it is doing two jobs while there is no sprite to carry the
    // pose. The line is the aim; the arc is the blow.
    //
    // What used to say a blow had been struck was the strike box, drawn in translucent
    // orange — a debug overlay doing an animation's job, and one that stopped being
    // true the moment the blow started landing where the cursor is rather than in
    // front of the body. It is still drawn by the overlay that owns it, under the
    // collider toggle, where a box belongs.
    const Vector2 centre = Centre();

    // How far through the swing the arm is, in [0,1], and nought when nothing is being
    // swung — so the same two lines draw the still arm and the swinging one.
    const float through = (attackTimer_ > 0.0f)
                            ? 1.0f - std::clamp(attackTimer_ / player_config::kAttackDuration, 0.0f, 1.0f)
                            : 1.0f;

    // Raised behind and brought down through the aim. Which way *behind* is depends on
    // which way the hand is pointing: Y grows downward, so the arm is lifted by turning
    // it towards the top of the screen, and that is one way round aiming right and the
    // other aiming left.
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
