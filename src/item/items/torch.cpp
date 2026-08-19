#include "item/items/torch.h"

// Puts the row beside this into the item table before main runs.
//
// One line, in the row's own file. Nothing else in the project learns that
// torch exists — not a table, not an enum, not the build, which globs this
// directory rather than listing it.
namespace {

const registry::Registrar<ItemDef> entry{items::kTorch};

} // namespace
