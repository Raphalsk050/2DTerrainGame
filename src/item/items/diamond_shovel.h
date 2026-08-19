#pragma once

#include "item/item_def.h"

// The diamond shovel. Right for soil, sand and snow.
//
// Made of the diamond found at the bottom of the world, so the head takes the diamond
// row's own pale cyans against the same brown haft every tool here carries.
//
// The end of the ladder, and it is the lifetime rather than the speed that says so: gold
// is half again as quick and lasts a fiftieth as long.
namespace items {

inline constexpr ItemDef kDiamondShovel = {
    .name   = "diamond shovel",
    .colour = {99, 220, 216, 255},

    // A lozenge blade, widest across its middle. Kept clear of the axe's by being
    // symmetrical: an axe is all on one side of its haft and a spade is not.
    .picture =
        {
            .tone = {{159, 243, 236, 255}, {99, 220, 216, 255}, {63, 175, 184, 255}, {150, 88, 48, 255}},
            .art =
                {
                    ".aab..",
                    "aabbb.",
                    ".acb..",
                    "..d...",
                    "...d..",
                    "...d..",
                },
        },

    // Drawn from a file rather than from the four tones above. See `ItemDef::art` for
    // what that means and `item/icon.h` for how the two are drawn to the same size.
    .art = "blocks/tools/diamond_shovel",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Shovel, .speed = tool::kDiamond, .damage = 4, .lasts = tool::kDiamondLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item DiamondShovel() {
    static const Item id = item::Table().IdOf(&kDiamondShovel);

    return id;
}

} // namespace items
