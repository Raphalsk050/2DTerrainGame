#pragma once

#include "craft/recipe_def.h"

// Minecraft's own: two planks into four sticks.
//
// **This replaces one wood into eight sticks**, and the change is the point rather
// than a retune. That row existed because there was no plank to spend: a log is
// worth four planks and two planks are worth four sticks, so a log is worth eight
// sticks — and with the middle step missing the only honest thing to write was the
// end of the chain, one wood into eight. It was the right number for the wrong
// reason, and it let a player skip a step Minecraft charges for.
//
// The rate through the whole chain is unchanged: one wood still ends up eight
// sticks. What it costs now is two clicks instead of one, which is what the plank
// step *is*.
namespace recipes {

inline constexpr craft::RecipeDef kStick = {
    .name   = "stick",
    .makes  = "stick",
    .yields = 4,
    .needs  = {{.what = "wood plank", .count = 2}},
    .blurb  = "Two boards split down. Everything with a handle starts here.",
};

} // namespace recipes
