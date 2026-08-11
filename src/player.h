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

// Height of a ledge the character walks over instead of being stopped by.
//
// The ground is sampled on a six pixel lattice and terraced on top of that, so
// small steps are everywhere — a hillside is a staircase and a cave floor is
// gravel. Without this the body is stopped dead by every one of them, and running
// across open ground is a series of snags rather than a run. Set a little above
// the terrace riser, so a ledge meant to be walked up is walked up.
inline constexpr float kStepHeight = 14.0f;

// How far the body is pulled back down onto the ground after it leaves it while
// still walking.
//
// Running downhill otherwise separates the body from the floor for a frame at
// every step down, and the character reads as skipping. Kept to about the step
// height, so what it follows is ground and not a cliff the character should be
// falling off.
inline constexpr float kSnapDistance = 14.0f;

// How far sideways the body may be nudged to clear a corner its head caught on.
//
// A jump made flush against the edge of a ledge otherwise stops on a single solid
// vertex, which is unreadable: nothing visible was in the way. Under half the body
// width, so it can never move the character somewhere it could not already stand.
inline constexpr float kCornerNudge = 5.0f;

// A jump still fires this long after walking off a ledge.
inline constexpr float kCoyoteTime = 0.10f;
// A jump pressed this long before landing is remembered and fires on contact.
inline constexpr float kJumpBuffer = 0.12f;

// Window during which the attack hitbox deals damage, and the delay before the
// next attack may start. They are separate so a short strike can still have a
// long recovery.
inline constexpr float kAttackDuration = 0.15f;
inline constexpr float kAttackCooldown = 0.35f;

// Upward push from liquid, as a multiple of gravity at full submersion. Above
// 1 so a body that sinks in comes back up and settles at the surface instead of
// resting on the bottom.
inline constexpr float kBuoyancy = 1.4f;

// Rate at which liquid bleeds off velocity, per second at full submersion.
inline constexpr float kWaterDrag = 5.0f;

// Submersion at which control switches from walking to swimming.
inline constexpr float kSwimThreshold = 0.25f;

inline constexpr float kSwimSpeed  = 150.0f;
inline constexpr float kSwimStroke = 200.0f;

// Downward stroke while holding crouch under water. Without it a body can only
// bob at the surface, since buoyancy always wins once it is submerged.
inline constexpr float kSinkStroke   = 170.0f;
inline constexpr float kWaterMaxFall = 200.0f;

// Free flight, for looking at the world rather than playing in it.
//
// Faster than running and much faster still while boosted, because what it is
// for is crossing enough of the world to judge how it was generated. The
// acceleration is high enough that it stops where the key is released, since
// drifting past what is being looked at is the whole annoyance of a flying
// camera.
inline constexpr float kFlySpeed      = 420.0f;
inline constexpr float kFlyBoostSpeed = 1600.0f;
inline constexpr float kFlyAccel      = 6000.0f;

// Distance from the body centre to the centre of the strike box, and the side
// of that box. The box stays axis aligned whatever the aim angle, which is an
// approximation of a swing but keeps the hit test a rectangle intersection.
inline constexpr float kAttackReach = 22.0f;
inline constexpr float kAttackSize  = 18.0f;

} // namespace player_config

// Per-frame input snapshot. Keeping Player independent of the keyboard allows
// remapping, replays and tests that run without a window.
struct PlayerInput {
    float moveX = 0.0f; // -1 left, +1 right.

    // -1 up, +1 down. Only read while flying, since on the ground the same keys
    // mean jump and crouch.
    float moveY = 0.0f;

    Vector2 aimWorld   = {}; // Aim target in world space, usually the cursor.
    bool jumpPressed   = false;
    bool jumpHeld      = false; // Held controls jump height, pressed triggers it.
    bool crouchHeld    = false;
    bool attackPressed = false;

    // Enters and leaves free flight. A debug control, but carried in the input
    // snapshot like every other one so that the character still knows nothing
    // about the keyboard.
    bool flyToggled = false;

    // Crosses ground quickly while flying.
    bool boostHeld = false;
};

// Platform character colliding against the terrain grid.
class Player {
public:
    enum class State { Idle, Running, Jumping, Falling, Crouching, Attacking, Swimming, Flying };

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

    // Deep enough in liquid that walking and jumping give way to swimming.
    bool IsSwimming() const { return submerged_ >= player_config::kSwimThreshold; }
    float Submerged() const { return submerged_; }
    int Facing() const { return facing_; }

    // Free flight: no gravity, no collision, no liquid. For inspecting the world
    // rather than playing in it, so it deliberately suspends every rule that
    // would otherwise keep the body out of the rock.
    bool IsFlying() const { return flying_; }

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

    // Free flight, which replaces the whole of the above: velocity follows the
    // keys in both axes and the body moves without being asked what it is moving
    // through.
    void Fly(const PlayerInput &input, float dt);

    // Movement resolves one axis at a time, horizontal then vertical. Moving
    // both at once cannot tell which axis caused an overlap, and the body
    // catches on flat ground it should slide along.
    //
    // The move is split into sub-steps no longer than half a grid cell so that
    // a fast fall cannot pass through thin ground between two frames.
    void MoveAndCollide(const World &terrain, float dt);

    // Lifts the body over a small ledge that blocked it sideways, and reports
    // whether it found a height that cleared. The three helpers below are what
    // separate walking over ground from being stopped by it; each one answers a
    // different way the lattice interrupts a move that should have carried on.
    bool StepOver(const World &terrain);

    // Pulls the body back down onto the ground it just walked off.
    void SnapToGround(const World &terrain);

    // Shifts the body sideways past a corner that blocked it going up.
    bool Sidestep(const World &terrain);

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
    bool flying_          = false;

    // Share of the body under liquid, in [0,1].
    float submerged_ = 0.0f;

    float coyoteTimer_     = 0.0f;
    float jumpBufferTimer_ = 0.0f;

    float attackTimer_    = 0.0f;
    float attackCooldown_ = 0.0f;

    int health_ = 100;
};
