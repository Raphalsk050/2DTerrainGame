#pragma once

#include "item/item_def.h"

// The diamond pickaxe. Right for stone, cobble and every ore.
//
// Made of the diamond found at the bottom of the world, so the head takes the diamond
// row's own pale cyans against the same brown haft every tool here carries.
//
// The end of the ladder, and it is the lifetime rather than the speed that says so: gold
// is half again as quick and lasts a fiftieth as long.
namespace items {

inline constexpr ItemDef kDiamondPickaxe = {
    .name   = "diamond pickaxe",
    .colour = {99, 220, 216, 255},

    // A bar across the top with a gap in the middle, so it reads as two points rather
    // than as a hammer. That gap is the whole silhouette: filled in, a pickaxe and a
    // shovel are the same picture.
    .picture =
        {
            .tone = {{159, 243, 236, 255}, {99, 220, 216, 255}, {63, 175, 184, 255}, {150, 88, 48, 255}},
            .art =
                {
                    "aa.aa.",
                    "abbba.",
                    ".cdc..",
                    "..d...",
                    "...d..",
                    "...d..",
                },
        },

    // Drawn from a file rather than from the four tones above. See `ItemDef::art` for
    // what that means and `item/icon.h` for how the two are drawn to the same size.
    .art = "blocks/tools/diamond_pickaxe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Pick, .speed = tool::kDiamond, .damage = 4, .lasts = tool::kDiamondLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item DiamondPickaxe() {
    static const Item id = item::Table().IdOf(&kDiamondPickaxe);

    return id;
}

} // namespace items
