#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kSand = {
        .name      = "sand",
        .threshold = terrain::kSurfaceLevel,
        .paint   = {.tone   = {{148, 128, 84, 255}, {181, 160, 108, 255}, {214, 192, 134, 255}, {240, 224, 178, 255}},
                    .grain  = 0.50f,
                    .patch  = 0.70f,
                    .strata = 0.60f},
        .contour = {166, 144, 92, 255},

        // The evenest face in the table, and deliberately. Sand is the one
        // material with no structure of its own, so what it gets is a ripple —
        // a single texel offset between one course and the next.
        .icon =
            {
                "aaaaaa",
                "aabaab",
                "bbbbbb",
                "bbcbbc",
                "cccccc",
                "ccdccd",
            },
        .stack = 64,
        // Sand: hardness 0.5 -- 0.75 s by hand.
        .hardness  = 0.5f,
        .tool      = Tool::Shovel,
        .loose     = 1.00f,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 8},
        .spawn =
            {
                .generator = Generator::Cover,
                .shape     = {.frequency = 2.2f, .octaves = 2, .seed = 8203},
                .space     = SpawnSpace::InsideGround,

                // Nearly as deep as the soil it displaces, so a desert is sand to
                // dig through and not a dusting over brown ground, and swinging as
                // widely for the same reason soil does — a shade wider, even, since
                // a desert is the one place where what the layer is doing *is* the
                // landscape.
                .thickness     = 48.0f,
                .thicknessVary = 46.0f,

                // Nearly as rough a floor as soil's, and the first attempt at this
                // had it much smoother on the grounds that sand slumps and cannot
                // hold a scarp. That is true of sand and it is an argument about
                // the wrong surface. What comes to rest along a curve is the
                // *top* of a sand bed — and the top of a cover here is the
                // terrain's own surface, which this cannot move. The floor is the
                // rock the sand is lying in, and buried rock is as ragged as rock
                // anywhere: a desert is sand filling the hollows of a bedrock
                // surface, which is the same relationship soil has with it.
                //
                // Measured, and it is why the reasoning got corrected rather than
                // kept: at one feature every two hundred and fifty pixels the
                // desert floor moved 0.48 px per lattice column against the 1.1 the
                // ground above it moves, so the desert still had the flat painted
                // underside this whole change exists to remove. Slightly coarser
                // than soil — a desert's bedrock is the more buried of the two —
                // and otherwise the same treatment.
                .grain     = {.frequency = 4.5f, .octaves = 3, .seed = 8204},
                .grainVary = 36.0f,

                // Hot and dry, and well past where any tree in the table grows —
                // the hottest of those is the apple at 0.60 — so a desert comes
                // out bare rather than wooded, without anything having to say so.
                .climate       = {.temperature      = 0.88f,
                                  .humidity         = 0.14f,
                                  .temperatureWidth = 0.22f,
                                  .humidityWidth    = 0.26f,
                                  .fullAt           = 0.45f,
                                  .goneAt           = 0.26f},
                .climateJitter = 0.16f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
