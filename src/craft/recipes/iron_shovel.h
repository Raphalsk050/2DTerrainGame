#pragma once

#include "craft/recipe_def.h"

// Minecraft's own bill in Minecraft's own counts: 1 of the material and 2 sticks.
//
// **The ore itself and not an ingot**, which is the one place this parts company with the
// game the numbers come from. Smelting is a furnace, a fuel and a fire, and there is
// none of the three here yet — so a metal tool is made of the metal that came out of the
// ground, exactly as a stone one is made of the cobble that came out of the rock rather
// than of anything done to it afterwards. The day there is a furnace, this row gains an
// ingot and nothing else in the project has to move.
//
// Iron is the middle of the ladder and the metal most of a life is spent holding.
//
// Soil, sand and snow give way to it.
namespace recipes {

inline constexpr craft::RecipeDef kIronShovel = {
    .name   = "iron shovel",
    .makes  = "iron shovel",
    .yields = 1,
    .needs  = {{.what = "iron", .count = 1}, {.what = "stick", .count = 2}},
    .blurb  = "Iron. Three times the bite of wood, and it will see out a hillside.",
};

} // namespace recipes
