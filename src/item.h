#pragma once

#include "picture.h"
#include "raylib.h"

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

enum class Item { Wood, Sapling, Apple, Resin, Fibre, Count };

inline constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::Count);

inline constexpr std::size_t ItemIndex(Item item) {
    return static_cast<std::size_t>(item);
}

struct ItemDef {
    const char *name;

    // The one colour to use where only one will fit — a marker on a map, a tint
    // on a label. Not what the item is drawn from; that is the picture.
    Color colour;

    Picture picture;

    int stack;
};

inline constexpr ItemDef kItems[] = {
    {
        .name   = "wood",
        .colour = {138, 92, 52, 255},

        // Bark lit from above, bark in shadow, the dark underside, and the pale
        // cut face — which is the one mark that says log rather than stick.
        .picture =
            {
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
            },
        .stack = 64,
    },
    {
        .name   = "sapling",
        .colour = {96, 168, 74, 255},

        // Two leaves, the stem, and the soil still on its root.
        .picture =
            {
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
            },
        .stack = 64,
    },
    {
        .name   = "apple",
        .colour = {214, 66, 58, 255},

        // Round, lit from the upper left, with the stalk still in it.
        .picture =
            {
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
            },
        .stack = 64,
    },
    {
        .name   = "resin",
        .colour = {228, 176, 68, 255},

        // A bead that has run and set: heavier at the bottom, with the highlight
        // near the top where the surface turns over.
        .picture =
            {
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
            },
        .stack = 64,
    },
    {
        .name   = "fibre",
        .colour = {186, 176, 120, 255},

        // A hank of stripped bark, gathered in the middle. The tie is what stops
        // it reading as a smudge.
        .picture =
            {
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
            },
        .stack = 64,
    },
};

static_assert(std::size(kItems) == kItemCount, "every Item needs exactly one row in kItems");

inline constexpr bool ItemPicturesAreSquare() {
    for (const ItemDef &def : kItems) {
        if (!IsSquare(def.picture)) return false;
    }

    return true;
}

static_assert(ItemPicturesAreSquare(), "every item picture is six rows of six characters");

inline constexpr const ItemDef &Def(Item item) {
    return kItems[ItemIndex(item)];
}
