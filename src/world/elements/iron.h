#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kIron = {
        .name      = "iron",
        .threshold = 0.45f,
        .paint   = {.tone   = {{86, 80, 74, 255}, {117, 111, 103, 255}, {150, 143, 134, 255}, {186, 180, 172, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    .vein   = {.share = 0.56f}},
        .contour = {92, 86, 79, 255},

        // Iron is rolled: courses running flat across, stepping straight down,
        // where copper's sheen runs diagonally. Banding against a diagonal is
        // what separates two greys with a warm cast at six texels.
        //
        // The courses are broken by a texel each rather than laid clean. A run
        // of six identical texels is a painted stripe and reads as one; the
        // stipple has to cross every boundary or the whole face goes flat.
        .icon =
            {
                "aaaaba",
                "abbbbb",
                "bbbcbb",
                "bcccbc",
                "ccccdc",
                "cdcddd",
            },
        .stack = 64,
        // Iron Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 3},
        .spawn =
            {
                // The broadest band of any ore, and small veins. Found nearly
                // anywhere underground, which is what makes it the material
                // everything ordinary is built out of.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8101},
                .probability = 0.0062f,
                .veinCells   = 6.0f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 400.0f, .bottom = 3200.0f, .peak = 1500.0f, .scarcity = 0.16f, .jitter = 56.0f},

                .wallBias    = 0.100f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
