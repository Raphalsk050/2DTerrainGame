#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kRock = {
        .name = "rock",

        // Taken from the generator rather than written down again. The terrain
        // field is a signed distance mapped into [0,1], and this is the value it
        // crosses where that distance is zero; a second copy of the number here
        // would let the rock's outline drift away from the ground the generator
        // describes.
        .threshold = terrain::kSurfaceLevel,

        .paint   = {.tone   = {{62, 70, 84, 255}, {84, 93, 108, 255}, {105, 115, 130, 255}, {132, 143, 158, 255}},

                    .grain  = 0.55f,

                    .patch  = 1.00f,

                    .strata = 1.15f},
        .contour = {0, 82, 172, 255},

        // Bedded, which is the one thing that separates stone from every other
        // grey at this size: the seam across the middle and the darker course
        // under it are the same layering the paint's strata term draws in the
        // wall.
        .icon =
            {
                "aaabaa",
                "abbabb",
                "bbbbbb",
                "bcbbcb",
                "cccccc",
                "cdccdc",
            },
        .stack = 64,

        // Broken stone, not stone. What a pick takes out of a hillside is rubble,
        // and rubble is what there is to build with — which is also the whole of
        // why the world starts the player with nothing and a hillside.
        //
        // It means the untouched rock of the generator is the only rock there is:
        // a player can never put it back, only cobble in its place. That is
        // Minecraft's arrangement and it is worth keeping for the same reason —
        // ground that was never dug looks different from ground that was.
        .yields = Element::Cobblestone,
        // Stone: hardness 1.5, and a fist gets nothing out of it -- 7.5 s by hand.
        .hardness  = 1.5f,
        .tool      = Tool::Pick,
        .loose     = 0.22f,
        .needsTool = true,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 0},
        .spawn   = {.generator = Generator::Terrain},
        .light   = {.opacity = 0.8f},
    };

} // namespace elements
