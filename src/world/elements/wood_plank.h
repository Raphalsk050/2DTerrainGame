#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

// The two built materials. Neither is anywhere in the world until somebody
// puts it there — see Generator::None — and both go down a cell at a time
// rather than under the brush, which is what separates building from shaping.
//
// Both sit at the same threshold as everything else in this table, and that
// is load-bearing twice over.
//
// A block has to meet the ground it is laid against without a seam. The
// contour crosses where the field meets the threshold, so two materials
// crossing at different values part company along their shared edge — a plank
// set into a hillside would either sink into the rock or stand a fraction of a
// cell off it. Matching the table is what makes a built wall continuous with
// the world it is built into.
//
// And it keeps a new material from reaching into the generator. Every occupying
// material is held under everything that outranks it by a term in the *other's*
// threshold — see World::ExclusionHeadroom — so a row added at a new threshold
// changes the headroom over every older one beneath it, and these two outrank
// the whole table. The clamp does not bite at the depths an ore actually sits
// at, so nothing was found to move here; matching the table is what means
// nothing has to be checked again if it ever does.
//
// The cost is six tenths of a pixel. At one half the contour would cross at the
// midpoint between a filled vertex and its empty neighbour and a cell would
// measure exactly its eighteen; at 0.45 it crosses a little further out and the
// block is that much proud of its square. Nothing can see it, and it is the
// cheaper of the two mistakes.
inline constexpr ElementDef kWoodPlank = {
        .name      = "wood plank",
        .threshold = 0.45f,

        // Sawn rather than barked: lighter than the wood item it will be made
        // from, and far more even. What says plank at this size is that the
        // colour barely moves — a sawn face has grain and no patching at all.
        .paint   = {.tone   = {{104, 72, 40, 255}, {138, 98, 56, 255}, {170, 128, 78, 255}, {198, 160, 106, 255}},
                    .grain  = 0.30f,
                    .patch  = 0.08f,

                    // Boards are courses, like stone is, and at a lower contrast:
                    // enough for a wall to read as laid up out of lengths.
                    .strata = 0.85f,

                    // Eighteen texels across a block, which is where authored
                    // pixel art goes. See ElementPaint::texel.
                    },
        .contour = {104, 72, 40, 255},

        // The joint is the whole of it. Two boards with a dark seam between them
        // and the grain running along each — take the seam away and this is a
        // brown square that could be anything.
        .icon =
            {
                "aaaaaa",
                "abbbab",
                "bbbbbb",
                "dddddd",
                "bcbccb",
                "cccccc",
            },
        .stack  = 64,
        .laying = Laying::Cell,

        // Outranks every ore and every cover, so a plank laid into a seam or into
        // a snowbank replaces it rather than being swallowed by it. What it
        // displaces comes back to the player — see World::ApplyBrush, which pays
        // out whatever it cleared.
        // Oak Planks: hardness 2 -- 3 s by hand.
        .hardness  = 2.0f,
        .tool      = Tool::Axe,
        .loose     = 0.00f,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 10},
        .spawn = {.generator = Generator::None},

        // A shade denser than stone. A plank floor is meant to be the thing that
        // makes a room a room, and a roof daylight leaked through would leave
        // nothing for a torch to do.
        .light = {.opacity = 0.85f},
    };

} // namespace elements
