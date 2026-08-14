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

// Held speed, on the shift key.
//
// A little over one and a half times the walk, which is the ratio a pair of
// Terraria's boots gives and is about the smallest one that reads as a different
// gait rather than as the same one tuned up. It is also what makes the jump worth
// running into: the arc lasts six tenths of a second whatever the speed, so a
// standing jump carries the body 132 pixels and a sprinting one 228 — the
// difference between clearing a hole and landing in it.
//
// Measured against the frame rather than against the character, because what a
// player is actually asking for when they ask to run is to spend less time
// crossing ground they have already seen: at this speed the thousand pixels of
// the window go by in two and a half seconds instead of four and a half.
inline constexpr float kSprintSpeed = 380.0f;

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

// How far the body is carried to get out of ground that closed around it.
//
// Nothing in ordinary play puts a body inside rock — the brush now refuses to
// lay a block where the character is standing, which is where this used to come
// from — but a few paths still can: leaving free flight inside a hill, a chunk
// regenerating under a body resting on it, liquid turning to something solid.
// Every one of them ends the same way if nothing catches it, because a body
// already overlapping is blocked whichever direction it tries, so it is not stuck
// in the ground so much as stuck against every wall of it at once.
//
// Two body heights, and no further. What this is for is the pixel or the block
// that closed over the character, not tunnelling out of a mountain: past this
// distance the honest answer is that the character is buried, and lifting one out
// of solid rock across half a screen would be a worse surprise than the burial.
inline constexpr float kUnstickReach = 2.0f * kHeight;

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

    // Asks for speed: a run on the ground, and a much faster crossing in flight.
    //
    // One field for both, the way moveY above is one field for two meanings, and
    // for the same reason — the two can never be asked for at once, because a
    // flying body is not walking.
    bool sprintHeld = false;
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

    // Puts the character somewhere outright, clearing whatever it was doing on the
    // way. Momentum is dropped with it: arriving somewhere new still carrying the
    // speed of the fall that was interrupted is how a teleport ends underground.
    void PlaceAt(Vector2 at) {
        position_ = at;
        velocity_ = {};
    }
    Vector2 AimDirection() const { return aimDirection_; }
    State CurrentState() const { return state_; }
    bool IsGrounded() const { return grounded_; }

    // How fast the character is travelling, in world pixels per second.
    //
    // Exposed because what the ground does underfoot depends on it — dust is kicked
    // up by a pace, not by a position — and the alternative is for every watcher to
    // difference the body's own corner and get a different answer to this one.
    Vector2 Velocity() const { return velocity_; }
    bool IsAttacking() const { return attackTimer_ > 0.0f; }

    // True on the one frame a swing began.
    //
    // Distinct from IsAttacking, which stays true for the whole strike window —
    // nine frames at sixty. Anything that lands a blow has to read this one, or a
    // single swing deals its damage nine times.
    bool AttackStarted() const { return attackStarted_; }

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

    // Carries a body that is already inside the ground to the nearest place it
    // is not, and reports whether it found one.
    //
    // The three above are about a move that was interrupted; this one is about a
    // body that has nowhere to move from. Collision alone cannot answer it —
    // every direction out of solid ground starts in solid ground, so every move
    // is refused and the character is held where it stands for good.
    bool Unstick(const World &terrain);

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
    bool attackStarted_   = false;

    int health_ = 100;
};
