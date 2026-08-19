#pragma once

#include "item/item_def.h"

// The wood axe. Right for wood: planks, walls, and the trees themselves.
//
// Made of planks, so the head takes the plank row's own pale tones and the haft takes a
// brown darker than any of them. A wooden tool whose head and handle were the same
// colour would be a brown smear.
// It also fells trees faster, and that is not a second mechanic: `main.cpp` divides
// flora::kLogSeconds by the same tier multiplier BreakSeconds divides by, so an axe
// is worth exactly as much to a standing tree as it is to a plank wall.
namespace items {

inline constexpr ItemDef kWoodAxe = {
    .name   = "wood axe",
    .colour = {170, 128, 78, 255},

    // A head four wide across the top narrowing to two at the bottom. It was a
    // three-by-four block of solid tone once and read at thirty-six pixels as a grey
    // rectangle beside a brown stick — a silhouette is all there is to go on at this
    // size, and a rectangle is not one.
    .picture =
        {
            .tone = {{198, 160, 106, 255}, {170, 128, 78, 255}, {138, 98, 56, 255}, {90, 58, 32, 255}},
            .art =
                {
                    "aaab..",
                    "abbbd.",
                    "acbbd.",
                    "cc..d.",
                    "....d.",
                    "....d.",
                },
        },

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Axe, .speed = tool::kWood, .damage = 2},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item WoodAxe() {
    static const Item id = item::Table().IdOf(&kWoodAxe);

    return id;
}

} // namespace items
