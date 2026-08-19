#pragma once

#include "item/item_def.h"

// The copper shovel. Right for soil, sand and snow.
//
// Made of the copper dug out of the hillside, so the head takes the copper row's own
// warm reds against the same brown haft every tool here carries.
//
// Between stone and iron in every respect, which is where the ore itself sits: it is
// found shallower than iron and worked harder than stone. The blow it lands is stone's
// and not a hair more — a point of damage is half a heart and there is no half step to
// give it — so what copper buys is the rate and the lifetime.
namespace items {

inline constexpr ItemDef kCopperShovel = {
    .name   = "copper shovel",
    .colour = {160, 80, 58, 255},

    // A lozenge blade, widest across its middle. Kept clear of the axe's by being
    // symmetrical: an axe is all on one side of its haft and a spade is not.
    .picture =
        {
            .tone = {{237, 164, 123, 255}, {160, 80, 58, 255}, {107, 47, 38, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/copper_shovel",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Shovel, .speed = tool::kCopper, .damage = 2, .lasts = tool::kCopperLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item CopperShovel() {
    static const Item id = item::Table().IdOf(&kCopperShovel);

    return id;
}

} // namespace items
