#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: 1 cobble and 2 sticks.
//
// Taken from Minecraft's shape for it rather than from memory. Every tool in that game is the same
// two-part bill — so many units of the material and so many sticks — and the counts
// differ only by how much head the shape needs: a pickaxe and an axe take three, a
// sword takes two, a shovel takes one.
//
// Soil, sand and snow give way to it.
namespace recipes {

inline constexpr craft::RecipeDef kStoneShovel = {
    .name   = "stone shovel",
    .makes  = "stone shovel",
    .yields = 1,
    .needs  = {{.what = "cobblestone", .count = 1}, {.what = "stick", .count = 2}},
    .blurb  = "A worked edge on a haft. Twice the bite of the wooden one.",
};

} // namespace recipes
