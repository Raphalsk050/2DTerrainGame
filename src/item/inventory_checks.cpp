#include "core/registry.h"
#include "item/inventory.h"
#include "world/element.h"

#include <string>

// Everything in the game has to fit on one page of the creative palette.
//
// A `static_assert` until the tables started assembling themselves, and the reason
// it is worth keeping at all is that the failure is silent: nothing errors, nothing
// warns, the palette simply stops listing whatever was added last — and the row
// looks perfectly fine in the file it was added to.
//
// The fix when this fires is another tab and not a scrollbar. See `Inventory::Tab`,
// where the rule that sorts a thing onto a page lives.
namespace {

std::string PalettePagesAreBigEnough() {
    std::string wrong;

    if (item::Count() > Inventory::kSlotsPerPage) {
        wrong += "the items no longer fit on one page of the palette (" + std::to_string(item::Count())
                 + " of " + std::to_string(Inventory::kSlotsPerPage) + ")";
    }

    if (static_cast<int>(kElementCount) > Inventory::kSlotsPerPage) {
        if (!wrong.empty()) wrong += "; ";

        wrong += "the blocks no longer fit on one page of the palette (" + std::to_string(kElementCount)
                 + " of " + std::to_string(Inventory::kSlotsPerPage) + ")";
    }

    return wrong;
}

const registry::Checker fits{PalettePagesAreBigEnough};

} // namespace
