#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: 2 planks and 1 stick.
//
// Taken from Minecraft's shape for it rather than from memory. Every tool in that game is the same
// two-part bill — so many units of the material and so many sticks — and the counts
// differ only by how much head the shape needs: a pickaxe and an axe take three, a
// sword takes two, a shovel takes one.
//
// Nothing in the ground gives way to it. It is for what moves.
namespace recipes {

inline constexpr craft::RecipeDef kWoodSword = {
    .name   = "wood sword",
    .makes  = "wood sword",
    .yields = 1,
    .needs  = {{.what = "wood plank", .count = 2}, {.what = "stick", .count = 1}},
    .blurb  = "Wood on wood. It will not last, and it is what there is first.",
};

} // namespace recipes
