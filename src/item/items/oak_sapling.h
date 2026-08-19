#pragma once

#include "item/item_def.h"

// One sapling per tree, and not one sapling that becomes whichever tree the
// climate favours.
//
// The single sapling was a shortcut wearing the clothes of a design: it read as
// "the world knows what belongs here", and what it actually meant was that a
// player could not choose. Minecraft has nine of these, every one dropped by the
// leaves of its own tree and every one plantable on any dirt in any biome — a
// jungle sapling grows in a snowfield. What the climate decides is what *grows on
// its own*, which is the scatter's business and stays there; what a player plants
// is what a player is holding.
//
// Tones throughout the four saplings: a lit leaf, b shaded leaf, c the stem, d the
// soil still on the root.
namespace items {

inline constexpr ItemDef kOakSapling = {
    .name   = "oak sapling",
    .colour = {96, 182, 74, 255},

    // A rounded pair of leaves, the shape an oak's crown keeps all its life.
    .picture =
        {
            .tone = {{150, 214, 102, 255}, {58, 138, 58, 255}, {107, 68, 35, 255}, {70, 52, 34, 255}},
            .art =
                {
                    "..a...",
                    ".aab..",
                    "aabba.",
                    ".abc..",
                    "..c...",
                    ".ddd..",
                },
        },
    .stack     = 64,
    .placement = Placement::Plant,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item OakSapling() {
    static const Item id = item::Table().IdOf(&kOakSapling);

    return id;
}

} // namespace items
