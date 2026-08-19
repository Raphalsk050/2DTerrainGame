#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kDiamond = {
        .name      = "diamond",
        .threshold = 0.45f,
        .paint   = {.tone   = {{38, 144, 152, 255}, {70, 184, 189, 255}, {104, 224, 226, 255}, {170, 245, 246, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    .vein   = {.share = 0.52f}},
        .contour = {40, 146, 154, 255},

        // The two gems are cut rather than surfaced: the corners are taken off
        // and the light collects in the middle instead of along the top, which
        // is the only way six texels can say faceted. Diamond is cut across —
        // a wide table with the crown falling away below it.
        .icon =
            {
                "ccaacc",
                "cabbac",
                "abbbba",
                "abccba",
                "bcddcb",
                "cddddc",
            },
        .stack = 64,
        // Diamond Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 5},
        .spawn =
            {
                // The deepest and by a distance the rarest: a quarter of one per
                // cent of the rock even where it is densest. Minecraft puts its
                // peak five blocks off the floor of the world, and the shape of
                // that is what is copied here — almost all of it in the last
                // stretch before there is nothing below.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8106},
                .probability = 0.00028f,
                .veinCells   = 3.5f,
                .space       = SpawnSpace::InsideGround,
                .band =
                    {.top = 1100.0f, .bottom = kUnboundedDepth, .peak = 2400.0f, .scarcity = 0.22f, .jitter = 72.0f},

                .wallBias    = 0.180f,
                .wallReach = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
