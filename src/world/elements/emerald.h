#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kEmerald = {
        .name      = "emerald",
        .threshold = 0.45f,
        .paint   = {.tone   = {{24, 122, 60, 255}, {47, 163, 88, 255}, {72, 206, 118, 255}, {134, 233, 166, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    // The rarest and the narrowest seam, and the sparsest to look at with it —
                    // a few flecks caught in the rock, which is what a stone holding a little
                    // of something precious actually looks like.
                    .vein   = {.share = 0.50f}},
        .contour = {26, 124, 62, 255},

        // Emerald takes a step cut instead of diamond's brilliant: the corners
        // are off it in the same way, but the light gathers to one side and
        // falls away across the stone rather than sitting symmetrically on a
        // table. Two gems that took the same cut would be one green square and
        // one cyan square, and the point of a picture is that it is not that.
        //
        // Its first cut ran a dark course down the middle of the lit face,
        // meaning to read as a facet edge. At this size an interior mark
        // surrounded by lighter tone has nothing to be an edge *of* and reads as
        // a smudge on the stone. A facet has to be a boundary between two
        // regions, which means it has to reach the outline.
        .icon =
            {
                "cbaabc",
                "baaabb",
                "aaabbb",
                "abbbcc",
                "abbccd",
                "cbccdc",
            },
        .stack = 64,
        // Emerald Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 6},
        .spawn =
            {
                // The odd one out: in Minecraft it is found only in mountains and
                // gets denser the higher it goes, which makes it the one ore that
                // is a reward for climbing rather than for digging. Until there
                // are biomes to hang that on, it is here as the shallowest and
                // rarest of the set, which keeps the shape of the idea if not the
                // rule behind it.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8107},
                .probability = 0.00060f,
                .veinCells   = 3.0f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 170.0f, .bottom = 1100.0f, .peak = 320.0f, .scarcity = 0.24f, .jitter = 40.0f},

                .wallBias    = 0.035f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
