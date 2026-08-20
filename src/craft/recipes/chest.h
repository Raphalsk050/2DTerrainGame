#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: eight planks round an empty middle.
//
// The middle is what makes it a chest rather than a block of wood, and a bill of
// materials cannot say that — this game has no grid to arrange ingredients in and does
// not want one (see `ui/crafting.h` on why the strip is Don't Starve's and not
// Minecraft's). So what carries over is the *cost*, which is eight planks, and the
// shape stays a fact about the other game.
//
// Nothing had to be redone to the arithmetic here, unlike the stick (§28.4): a plank
// is a material this world already has and the count is one for one.
namespace recipes {

inline constexpr craft::RecipeDef kChest = {
    .name   = "chest",
    .makes  = "chest",
    .yields = 1,
    .needs  = {{.what = "wood plank", .count = 8}},
    .blurb  = "Boards and a lid. Thirty-two slots, and three will stand together as one.",
};

} // namespace recipes
