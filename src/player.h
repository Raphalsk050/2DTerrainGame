#pragma once

#include "raylib.h"
#include "world.h"

// Tuning values for the player character. They describe the character rather
// than an instance of it, so they live here instead of as members.
namespace player_config {

// Body dimensions in pixels. The crouched body is shorter but just as wide.
inline constexpr float kWidth        = 12.0f;
inline constexpr float kHeight       = 26.0f;
inline constexpr float kCrouchHeight = 14.0f;

inline constexpr float kRunSpeed    = 220.0f; // Pixels per second.
inline constexpr float kCrouchSpeed = 90.0f;
inline constexpr float kGroundAccel = 2200.0f; // Pixels per second squared.
inline constexpr float kAirAccel    = 1200.0f; // Reduced control while airborne.

inline constexpr float kGravity      = 1600.0f;
inline constexpr float kMaxFallSpeed = 900.0f;

// Upward launch speed. Peak height is kJumpSpeed^2 / (2 * kGravity).
inline constexpr float kJumpSpeed = 480.0f;

// Releasing the jump button mid-rise clamps the remaining upward speed to this,
// giving a short hop for a tap and a full arc for a hold. Clamping rather than
// scaling keeps the result the same however many frames the button stays up.
inline constexpr float kJumpCutSpeed = 190.0f;

// A jump still fires this long after walking off a ledge.
inline constexpr float kCoyoteTime = 0.10f;
// A jump pressed this long before landing is remembered and fires on contact.
inline constexpr float kJumpBuffer = 0.12f;

// Window during which the attack hitbox deals damage, and the delay before the
// next attack may start. They are separate so a short strike can still have a
// long recovery.
inline constexpr float kAttackDuration = 0.15f;
inline constexpr float kAttackCooldown = 0.35f;

// Distance from the body centre to the centre of the strike box, and the side
// of that box. The box stays axis aligned whatever the aim angle, which is an
// approximation of a swing but keeps the hit test a rectangle intersection.
inline constexpr float kAttackReach = 22.0f;
inline constexpr float kAttackSize  = 18.0f;

} // namespace player_config

// Per-frame input snapshot. Keeping Player independent of the keyboard allows
// remapping, replays and tests that run without a window.
struct PlayerInput {
    float moveX        = 0.0f; // -1 left, +1 right.
    Vector2 aimWorld   = {};   // Aim target in world space, usually the cursor.
    bool jumpPressed   = false;
    bool jumpHeld      = false; // Held controls jump height, pressed triggers it.
    bool crouchHeld    = false;
    bool attackPressed = false;
};

// Platform character colliding against the terrain grid.
class Player {
public:
    enum class State { Idle, Running, Jumping, Falling, Crouching, Attacking };

    explicit Player(Vector2 spawn);

    void Update(const PlayerInput &input, const World &terrain, float dt);
    void Draw() const;

    // Body in world space. Shrinks vertically while crouching.
    Rectangle Bounds() const;

    // Damage region of the current attack. Width is zero while inactive, so
    // callers can intersect it unconditionally.
    Rectangle AttackHitbox() const;

    // Centre of the body, which is what the character aims from.
    Vector2 Centre() const;

    Vector2 Position() const { return position_; }
    Vector2 AimDirection() const { return aimDirection_; }
    State CurrentState() const { return state_; }
    bool IsGrounded() const { return grounded_; }
    bool IsAttacking() const { return attackTimer_ > 0.0f; }
    int Facing() const { return facing_; }

private:
    // Body rectangle the character would occupy at an arbitrary position and
    // stance, used to test moves before committing to them.
    Rectangle BodyRect(Vector2 position, bool crouched) const;

    void UpdateTimers(const PlayerInput &input, float dt);
    void UpdateStance(const PlayerInput &input, const World &terrain);
    void UpdateAim(const PlayerInput &input);
    void UpdateAttack(const PlayerInput &input);
    void UpdateVelocity(const PlayerInput &input, float dt);
    void TryJump(const PlayerInput &input);

    // Movement resolves one axis at a time, horizontal then vertical. Moving
    // both at once cannot tell which axis caused an overlap, and the body
    // catches on flat ground it should slide along.
    //
    // The move is split into sub-steps no longer than half a grid cell so that
    // a fast fall cannot pass through thin ground between two frames.
    void MoveAndCollide(const World &terrain, float dt);

    bool CanStandUp(const World &terrain) const;
    void UpdateState();

    // Anchored at the feet: bottom edge, horizontally centred. Ground contact
    // and crouching both act on the bottom, so the anchor stays put when the
    // body height changes.
    Vector2 position_{};
    Vector2 velocity_{};

    State state_ = State::Falling;

    // Unit vector towards the aim target. Facing follows its horizontal sign,
    // so the character turns with the cursor rather than with the movement key.
    Vector2 aimDirection_ = {1.0f, 0.0f};
    int facing_           = 1;
    bool grounded_        = false;
    bool crouched_        = false;

    float coyoteTimer_     = 0.0f;
    float jumpBufferTimer_ = 0.0f;

    float attackTimer_    = 0.0f;
    float attackCooldown_ = 0.0f;

    int health_ = 100;
};
