#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kWater = {
        .name = "water",

        // The threshold is low on purpose. The field holds mass, not height,
        // and a lattice cell is the smallest thing that can be drawn: a stream
        // carrying a third of a unit is physically a third of a cell across,
        // which the grid cannot express. Only two outcomes are available,
        // nothing or a whole cell, and a high threshold picks nothing, so
        // running water breaks into disconnected pieces or vanishes.
        .threshold = 0.15f,

        .paint   = {.tone   = {{48, 122, 190, 255}, {74, 156, 223, 255}, {102, 191, 255, 255}, {160, 218, 255, 255}},

                    .grain  = 0.35f,

                    .patch  = 0.70f,

                    .strata = 0.00f},
        .contour = {56, 152, 236, 255},

        // Sand's ripple, deepened. Water is the only material here that is
        // drawn as a surface seen from above rather than as a face seen from
        // the side, so the courses are broken twice over instead of offset once.
        .icon =
            {
                "aabaab",
                "bbbbbb",
                "babbba",
                "bbcbbc",
                "cccccc",
                "ccdccd",
            },
        .stack = 64,

        // Not dug. It comes away the instant it is touched, which is what a zero
        // hardness is for.
        .hardness  = 0.0f,
        .tool      = Tool::Hand,
        .loose     = 0.00f,

        .rules = {.flows = true, .buoyancy = 1.0f},
        // The only row whose extent this table does not describe.
        //
        // Everything else here is a field thresholded against a depth band, and
        // that is the wrong shape for a liquid: a share of the cavities scattered
        // through a band is not water, it is a mist of it, hanging at whatever
        // height the noise happened to clear its cutoff. Water stands at a level.
        // So `Generator::Pool` reads terrain::TableAt instead, and neither the
        // shape, the probability nor the band below is consulted — `space` is the
        // whole of what it takes from here, and it says the obvious thing, that
        // water goes where the rock is not.
        //
        // See terrain::AquiferSettings for the level itself and for why placing a
        // liquid anywhere but at its own resting height cannot be made to work.
        .spawn =
            {
                .generator = Generator::Pool,
                .space     = SpawnSpace::OpenSpace,
            },

        // Dims light rather than stopping it, so a pool reads as deep by
        // getting darker towards the bottom instead of turning into a wall the
        // moment it is thick enough to swim in.
        .light = {.opacity = 0.10f},
    };

} // namespace elements
