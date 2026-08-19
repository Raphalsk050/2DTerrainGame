#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kCopper = {
        .name      = "copper",
        .threshold = 0.45f,
        .paint   = {.tone   = {{116, 56, 34, 255}, {155, 82, 53, 255}, {196, 110, 74, 255}, {228, 150, 112, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    .vein   = {.share = 0.58f}},
        .contour = {132, 66, 40, 255},

        // The three metals are told apart by how each one catches the light,
        // since their tones alone are close enough to read as one another in a
        // slot. Copper takes a diagonal sheen, which is the sharpest of the
        // three and the one that says beaten rather than cast.
        .icon =
            {
                "aaabbb",
                "aabbbc",
                "abbbcc",
                "bbbccd",
                "bbcccd",
                "bcccdd",
            },
        .stack = 64,
        // Copper Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 2},
        .spawn =
            {
                // Shares coal's range almost exactly in Minecraft, and does here
                // too, so the shallow depths have two things worth mining rather
                // than one.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8105},
                .probability = 0.0154f,
                .veinCells   = 6.5f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 220.0f, .bottom = 1800.0f, .peak = 560.0f, .scarcity = 0.18f, .jitter = 44.0f},

                .wallBias    = 0.170f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
