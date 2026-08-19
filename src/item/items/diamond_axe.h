#pragma once

#include "item/item_def.h"

// The diamond axe. Right for wood: planks, walls, and the trees themselves.
//
// Made of the diamond found at the bottom of the world, so the head takes the diamond
// row's own pale cyans against the same brown haft every tool here carries.
//
// It also fells trees faster, and that is not a second mechanic: `main.cpp` divides
// flora::kLogSeconds by the same tier multiplier BreakSeconds divides by, so an axe
// is worth exactly as much to a standing tree as it is to a plank wall.
//
// The end of the ladder, and it is the lifetime rather than the speed that says so: gold
// is half again as quick and lasts a fiftieth as long.
namespace items {

inline constexpr ItemDef kDiamondAxe = {
    .name   = "diamond axe",
    .colour = {99, 220, 216, 255},

    // A head four wide across the top narrowing to two at the bottom. All of it on one
    // side of the haft, which is what keeps it apart from the spade.
    .picture =
        {
            .tone = {{159, 243, 236, 255}, {99, 220, 216, 255}, {63, 175, 184, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/diamond_axe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Axe, .speed = tool::kDiamond, .damage = 5, .lasts = tool::kDiamondLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item DiamondAxe() {
    static const Item id = item::Table().IdOf(&kDiamondAxe);

    return id;
}

} // namespace items
