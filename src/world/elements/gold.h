#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kGold = {
        .name      = "gold",
        .threshold = 0.45f,
        .paint   = {.tone   = {{136, 104, 24, 255}, {179, 143, 43, 255}, {222, 183, 64, 255}, {248, 218, 122, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    .vein   = {.share = 0.54f}},
        .contour = {148, 114, 20, 255},

        // Gold is soft, so its sheen is the broadest and the least stepped of
        // the three: the same diagonal as copper with the boundaries stippled
        // open, which is the difference between a struck edge and a polished
        // one.
        .icon =
            {
                "aaabba",
                "aabbbc",
                "abbbcc",
                "bbabcd",
                "bccbcd",
                "cccddd",
            },
        .stack = 64,
        // Gold Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 4},
        .spawn =
            {
                // A fifth of iron's share, and deep. The lower frequency is what
                // keeps that share as a few seams worth finding rather than
                // dust scattered evenly through the rock.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8104},
                .probability = 0.00065f,
                .veinCells   = 3.5f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 700.0f, .bottom = 3400.0f, .peak = 1800.0f, .scarcity = 0.20f, .jitter = 64.0f},

                .wallBias    = 0.130f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
