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

// One sapling per tree and not one sapling that becomes whichever tree the
// climate there favours.
//
// The single sapling was a shortcut wearing the clothes of a design: it read as
// "the world knows what belongs here", and what it actually meant was that a
// player could not choose. Minecraft has nine of these, every one of them dropped
// by the leaves of its own tree and every one of them plantable on any dirt in
// any biome — a jungle sapling grows in a snowfield. What the climate decides is
// what *grows on its own*, which is the scatter's business and stays there; what
// a player plants is what a player is holding.
enum class Item { Wood, OakSapling, PineSapling, BirchSapling, AppleSapling, Apple, Resin, Fibre, Count };

inline constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::Count);

inline constexpr std::size_t ItemIndex(Item item) {
    return static_cast<std::size_t>(item);
}

// What an item does when the right hand puts it into the world.
//
// A row on the table rather than a test in the editor, and that is the point:
// what "the player can put this down" means has to be one fact in one place, or
// the hand that places it, the hand that decides whether it can be placed, and
// the ghost that shows where it would go all end up asking different questions.
// Adding a torch or a workbench later is a row here and nothing else.
//
// Nothing to do with materials. A material is terrain, it goes down under a
// brush by the fistful, and it has an element rather than an item.
enum class Placement {
    // Only ever carried. Wood, apples, resin, fibre.
    None,

    // Takes root standing on the ground under the cursor, and grows.
    Plant,
};

struct ItemDef {
    const char *name;

    // The one colour to use where only one will fit — a marker on a map, a tint
    // on a label. Not what the item is drawn from; that is the picture.
    Color colour;

    Picture picture;

    int stack;

    Placement placement = Placement::None;
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
    // The four saplings. Each is drawn from its own tree's greens and its own
    // tree's bark — see kSpecies — so a row of them in the bar is four
    // recognisably different plants rather than one picture repeated in four
    // slots. The shapes differ too, and along the axis the trees themselves
    // differ: the oak is round, the pine is tiered and narrow, the birch is
    // slender on a pale stem, the apple carries a blossom.
    //
    // Tones throughout: a lit leaf, b shaded leaf, c the stem, d the soil still
    // on the root — except the apple, whose d is its blossom and whose stem
    // colour does for both.
    {
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
    },
    {
        .name   = "pine sapling",
        .colour = {92, 124, 56, 255},

        // Two tiers of needles on a leader, which is the conifer in miniature.
        .picture =
            {
                .tone = {{130, 158, 74, 255}, {62, 92, 42, 255}, {96, 62, 34, 255}, {70, 52, 34, 255}},
                .art =
                    {
                        "..a...",
                        ".aab..",
                        "..bc..",
                        ".aab..",
                        "..c...",
                        ".ddd..",
                    },
            },
        .stack     = 64,
        .placement = Placement::Plant,
    },
    {
        .name   = "birch sapling",
        .colour = {126, 196, 92, 255},

        // Thin and open, on the one pale stem in the wood.
        .picture =
            {
                .tone = {{172, 224, 124, 255}, {88, 158, 72, 255}, {198, 200, 194, 255}, {70, 52, 34, 255}},
                .art =
                    {
                        ".a..a.",
                        ".ab.b.",
                        "..ab..",
                        "..c...",
                        "..c...",
                        ".ddd..",
                    },
            },
        .stack     = 64,
        .placement = Placement::Plant,
    },
    {
        .name   = "apple sapling",
        .colour = {96, 156, 70, 255},

        // In blossom already, which is the one thing that tells it from the oak
        // at this size — the two trees are the same broadleaf shape.
        .picture =
            {
                .tone = {{140, 196, 92, 255}, {58, 116, 52, 255}, {110, 74, 42, 255}, {244, 222, 232, 255}},
                .art =
                    {
                        "..d...",
                        ".aaba.",
                        ".abba.",
                        "..bc..",
                        "..c...",
                        ".ccc..",
                    },
            },
        .stack     = 64,
        .placement = Placement::Plant,
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
