#include "craft/recipes/iron_shovel.h"

// Puts the row beside this into the recipe table before main runs.
namespace {

const registry::Registrar<craft::RecipeDef> entry{recipes::kIronShovel};

} // namespace
