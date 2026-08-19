#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: one log into four planks.
//
// The step that was missing, and its absence is what forced the stick beside it to
// be written at a rate nobody could check. `wood plank` was already a material —
// with a field, a threshold and a place in the exclusion order — and there was
// simply no way for a player to come by one. It could be laid in creative and
// nowhere else.
//
// **A recipe may make a material.** `craft::Named` resolves a name against the item
// table and then against the elements, so what comes out of this is nine blocks'
// worth of wall in the same slots that carry apples. That is the whole reason a
// `Stack` was written to hold either from the start.
namespace recipes {

inline constexpr craft::RecipeDef kWoodPlank = {
    .name   = "wood plank",
    .makes  = "wood plank",
    .yields = 4,
    .needs  = {{.what = "wood", .count = 1}},
    .blurb  = "A log split into boards. What everything wooden is cut from.",
};

} // namespace recipes
