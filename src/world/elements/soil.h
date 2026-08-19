#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

// The three covers. Each one is a skin over the rock as thick as its climate
// allows, and they stack by precedence alone: sand outranks soil and so
// replaces it where a desert wants it, snow outranks both but asks for so
// little depth that what it takes is a cap, leaving the soil underneath.
// Nothing had to be taught either arrangement — it falls out of the exclusion
// pass, which is the whole point of ranking materials rather than choosing
// between them.
inline constexpr ElementDef kSoil = {
        .name = "soil",

        // The same line the rock crosses, for the same reason: a cover's field is
        // its distance to its own edges mapped through kDensitySpan, exactly as
        // the terrain's is, so the two meet on one contour instead of leaving a
        // seam a fraction of a cell wide between them.
        .threshold = terrain::kSurfaceLevel,

        .paint   = {.tone   = {{68, 48, 30, 255}, {95, 68, 42, 255}, {122, 88, 56, 255}, {154, 115, 76, 255}},

                    .grain  = 0.72f,

                    .patch  = 0.95f,

                    .strata = 0.40f},
        .contour = {74, 52, 30, 255},

        // Loose rather than laid down: the tones break across each other in
        // clods instead of in courses, and the darkest marks are scattered as
        // the stones in it are.
        .icon =
            {
                "aabaab",
                "babbba",
                "bbcbbb",
                "bccbcb",
                "ccdccc",
                "cdcddc",
            },
        .stack = 64,
        // Dirt: hardness 0.5 -- 0.75 s by hand.
        .hardness  = 0.5f,
        .tool      = Tool::Shovel,
        .loose     = 0.70f,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 7},
        .spawn =
            {
                .generator = Generator::Cover,

                // One feature every four hundred pixels or so: the scale at which
                // soil reads as having gathered in places rather than as having
                // been spread with a trowel.
                .shape = {.frequency = 2.6f, .octaves = 2, .seed = 8201},
                .space = SpawnSpace::InsideGround,

                // Around six squares on average, with nearly the whole budget spent
                // on the swing rather than the mean. The old pair was 36 ± 10 and
                // it drew the stripe this whole arrangement exists to get rid of:
                // a standard deviation of 3.2 px on a mean of 36, which is a layer
                // of one thickness with a rounding error on it.
                //
                // Thirty-two with thirty either way is very nearly the same soil on
                // average — an ordinary hole still stays in it and reaching rock is
                // still a decision — but the two noises together carry it from a
                // scrape over bare rock to a pocket seventeen squares deep. That
                // range is the point and the mean is not: what makes ground worth
                // digging is that the next hole is not the last hole.
                //
                // No climate: soil is what the ground is made of wherever nothing
                // else has claimed it, so its bell is the default one and reads
                // as one everywhere.
                .thickness     = 50.0f,
                .thicknessVary = 44.0f,

                // A feature every hundred and eighty pixels, with two octaves under
                // it reaching down to forty-five — about seven lattice columns,
                // which is near the limit of what the world can describe. Soil is
                // the roughest floor of the three: it holds an edge, so it keeps
                // whatever shape it was left in.
                //
                // The frequency came down as the amplitude went up, and the two are
                // not independent. What the eye reads as ground rather than as
                // static is how far the floor moves *between neighbouring columns*,
                // which is amplitude over wavelength — so doubling the swing at a
                // fixed frequency does not make a rougher floor, it makes a noisy
                // one. `--covers` reports that figure and the target is the ground's
                // own 1.1 px per lattice column.
                .grain     = {.frequency = 5.5f, .octaves = 3, .seed = 8202},
                .grainVary = 38.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
