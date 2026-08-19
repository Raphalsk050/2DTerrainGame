#pragma once

#include "item/item_def.h"

// The copper pickaxe. Right for stone, cobble and every ore.
//
// Made of the copper dug out of the hillside, so the head takes the copper row's own
// warm reds against the same brown haft every tool here carries.
//
// Between stone and iron in every respect, which is where the ore itself sits: it is
// found shallower than iron and worked harder than stone. The blow it lands is stone's
// and not a hair more — a point of damage is half a heart and there is no half step to
// give it — so what copper buys is the rate and the lifetime.
namespace items {

inline constexpr ItemDef kCopperPickaxe = {
    .name   = "copper pickaxe",
    .colour = {160, 80, 58, 255},

    // A bar across the top with a gap in the middle, so it reads as two points rather
    // than as a hammer. That gap is the whole silhouette: filled in, a pickaxe and a
    // shovel are the same picture.
    .picture =
        {
            .tone = {{237, 164, 123, 255}, {160, 80, 58, 255}, {107, 47, 38, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/copper_pickaxe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Pick, .speed = tool::kCopper, .damage = 2, .lasts = tool::kCopperLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item CopperPickaxe() {
    static const Item id = item::Table().IdOf(&kCopperPickaxe);

    return id;
}

} // namespace items
