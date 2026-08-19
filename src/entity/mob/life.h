#pragma once

#include "entity/mob/mob_def.h"
#include "entity/mob/wits.h"
#include "raylib.h"

namespace mob {

// One creature as it is remembered while nobody is looking at it.
//
// Everything a live `Mob` is, less the parts that can be worked out again: no body
// (it is rebuilt from the row), no brain pointer (looked up from the row), no
// facing (it follows the first step it takes). What is left is the four things that
// are *history* rather than description — where it got to, how much of it is left,
// what it was in the middle of doing, and its own stream of small decisions.
//
// The last two are what make coming back feel like returning rather than reloading.
// Without the `wits`, a boar you frightened and walked away from is grazing calmly
// when you come back, and every animal in the county steps off on the same foot at
// the same moment because they all start from a fresh seed.
struct Life {
    Kind kind{};

    // The body's own anchor — its feet.
    Vector2 at{};

    int health = 0;

    Wits wits{};
};

} // namespace mob
