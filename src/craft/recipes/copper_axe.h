#pragma once

#include "craft/recipe_def.h"

// Minecraft's own bill in Minecraft's own counts: 3 of the material and 2 sticks.
//
// **The ore itself and not an ingot**, which is the one place this parts company with the
// game the numbers come from. Smelting is a furnace, a fuel and a fire, and there is
// none of the three here yet — so a metal tool is made of the metal that came out of the
// ground, exactly as a stone one is made of the cobble that came out of the rock rather
// than of anything done to it afterwards. The day there is a furnace, this row gains an
// ingot and nothing else in the project has to move.
//
// Copper is shallow and plentiful, which is what makes it the first metal worth the walk.
//
// Planks, walls and standing trees give way to it.
namespace recipes {

inline constexpr craft::RecipeDef kCopperAxe = {
    .name   = "copper axe",
    .makes  = "copper axe",
    .yields = 1,
    .needs  = {{.what = "copper", .count = 3}, {.what = "stick", .count = 2}},
    .blurb  = "Beaten copper on a haft. Quicker than stone, and it lasts half again as long.",
};

} // namespace recipes
