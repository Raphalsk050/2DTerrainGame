#pragma once

#include "item/item_def.h"

// The copper axe. Right for wood: planks, walls, and the trees themselves.
//
// Made of the copper dug out of the hillside, so the head takes the copper row's own
// warm reds against the same brown haft every tool here carries.
//
// It also fells trees faster, and that is not a second mechanic: `main.cpp` divides
// flora::kLogSeconds by the same tier multiplier BreakSeconds divides by, so an axe
// is worth exactly as much to a standing tree as it is to a plank wall.
//
// Between stone and iron in every respect, which is where the ore itself sits: it is
// found shallower than iron and worked harder than stone. The blow it lands is stone's
// and not a hair more — a point of damage is half a heart and there is no half step to
// give it — so what copper buys is the rate and the lifetime.
namespace items {

inline constexpr ItemDef kCopperAxe = {
    .name   = "copper axe",
    .colour = {160, 80, 58, 255},

    // A head four wide across the top narrowing to two at the bottom. All of it on one
    // side of the haft, which is what keeps it apart from the spade.
    .picture =
        {
            .tone = {{237, 164, 123, 255}, {160, 80, 58, 255}, {107, 47, 38, 255}, {150, 88, 48, 255}},
            .art =
                {
                    "aaab..",
                    "abbbd.",
                    "acbbd.",
                    "cc..d.",
                    "....d.",
                    "....d.",
                },
        },

    // Drawn from a file rather than from the four tones above. See `ItemDef::art` for
    // what that means and `item/icon.h` for how the two are drawn to the same size.
    .art = "blocks/tools/copper_axe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Axe, .speed = tool::kCopper, .damage = 3, .lasts = tool::kCopperLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item CopperAxe() {
    static const Item id = item::Table().IdOf(&kCopperAxe);

    return id;
}

} // namespace items
