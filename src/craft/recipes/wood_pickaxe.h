#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: 3 planks and 2 sticks.
//
// Taken from the wiki's own pickaxe article rather than from memory. Every tool in that game is the same
// two-part bill — so many units of the material and so many sticks — and the counts
// differ only by how much head the shape needs: a pickaxe and an axe take three, a
// sword takes two, a shovel takes one.
//
// Stone, cobble and every ore give way to it, and to nothing else.
namespace recipes {

inline constexpr craft::RecipeDef kWoodPickaxe = {
    .name   = "wood pickaxe",
    .makes  = "wood pickaxe",
    .yields = 1,
    .needs  = {{.what = "wood plank", .count = 3}, {.what = "stick", .count = 2}},
    .blurb  = "Wood on wood. It will not last, and it is what there is first.",
};

} // namespace recipes
