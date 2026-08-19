#pragma once

#include "item/item_def.h"

// The gold shovel. Right for soil, sand and snow.
//
// Made of the gold dug out of the deep rock, so the head takes the gold row's own yellows
// against the same brown haft every tool here carries.
//
// The fastest thing in the game and the most fragile, which is Minecraft's own trade and
// is the whole reason `tool::Kit` carries a lifetime beside the speed: gold goes through
// rock quicker than diamond does and is gone in thirty-two blows, which is barely half
// of what wood gives. It hits like wood too. It is a tool for one job in a hurry.
namespace items {

inline constexpr ItemDef kGoldShovel = {
    .name   = "gold shovel",
    .colour = {169, 118, 26, 255},

    // A lozenge blade, widest across its middle. Kept clear of the axe's by being
    // symmetrical: an axe is all on one side of its haft and a spade is not.
    .picture =
        {
            .tone = {{246, 215, 90, 255}, {169, 118, 26, 255}, {107, 67, 16, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/gold_shovel",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Shovel, .speed = tool::kGold, .damage = 1, .lasts = tool::kGoldLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item GoldShovel() {
    static const Item id = item::Table().IdOf(&kGoldShovel);

    return id;
}

} // namespace items
