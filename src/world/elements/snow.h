#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

inline constexpr ElementDef kSnow = {
        .name      = "snow",
        .threshold = terrain::kSurfaceLevel,
        .paint   = {.tone   = {{168, 182, 205, 255}, {201, 213, 231, 255}, {234, 239, 248, 255}, {252, 254, 255, 255}},
                    .grain  = 0.34f,
                    .patch  = 0.55f,
                    .strata = 0.00f},
        .contour = {182, 196, 216, 255},

        // Weighted to the lit face, since snow is the one material whose whole
        // character is that it is bright. The shading is kept to the last two
        // courses; taking it further up turns fresh snow into grey slush, and
        // its four tones are close enough together that there is nowhere to
        // recover the brightness from once it has gone.
        .icon =
            {
                "aaaaaa",
                "aaaaaa",
                "aabaab",
                "bbbbbb",
                "bbcbbc",
                "ccdccd",
            },
        .stack = 64,
        // Snow Block: hardness 0.2, and a fist gets nothing out of it -- 1 s by hand.
        .hardness  = 0.2f,
        .tool      = Tool::Shovel,
        .loose     = 0.95f,
        .needsTool = true,

        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 9},
        .spawn =
            {
                .generator = Generator::Cover,

                // The finest of the three, because a snow line is drawn by the
                // shape of the hill it lies on and a coarse field would drape it
                // across two valleys at once.
                .shape = {.frequency = 3.4f, .octaves = 2, .seed = 8205},
                .space = SpawnSpace::InsideGround,

                // A cap and not a layer. Thin enough that the soil it sits on is
                // still there to be dug, which is what keeps a snowfield ground
                // rather than a different world.
                //
                // Proportionally the widest swing of the three, and that is what
                // snow actually does: it is the one cover that is *placed by the
                // weather* rather than weathered out of the rock, so it gathers
                // in every hollow and is scoured off every rib. Sixteen either way
                // on a mean of sixteen means a mountainside is drifts and bare
                // patches rather than a coat of paint over the crest — and the
                // patches cost nothing extra, since a column whose snow came out
                // at zero simply shows the ground underneath.
                //
                // It does not follow the other two up into the new headroom, and
                // that is deliberate. A cover this thin is a cap by definition, and
                // the whole of what keeps a snowfield ground rather than a separate
                // world is that the soil beneath it is still there to be dug. A
                // drift may be deep; a snowfield may not be a stratum.
                .thickness     = 16.0f,
                .thicknessVary = 10.0f,

                // The finest grain in the table, matching the field above it. A
                // drift is a small thing — it forms behind whatever broke the wind
                // — so this runs at one feature every seventy pixels, near the
                // limit of what the lattice can hold. Snow is also the one cover
                // where the grain carries more than the regional swell: where it
                // lies at all it lies everywhere, and what varies is how deep it
                // has piled from one dip to the next.
                .grain     = {.frequency = 11.0f, .octaves = 3, .seed = 8206},
                .grainVary = 14.0f,

                // The snow line, and it is what makes snow a mountain and not a
                // latitude.
                //
                // Measured, not reasoned: `--covers` reports the surface's own range
                // over a stretch of world, and with the ranges switched off it runs
                // -14 to 310. So ordinary high ground never reaches y = -14 and a
                // line at -60 clears it with room to spare, while the ranges climb
                // past -360. Full depth from y = -190 up, nothing below y = -60, and
                // the hundred and thirty between is the belt where a mountainside is
                // patchy — which is what a snow line looks like from a distance and
                // is why this is a fade rather than an edge.
                //
                // It replaces nothing: the climate bell below still decides whether
                // a range is cold enough to hold snow at all, and the lapse rate
                // still carries a northern peak past it sooner than a southern one.
                // What this adds is the one thing a bell cannot say, which is
                // "high". Before it, the coldest lowland in the world read exactly
                // like a summit and lay under snow a day's walk from any mountain.
                .crest     = -190.0f,
                .crestFade = 130.0f,

                // Colder than anything else asks for — the pine, the coldest tree
                // in the table, centres on 0.30. Widened now that the crest above
                // does the work of keeping snow off the plains: what this is for is
                // no longer "only the far north" but "not the tropics", so a
                // temperate range wears a cap and only a range standing in a desert
                // comes out bare.
                //
                // Indifferent to how wet it is, since what falls as snow is
                // decided by the cold alone.
                .climate       = {.temperature      = 0.20f,
                                  .humidity         = 0.5f,
                                  .temperatureWidth = 0.40f,
                                  .humidityWidth    = kUnboundedDepth,
                                  .fullAt           = 0.50f,
                                  .goneAt           = 0.30f},
                .climateJitter = 0.14f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
