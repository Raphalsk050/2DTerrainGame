#pragma once

#include "item/item_def.h"

// The gold axe. Right for wood: planks, walls, and the trees themselves.
//
// Made of the gold dug out of the deep rock, so the head takes the gold row's own yellows
// against the same brown haft every tool here carries.
//
// It also fells trees faster, and that is not a second mechanic: `main.cpp` divides
// flora::kLogSeconds by the same tier multiplier BreakSeconds divides by, so an axe
// is worth exactly as much to a standing tree as it is to a plank wall.
//
// The fastest thing in the game and the most fragile, which is Minecraft's own trade and
// is the whole reason `tool::Kit` carries a lifetime beside the speed: gold goes through
// rock quicker than diamond does and is gone in thirty-two blows, which is barely half
// of what wood gives. It hits like wood too. It is a tool for one job in a hurry.
namespace items {

inline constexpr ItemDef kGoldAxe = {
    .name   = "gold axe",
    .colour = {169, 118, 26, 255},

    // A head four wide across the top narrowing to two at the bottom. All of it on one
    // side of the haft, which is what keeps it apart from the spade.
    .picture =
        {
            .tone = {{246, 215, 90, 255}, {169, 118, 26, 255}, {107, 67, 16, 255}, {150, 88, 48, 255}},
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
    .art = "blocks/tools/gold_axe",

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Axe, .speed = tool::kGold, .damage = 2, .lasts = tool::kGoldLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item GoldAxe() {
    static const Item id = item::Table().IdOf(&kGoldAxe);

    return id;
}

} // namespace items
