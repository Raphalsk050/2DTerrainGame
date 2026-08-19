#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kCobblestone = {
        .name      = "cobblestone",
        .threshold = 0.45f,

        // Rock's own greys, cooled very slightly and spread wider apart. Broken
        // stone catches the light on more faces than bedded stone does, so the
        // range is what says it has been through a pick.
        .paint   = {.tone   = {{74, 74, 80, 255}, {102, 102, 108, 255}, {130, 130, 136, 255}, {162, 162, 168, 255}},

                    .grain  = 0.85f,

                    // High patching and no bedding at all, which is exactly the
                    // opposite of the rock it came out of: cobble is rubble, and
                    // rubble has no layers left.
                    .patch  = 1.10f,
                    .strata = 0.00f},
        .contour = {74, 74, 80, 255},

        // Stones of no particular size with the joints between them, against
        // rock's clean horizontal courses. The two greys have to be told apart in
        // a slot, and this is the only mark that does it.
        .icon =
            {
                "aabaab",
                "badbba",
                "bbbdbb",
                "bdcbcd",
                "ccdccc",
                "cdcccd",
            },
        .stack  = 64,
        .laying = Laying::Cell,
        // Cobblestone: hardness 2, and a fist gets nothing out of it -- 10 s by hand.
        .hardness  = 2.0f,
        .tool      = Tool::Pick,
        .loose     = 0.22f,
        .needsTool = true,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 11},
        .spawn = {.generator = Generator::None},

        // The stone it came from, unchanged. Breaking rock up does not make it
        // let light through.
        .light = {.opacity = 0.8f},
    };

} // namespace elements
