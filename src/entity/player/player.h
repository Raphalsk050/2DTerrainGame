#pragma once

#include "entity/body/body.h"
#include "entity/life/blow.h"
#include "entity/life/health.h"
#include "entity/player/player_config.h"
#include "entity/player/player_input.h"
#include "raylib.h"

class World;

// The character.
//
// It *has* a body rather than *being* one, and that one change is what the mob layer
// is built on: the walk, the ledge, the corner, the water and the unsticking all
// moved into `body::Body`, so a creature gets every one of them by carrying the same
// member. Before that, the four helpers that make walking over this terrain bearable
// were private methods here, and a second walking thing would have meant a second
// copy of them — which is a place for the two to disagree about what a hillside is.
//
// What is left in this class is what is actually the character: where it is pointing,
// the swing, how much it can take, the state its animation is in, and how it is
// drawn. Everything else is composed.
class Player {
public:
    enum class State { Idle, Running, Jumping, Falling, Crouching, Attacking, Swimming, Flying };

    explicit Player(Vector2 spawn);

    void Update(const PlayerInput &input, const World &terrain, float dt);
    void Draw() const;

    // Body in world space. Shrinks vertically while crouching.
    Rectangle Bounds() const { return body_.Bounds(); }

    // Damage region of the current attack. Width is zero while inactive, so callers
    // can intersect it unconditionally.
    Rectangle AttackHitbox() const;

    // Centre of the body, which is what the character aims from.
    Vector2 Centre() const { return body_.Centre(); }

    Vector2 Position() const { return body_.Position(); }

    // Puts the character somewhere outright, clearing whatever it was doing on the
    // way.
    void PlaceAt(Vector2 at) { body_.PlaceAt(at); }

    Vector2 AimDirection() const { return aimDirection_; }
    State CurrentState() const { return state_; }
    bool IsGrounded() const { return body_.Grounded(); }

    // How fast the character is travelling, in world pixels per second.
    //
    // Exposed because what the ground does underfoot depends on it — dust is kicked
    // up by a pace, not by a position — and the alternative is for every watcher to
    // difference the body's own corner and get a different answer to this one.
    Vector2 Velocity() const { return body_.Velocity(); }

    bool IsAttacking() const { return attackTimer_ > 0.0f; }

    // True on the one frame a swing began.
    //
    // Distinct from IsAttacking, which stays true for the whole strike window — nine
    // frames at sixty. Anything that lands a blow has to read this one, or a single
    // swing deals its damage nine times.
    bool AttackStarted() const { return attackStarted_; }

    // Deep enough in liquid that walking and jumping give way to swimming.
    bool IsSwimming() const { return body_.Swimming(); }
    float Submerged() const { return body_.Submerged(); }
    int Facing() const { return facing_; }

    // Free flight: no gravity, no collision, no liquid. For inspecting the world
    // rather than playing in it.
    bool IsFlying() const { return body_.Ghost(); }

    // Something hit the character. Returns whether the blow landed — a refusal is
    // the mercy window, and a caller that ignored the answer would knock the player
    // about sixty times a second while dealing no damage.
    //
    // The same `life::Blow` a creature takes, travelling the other way. That is the
    // whole reason the type carries a position rather than a direction: one rule for
    // which way a thing is thrown, written once.
    bool Hurt(const life::Blow &blow);

    const life::Health &Vigour() const { return health_; }

    // Back on its feet, wherever it is. What a respawn and a change of gamemode both
    // want, and neither should have to know which fields that means.
    void Revive() { health_.Fill(); }

private:
    void UpdateAim(const PlayerInput &input);
    void UpdateAttack(const PlayerInput &input, float dt);
    void UpdateState();

    body::Body body_{player_config::Build(), {}};

    State state_ = State::Falling;

    // Unit vector towards the aim target. Facing follows its horizontal sign, so the
    // character turns with the cursor rather than with the movement key.
    Vector2 aimDirection_ = {1.0f, 0.0f};
    int facing_           = 1;

    float attackTimer_    = 0.0f;
    float attackCooldown_ = 0.0f;
    bool attackStarted_   = false;

    life::Health health_{.most = player_config::kHealth, .now = player_config::kHealth};
};
