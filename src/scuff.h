#pragma once

#include "element.h"
#include "raylib.h"

#include <array>

class World;

// The ground answering back under a foot.
//
// A footfall is the one event in this world that is *not* a pure function of
// anything: where a player put a foot down is not derivable from the clock or from
// the terrain, so unlike the leaves — which are a field with nothing stored — this
// has to remember. What it remembers is the smallest thing that will do: where a
// foot landed, what it landed on, and when. The specks themselves are still worked
// out from that in closed form, exactly as a burst of leaves is, so nothing is
// integrated frame by frame and a puff cannot drift.
//
// It reads the material and never writes to it. Kicking up dust changes nothing
// about the ground, which is the whole reason this can sit outside the world rather
// than inside it.
namespace scuff {

// How many footfalls are kept in the air at once.
//
// Small, and it can be: a puff lives under half a second and a walking pace lands a
// foot about three times a second, so four is already more than can overlap. What
// this is really sized against is a player sprinting downhill, where the stride
// shortens and a landing throws its own puff on top of the walking ones.
inline constexpr int kSteps = 8;

// One foot coming down.
struct Step {
    Vector2 at{};

    // What it came down on. The dust is that material's own colour, because a foot
    // in snow and a foot in sand throw up plainly different things and reading it
    // off the ground is what makes the two look like the same rule rather than two.
    Element on = Element::Soil;

    // When it landed, on the weather clock — the same one the leaves and the sway
    // run on, so everything loose in the air speeds up together under F7.
    float when = -1.0f;

    // How hard, in [0,1]. A walking pace barely marks the ground; a sprint tears at
    // it and a landing from a height throws a ring.
    float force = 1.0f;

    // Which way the foot was travelling. Dust is thrown *behind* a runner, and that
    // is most of what makes it read as speed rather than as a puff of smoke.
    float away = 1.0f;
};

// The dust under one pair of feet.
//
// Held by whoever owns the character rather than by the world, since it is a
// property of something moving through the ground and not of the ground.
class Trail {
public:
    // Watches the feet and lays down a step whenever one lands.
    //
    // Given the body rather than a point, because where a foot is is the bottom of
    // the body and the caller should not have to know that. `speed` is how fast the
    // character is travelling and `fall` is how fast it was falling on the frame it
    // touched down — zero while it stays down.
    void Update(const World &world, Rectangle body, float speed, float fall, bool grounded, float now);

    // Every puff still in the air. Drawn in world space, under everything the
    // character is drawn over.
    void Draw(float now) const;

    // How many puffs are still live, for the head-up display.
    int Live(float now) const;

private:
    std::array<Step, kSteps> steps_{};

    int next_ = 0;

    // How far the character has walked since the last step was laid, in world
    // pixels. A stride and not a timer, so a walk and a sprint put feet down the
    // same distance apart rather than the same time apart — which is the difference
    // between a run that lengthens its stride and one that shuffles faster.
    float strode_ = 0.0f;

    // Where the body was last frame, to measure that against.
    Vector2 wasAt_{};

    bool started_ = false;

    // Whether the feet were on the ground last frame, so a landing can be told from
    // a stride.
    bool wasDown_ = false;
};

} // namespace scuff
