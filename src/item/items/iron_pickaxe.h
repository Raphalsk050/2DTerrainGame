#pragma once

#include "item/item_def.h"

// The iron pickaxe. Right for stone, cobble and every ore.
//
// Made of the iron dug out of the deep rock, so the head takes the iron row's own cold
// blues against the same brown haft every tool here carries.
//
// Minecraft's middle of the ladder and the tool most of a game is played with: half again
// the bite of stone and near twice the lifetime.
namespace items {

inline constexpr ItemDef kIronPickaxe = {
    .name   = "iron pickaxe",
    .colour = {100, 107, 136, 255},

    // A bar across the top with a gap in the middle, so it reads as two points rather
    // than as a hammer. That gap is the whole silhouette: filled in, a pickaxe and a
    // shovel are the same picture.
    .picture =
        {
            .tone = {{177, 191, 207, 255}, {100, 107, 136, 255}, {74, 80, 104, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/iron_pickaxe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Pick, .speed = tool::kIron, .damage = 3, .lasts = tool::kIronLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item IronPickaxe() {
    static const Item id = item::Table().IdOf(&kIronPickaxe);

    return id;
}

} // namespace items
