#pragma once

#include <algorithm>

// How much a thing can take, and how much is left.
//
// The player already carried an `int health_` that nothing read, which is the
// usual shape of a field added before there was anything to hurt it. It is a type
// now for one reason: the moment there is a second thing with health, "how much
// is left" and "was it just hit" have to mean exactly the same on both sides, or
// the player's own hurt flash and a mob's are two implementations of one idea and
// only one of them gets fixed.
//
// It knows nothing about what hit it, what it drops, or what happens when it runs
// out. Those are decisions for whoever owns the health — the character has a
// death screen and a boar has a drop table, and neither belongs in a counter.
namespace life {

// Seconds a thing reads as freshly hurt after a blow lands.
//
// Short: it is the whole feedback that a hit connected, and anything longer reads
// as a state the creature is in rather than as a thing that just happened.
inline constexpr float kFlash = 0.18f;

// Seconds after a blow before the same source may land another.
//
// Minecraft's is half a second, and it is not a courtesy — without it anything
// that hurts on contact hurts once per frame, which at sixty frames is death
// inside a step. Every damage rule in this project has to go through Hurt for
// exactly this reason.
inline constexpr float kMercy = 0.5f;

struct Health {
    int most = 1;
    int now  = 1;

    // Counts down. Both are seconds and both run on the frame clock rather than
    // the weather clock, for the reason CLAUDE.md §13.4 gives about the digging
    // bite: how long a thing stays hurt is a rule of the game, and on the weather
    // clock F7 would make everything invulnerable forty times faster.
    float flashFor = 0.0f;
    float mercyFor = 0.0f;

    bool Alive() const { return now > 0; }

    // In [0,1], for a bar.
    float Fraction() const { return (most > 0) ? std::clamp(static_cast<float>(now) / most, 0.0f, 1.0f) : 0.0f; }

    // True while the last blow is still being shown.
    bool Stung() const { return flashFor > 0.0f; }

    // Whether another blow may land at all.
    bool Open() const { return mercyFor <= 0.0f; }

    // Takes a blow, and reports whether it landed.
    //
    // A refusal is not a failure — it is the mercy window doing its job — so the
    // caller has to be able to tell the two apart: a hit that did not land must
    // not knock the target back, must not play a sound and must not count towards
    // anything.
    bool Hurt(int damage) {
        if (damage <= 0 || !Alive() || !Open()) return false;

        now      = std::max(0, now - damage);
        flashFor = kFlash;
        mercyFor = kMercy;

        return true;
    }

    void Heal(int by) { now = std::clamp(now + by, 0, most); }

    void Fill() { now = most; }

    void Tick(float dt) {
        flashFor = std::max(0.0f, flashFor - dt);
        mercyFor = std::max(0.0f, mercyFor - dt);
    }
};

} // namespace life
