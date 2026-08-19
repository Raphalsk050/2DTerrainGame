#include "craft/recipes/wood_plank.h"

// Puts the row beside this into the recipe table before main runs.
namespace {

const registry::Registrar<craft::RecipeDef> entry{recipes::kWoodPlank};

} // namespace
