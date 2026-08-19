#pragma once

#include "world/element_def.h"

// One material, as one row. See `world/element_def.h` for what every field
// means, and `world/element.h` for where the rows are gathered into a table.
namespace elements {

// The ores below follow Minecraft's set and its ordering, on a scale of one
// block to sixteen pixels: its sea level at Y 64 is this world's y 144 and
// its floor at Y -64 is y 2192. That was the scale when a block here was
// sixteen pixels too, and the bands were left alone when it became eighteen
// — see kBlockSide. They are absolute heights in pixels and were settled by
// digging for the ore rather than by the conversion, so re-deriving them
// against the new figure would move every seam in the world to fix a sum
// nothing reads. The arithmetic above is history; the depths are the world.
//
// What is not carried across is Minecraft's absolute heights, since it has a
// floor to arrange them against and this world does not, so each peak sits
// where the ore is actually worth digging for here.
//
// Each ore is written as three numbers and a level: how likely it is where it
// is densest, how many lattice cells across one vein of it is, how quickly it
// thins out away from that level, and the level itself.
inline constexpr ElementDef kCoal = {
        .name      = "coal",
        .threshold = 0.45f,
        .paint   = {.tone   = {{24, 24, 30, 255}, {40, 40, 48, 255}, {58, 58, 66, 255}, {82, 82, 92, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f,
                    // The commonest ore and the widest seams, so the richest of them to look at:
                    // above the percolation line, so the blotches join up and a coal seam reads
                    // as a mass of it with the rock coming through rather than as flecks.
                    .vein   = {.share = 0.62f}},
        .contour = {26, 26, 32, 255},

        // Lumpy and matt. Coal is the one ore that does not catch the light, so
        // it gets no sheen and no facet: the tones interleave without ever
        // settling into a course, which is what reads as a broken black surface
        // rather than as a polished one.
        .icon =
            {
                "abbbab",
                "bbbcbb",
                "bcbbcb",
                "cbccbc",
                "ccdcdc",
                "cddddc",
            },
        .stack = 64,
        // Coal Ore: hardness 3, and a fist gets nothing out of it -- 15 s by hand.
        .hardness  = 3.0f,
        .tool      = Tool::Pick,
        .needsTool = true,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 1},
        .spawn =
            {
                // The commonest ore and the largest veins, as in Minecraft, where
                // it is seventeen blocks to iron's four.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8103},
                .probability = 0.034f,
                .veinCells   = 8.0f,
                .space       = SpawnSpace::InsideGround,

                // Shallow: the first thing dug up, thinning out well before the
                // depths the later ores belong to.
                .band = {.top = 200.0f, .bottom = 1600.0f, .peak = 480.0f, .scarcity = 0.17f, .jitter = 48.0f},

                // Drawn onto the cave walls. See ElementSpawn::wallBias: the ore
                // is moved rather than added, since the cutoff is measured with
                // this in it, so what the bias buys is a reason to walk a passage
                // instead of tunnelling past it.
                .wallBias    = 0.200f,
                .wallReach = 48.0f,
            },
        .light = {.opacity = 1.0f},
    };

} // namespace elements
