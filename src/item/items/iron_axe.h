#pragma once

#include "item/item_def.h"

// The iron axe. Right for wood: planks, walls, and the trees themselves.
//
// Made of the iron dug out of the deep rock, so the head takes the iron row's own cold
// blues against the same brown haft every tool here carries.
//
// It also fells trees faster, and that is not a second mechanic: `main.cpp` divides
// flora::kLogSeconds by the same tier multiplier BreakSeconds divides by, so an axe
// is worth exactly as much to a standing tree as it is to a plank wall.
//
// Minecraft's middle of the ladder and the tool most of a game is played with: half again
// the bite of stone and near twice the lifetime.
namespace items {

inline constexpr ItemDef kIronAxe = {
    .name   = "iron axe",
    .colour = {100, 107, 136, 255},

    // A head four wide across the top narrowing to two at the bottom. All of it on one
    // side of the haft, which is what keeps it apart from the spade.
    .picture =
        {
            .tone = {{177, 191, 207, 255}, {100, 107, 136, 255}, {74, 80, 104, 255}, {150, 88, 48, 255}},
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

    // Drawn from a file rather than from the four tones above. See `ItemDef::art` for
    // what that means and `item/icon.h` for how the two are drawn to the same size.
    .art = "blocks/tools/iron_axe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Axe, .speed = tool::kIron, .damage = 4, .lasts = tool::kIronLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item IronAxe() {
    static const Item id = item::Table().IdOf(&kIronAxe);

    return id;
}

} // namespace items
