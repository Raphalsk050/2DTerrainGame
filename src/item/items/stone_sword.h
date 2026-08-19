#pragma once

#include "item/item_def.h"

// The stone sword. Right for nothing in the ground at all.
//
// Made of cobblestone, so the head takes the cobble row's greys — the recipe spends cobble
// and the picture had better agree — against a wooden haft.
// **It breaks nothing faster and that is correct.** No material in the table asks
// for `Tool::Sword`, so BreakSeconds gives it the bare-hand rate against everything
// — which is Minecraft's arrangement too. What it is for is the swing, and its
// `damage` is the whole of what it does.
namespace items {

inline constexpr ItemDef kStoneSword = {
    .name   = "stone sword",
    .colour = {130, 130, 136, 255},

    // A blade up the diagonal with the guard across it, which is the one mark that
    // separates a sword from a stick at this size — take the guard away and it is the
    // stick row next door in a lighter colour.
    .picture =
        {
            .tone = {{162, 162, 168, 255}, {130, 130, 136, 255}, {74, 74, 80, 255}, {138, 92, 52, 255}},
            .art =
                {
                    "....ab",
                    "...ab.",
                    "..ab..",
                    ".dcd..",
                    "..d...",
                    "..d...",
                },
        },

    // One to a slot, as every tool in Minecraft is. A stack of nine identical
    // pickaxes is a thing no player has ever wanted and nine slots of the bar gone.
    .stack = 1,

    // Carried and swung, never put into the world: `Placement::None`. A tool going
    // down as a block is the one thing a right click must not do with it.
    .placement = Placement::None,

    .tool = {.kind = Tool::Sword, .speed = tool::kStone, .damage = 8, .lasts = tool::kStoneLasts},
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item StoneSword() {
    static const Item id = item::Table().IdOf(&kStoneSword);

    return id;
}

} // namespace items
