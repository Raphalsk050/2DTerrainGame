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
// Gold is the fastest thing there is and the shortest lived — thirty-two blows, barely
// half of what a wooden one gives. Worth making for one job in a hurry and not for a day's
// work.
//
// Stone, cobble and every ore give way to it, and to nothing else.
namespace recipes {

inline constexpr craft::RecipeDef kGoldPickaxe = {
    .name   = "gold pickaxe",
    .makes  = "gold pickaxe",
    .yields = 1,
    .needs  = {{.what = "gold", .count = 3}, {.what = "stick", .count = 2}},
    .blurb  = "Soft, quick and gone. Six times the bite of wood, for thirty-two swings.",
};

} // namespace recipes
