#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kWoodWall = {
        .name      = "wood wall",
        .threshold = 0.45f,

        // The plank's own tones taken down towards the dark, and that is the whole
        // of how a wall reads as *behind*. Not by being faded — see the cache rule
        // in CLAUDE.md §5.5, which needs every colour the ground is drawn in to be
        // opaque — but by being the colour a plank is when no light reaches it.
        //
        // Far enough down that a wall is never mistaken for the floor in front of
        // it. A player builds a room out of both at once, and the two have to be
        // told apart at a glance in the dark.
        .paint   = {.tone   = {{40, 28, 16, 255}, {56, 39, 22, 255}, {72, 52, 31, 255}, {88, 68, 44, 255}},
                    .grain  = 0.28f,
                    .patch  = 0.08f,
                    .strata = 0.85f},
        .contour = {40, 28, 16, 255},

        // The plank's face, read in the dark. The same joint in the same place, so
        // a wall behind a wall of planks lines up with it.
        .icon =
            {
                "aaaaaa",
                "abbbab",
                "bbbbbb",
                "dddddd",
                "bcbccb",
                "cccccc",
            },
        .stack  = 64,
        .laying = Laying::Cell,

        // Behind everything and in the way of nothing. No precedence, because it
        // never enters the contest — see ElementRules::background.
        // Oak Planks: hardness 2 -- 3 s by hand.
        .hardness  = 2.0f,
        .tool      = Tool::Axe,

        .rules = {.background = true},
        .spawn = {.generator = Generator::None},

        // Casts nothing. A wall that dimmed the light would darken the room it is
        // the back of, and the torch standing in front of it would be solving a
        // problem the wall had just invented. Deliberate, and the first thing to
        // revisit if a built room ever wants to be dark on its own.
        .light = {.opacity = 0.0f},
    };

} // namespace elements
