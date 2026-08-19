#pragma once

#include "item/item_def.h"

// The wood pickaxe. Right for stone, cobble and every ore.
//
// Made of planks, so the head takes the plank row's own pale tones and the haft takes a
// brown darker than any of them. A wooden tool whose head and handle were the same
// colour would be a brown smear.
namespace items {

inline constexpr ItemDef kWoodPickaxe = {
    .name   = "wood pickaxe",
    .colour = {170, 128, 78, 255},

    // A bar across the top with a gap in the middle, so it reads as two points
    // rather than as a hammer. That gap is the whole silhouette: filled in, a pickaxe
    // and a shovel are the same picture.
    .picture =
        {
            .tone = {{198, 160, 106, 255}, {170, 128, 78, 255}, {138, 98, 56, 255}, {90, 58, 32, 255}},
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
    .art = "blocks/tools/wood_pickaxe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Pick, .speed = tool::kWood, .damage = 1, .lasts = tool::kWoodLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item WoodPickaxe() {
    static const Item id = item::Table().IdOf(&kWoodPickaxe);

    return id;
}

} // namespace items
