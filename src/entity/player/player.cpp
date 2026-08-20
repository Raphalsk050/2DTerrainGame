#include "entity/player/player.h"

#include "save/record.h"

#include "item/icon.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {

// Where along the canvas the hand closes on the haft, as a share of it from the top.
//
// Not the very bottom. Every one of these is drawn with the butt of the haft on the last
// row of the canvas, so a grip at 1.0 is a tool balanced on a fingertip — and it looked
// like one: at 0.86 there were three pixels of handle behind the fist and the whole tool
// read as floating off the end of the arm rather than as being carried by it. Under three
// quarters there is a visible stub of haft behind the hand, which is what a fist closed
// round a handle looks like.
//
// Here rather than beside `player_config::kHeldTool`, and the split is the point: how
// long the tool is is a fact about the character's reach and half of what frames it, and
// where the fist closes on it is a fact about how these particular files were drawn.
constexpr float kGrip = 0.72f;

} // namespace

Player::Player(Vector2 spawn) : body_(player_config::Build(), spawn) {}

void Player::Save(save::Writer &out) const {
    const Vector2 at = body_.Position();

    out.Tag("player").Real(at.x).Real(at.y).Int(health_.now).Int(health_.most).Done();
}

void Player::Load(save::Reader &in) {
    const float x = in.Real();
    const float y = in.Real();

    const long long now  = in.Int();
    const long long most = in.Int();

    if (!in.Ok()) return;

    // Through `PlaceAt`, which clears the momentum with it. A character put down
    // carrying the speed of the fall it was in the middle of is a character that
    // arrives underground — the same reason the console's teleport goes this way.
    body_.PlaceAt({x, y});

    health_.most = (most > 0) ? static_cast<int>(most) : player_config::kHealth;
    health_.now  = std::clamp(static_cast<int>(now), 0, health_.most);

    // Neither timer is saved and both are cleared. They are seconds of "was just hit",
    // which is a thing that happened rather than a thing that is true — and a world
    // loaded into a flash the player never saw is a world that starts by lying.
    health_.flashFor = 0.0f;
    health_.mercyFor = 0.0f;
}

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

void Player::Draw(const Stack &held) const {
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

    DrawHeld(held, muzzle, arm);
}

void Player::DrawHeld(const Stack &held, Vector2 hand, Vector2 arm) const {
    if (held.Empty() || held.holds != Holds::Item) return;

    // Only what was drawn as a thing. A material in the hand is a fistful of hillside
    // and there is no picture of one being carried; the six-texel icon in the bar is a
    // label rather than an object, and hanging it off the wrist would read as the
    // character waving a swatch.
    const Texture2D *art = icon::Art(Def(held.AsItem()));

    if (art == nullptr) return;

    // Along the arm, head first, because a tool is what the arm ends in. The art is
    // drawn upright — head at the top of the canvas, butt of the haft at the bottom —
    // so what has to happen is that the canvas's own up becomes the direction the arm
    // is pointing.
    //
    // Up is (0,-1) and a turn of `angle` clockwise takes it to (sin, -cos), so the
    // angle wanted is the one whose sine is the arm's x and whose cosine is minus its
    // y. Rotating about the grip rather than the centre is what keeps the hand on the
    // haft through the whole of the swing.
    const float angle = std::atan2(arm.x, -arm.y) * RAD2DEG;

    Rectangle source = {0.0f, 0.0f, static_cast<float>(art->width), static_cast<float>(art->height)};

    // Mirrored by which way the character is facing, and it is a mirror across the haft
    // rather than across the screen — which is what makes it the right thing to do to a
    // tool that is already pointing wherever the cursor is. An axe is all on one side of
    // its own handle, and without this the blade is above the shaft aiming one way and
    // below it aiming the other.
    if (facing_ < 0) source.width = -source.width;

    const Rectangle where = {hand.x, hand.y, player_config::kHeldTool, player_config::kHeldTool};

    const Vector2 grip = {player_config::kHeldTool * 0.5f, player_config::kHeldTool * kGrip};

    DrawTexturePro(*art, source, where, grip, angle, WHITE);
}
