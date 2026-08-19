#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: 3 cobble and 2 sticks.
//
// Taken from the wiki's own pickaxe article rather than from memory. Every tool in that game is the same
// two-part bill — so many units of the material and so many sticks — and the counts
// differ only by how much head the shape needs: a pickaxe and an axe take three, a
// sword takes two, a shovel takes one.
//
// Stone, cobble and every ore give way to it, and to nothing else.
namespace recipes {

inline constexpr craft::RecipeDef kStonePickaxe = {
    .name   = "stone pickaxe",
    .makes  = "stone pickaxe",
    .yields = 1,
    .needs  = {{.what = "cobblestone", .count = 3}, {.what = "stick", .count = 2}},
    .blurb  = "A worked edge on a haft. Twice the bite of the wooden one.",
};

} // namespace recipes
