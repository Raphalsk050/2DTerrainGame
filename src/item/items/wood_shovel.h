#pragma once

#include "item/item_def.h"

// The wood shovel. Right for soil, sand and snow.
//
// Made of planks, so the head takes the plank row's own pale tones and the haft takes a
// brown darker than any of them. A wooden tool whose head and handle were the same
// colour would be a brown smear.
namespace items {

inline constexpr ItemDef kWoodShovel = {
    .name   = "wood shovel",
    .colour = {170, 128, 78, 255},

    // A lozenge blade, widest across its middle. Kept clear of the axe's by being
    // symmetrical: an axe is all on one side of its haft and a spade is not.
    .picture =
        {
            .tone = {{198, 160, 106, 255}, {170, 128, 78, 255}, {138, 98, 56, 255}, {90, 58, 32, 255}},
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

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Shovel, .speed = tool::kWood, .damage = 1},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item WoodShovel() {
    static const Item id = item::Table().IdOf(&kWoodShovel);

    return id;
}

} // namespace items
