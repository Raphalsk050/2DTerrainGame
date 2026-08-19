#pragma once

#include "entity/body/intent.h"
#include "raylib.h"

// One frame's worth of what the player asked for.
//
// It is a `body::Intent` plus the three things only a character has, and the split
// is deliberate rather than tidy: the intent is the part a boar also has, and
// keeping it as a nested value rather than as six loose fields is what makes that
// visible at every call site. `Player` hands `motion` straight to its body and never
// translates anything.
//
// Keeping the character independent of the keyboard was already the rule here —
// originally for remapping, replays and tests that run without a window. It turned
// out to be the thing that made mobs cheap: an animal is a body with the same wishes
// filled in by a brain. See `entity/body/intent.h`.
struct PlayerInput {
    // What the body is being asked to do. Shared with every creature in the world.
    body::Intent motion{};

    // Aim target in world space, usually the cursor. A character points at things;
    // nothing else in the world does.
    Vector2 aimWorld{};

    bool attackPressed = false;

    // Enters and leaves free flight. A debug control, but carried in the input
    // snapshot like every other one so that the character still knows nothing about
    // the keyboard.
    bool flyToggled = false;
};
