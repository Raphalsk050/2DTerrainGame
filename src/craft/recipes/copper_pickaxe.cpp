#include "craft/recipes/copper_pickaxe.h"

// Puts the row beside this into the recipe table before main runs.
namespace {

const registry::Registrar<craft::RecipeDef> entry{recipes::kCopperPickaxe};

} // namespace
