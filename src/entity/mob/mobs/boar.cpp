#include "entity/mob/mobs/boar.h"

// Puts the row beside this into the creature table before main runs.
//
// One line, in the creature's own file. Nothing else in the project learns that a
// boar exists — not a table, not an enum, not the build, which globs this directory
// rather than listing it.
namespace {

const registry::Registrar<mob::Def> entry{mobs::kBoar};

} // namespace
