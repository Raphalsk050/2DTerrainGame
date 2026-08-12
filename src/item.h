#pragma once

#include "raylib.h"

#include <array>
#include <cstddef>
#include <iterator>

// Things a plant leaves behind when it is cut down or picked.
//
// Deliberately not rows in the element table. An element is a material the world
// is made of: it has a field over the lattice, a threshold, a rank against every
// other material and a rule for where it generates. An apple has none of those
// and would need all four invented for it, which is how a good table stops being
// one. What an item has is a name, a picture and a count, and this is the table
// of those.

// Side of an item's picture, in plant pixels.
//
// Six, which at the plant grid is twelve world pixels — the width of the
// character. An item lying on the ground is not trying to be to scale; it is
// trying to be recognised at a glance from across a clearing, and a log drawn to
// scale against a tree would be four pixels of brown.
inline constexpr int kItemArt = 6;

// How many tones one item is drawn from. Four is what these sizes carry: a lit
// face, a body, a shadow and one accent, which is exactly the vocabulary the
// canopy uses and for the same reason.
inline constexpr std::size_t kItemTones = 4;

enum class Item { Wood, Sapling, Apple, Resin, Fibre, Count };

inline constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::Count);

inline constexpr std::size_t ItemIndex(Item item) { return static_cast<std::size_t>(item); }

struct ItemDef {
    const char *name;

    // The one colour to use where only one will fit — a marker on a map, a tint
    // on a label. Not what the item is drawn from; that is the tones below.
    Color colour;

    // Lit face, body, shadow, accent. Every picture in the table is drawn from
    // its own four and nothing else, so recolouring an item is one row.
    Color tone[kItemTones];

    // The picture, one row of characters per row of pixels: `a` through `d` are
    // the tones above and a full stop is nothing.
    //
    // Written out rather than generated. A log and an apple have nothing in
    // common to generate *from*, and at six pixels a side the whole picture is
    // thirty-six characters — which is smaller, and far easier to change by eye,
    // than any rule that could have produced it.
    const char *art[kItemArt];

    int stack;
};

inline constexpr ItemDef kItems[] = {
    {
        .name   = "wood",
        .colour = {138, 92, 52, 255},

        // Bark lit from above, bark in shadow, the dark underside, and the pale
        // cut face — which is the one mark that says log rather than stick.
        .tone = {{166, 118, 68, 255}, {124, 82, 46, 255}, {82, 52, 28, 255}, {206, 174, 128, 255}},
        .art =
            {
                "......",
                ".daaa.",
                "ddabbb",
                "ddbbbc",
                ".dccc.",
                "......",
            },
        .stack = 999,
    },
    {
        .name   = "sapling",
        .colour = {96, 168, 74, 255},

        // Two leaves, the stem, and the soil still on its root.
        .tone = {{132, 202, 96, 255}, {74, 142, 62, 255}, {120, 92, 54, 255}, {70, 52, 34, 255}},
        .art =
            {
                "......",
                ".a..a.",
                ".ab.a.",
                "..bc..",
                "..c...",
                ".ddd..",
            },
        .stack = 99,
    },
    {
        .name   = "apple",
        .colour = {214, 66, 58, 255},

        // Round, lit from the upper left, with the stalk still in it.
        .tone = {{226, 92, 78, 255}, {176, 46, 44, 255}, {118, 28, 32, 255}, {96, 152, 70, 255}},
        .art =
            {
                "..dc..",
                ".aab..",
                "aaabb.",
                "aaabb.",
                ".abbc.",
                "..cc..",
            },
        .stack = 99,
    },
    {
        .name   = "resin",
        .colour = {228, 176, 68, 255},

        // A bead that has run and set: heavier at the bottom, with the highlight
        // near the top where the surface turns over.
        .tone = {{248, 214, 122, 255}, {216, 160, 56, 255}, {156, 104, 30, 255}, {255, 244, 206, 255}},
        .art =
            {
                "..a...",
                "..da..",
                ".aabb.",
                ".abbbc",
                "..bbc.",
                "..cc..",
            },
        .stack = 99,
    },
    {
        .name   = "fibre",
        .colour = {186, 176, 120, 255},

        // A hank of stripped bark, gathered in the middle. The tie is what stops
        // it reading as a smudge.
        .tone = {{214, 206, 154, 255}, {170, 158, 104, 255}, {124, 112, 70, 255}, {148, 116, 66, 255}},
        .art =
            {
                ".a..a.",
                ".ab.b.",
                "..aba.",
                "..dd..",
                ".ab.a.",
                ".b..b.",
            },
        .stack = 999,
    },
};

static_assert(std::size(kItems) == kItemCount, "every Item needs exactly one row in kItems");

inline constexpr const ItemDef &Def(Item item) { return kItems[ItemIndex(item)]; }

// How many of each item something gave up.
//
// The same shape as World::Yield and for the same reason: what a harvest is, is
// a count per kind, and the caller decides what the count is worth.
using Harvest = std::array<int, kItemCount>;
