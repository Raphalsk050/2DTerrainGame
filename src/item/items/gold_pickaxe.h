#pragma once

#include "item/item_def.h"

// The gold pickaxe. Right for stone, cobble and every ore.
//
// Made of the gold dug out of the deep rock, so the head takes the gold row's own yellows
// against the same brown haft every tool here carries.
//
// The fastest thing in the game and the most fragile, which is Minecraft's own trade and
// is the whole reason `tool::Kit` carries a lifetime beside the speed: gold goes through
// rock quicker than diamond does and is gone in thirty-two blows, which is barely half
// of what wood gives. It hits like wood too. It is a tool for one job in a hurry.
namespace items {

inline constexpr ItemDef kGoldPickaxe = {
    .name   = "gold pickaxe",
    .colour = {169, 118, 26, 255},

    // A bar across the top with a gap in the middle, so it reads as two points rather
    // than as a hammer. That gap is the whole silhouette: filled in, a pickaxe and a
    // shovel are the same picture.
    .picture =
        {
            .tone = {{246, 215, 90, 255}, {169, 118, 26, 255}, {107, 67, 16, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/gold_pickaxe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Pick, .speed = tool::kGold, .damage = 1, .lasts = tool::kGoldLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item GoldPickaxe() {
    static const Item id = item::Table().IdOf(&kGoldPickaxe);

    return id;
}

} // namespace items
