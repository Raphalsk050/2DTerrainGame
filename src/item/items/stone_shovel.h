#pragma once

#include "item/item_def.h"

// The stone shovel. Right for soil, sand and snow.
//
// Made of cobblestone, so the head takes the cobble row's greys — the recipe spends cobble
// and the picture had better agree — against a wooden haft.
namespace items {

inline constexpr ItemDef kStoneShovel = {
    .name   = "stone shovel",
    .colour = {130, 130, 136, 255},

    // A lozenge blade, widest across its middle. Kept clear of the axe's by being
    // symmetrical: an axe is all on one side of its haft and a spade is not.
    .picture =
        {
            .tone = {{162, 162, 168, 255}, {130, 130, 136, 255}, {74, 74, 80, 255}, {138, 92, 52, 255}},
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

    .tool = {.kind = Tool::Shovel, .speed = tool::kStone, .damage = 2},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item StoneShovel() {
    static const Item id = item::Table().IdOf(&kStoneShovel);

    return id;
}

} // namespace items
