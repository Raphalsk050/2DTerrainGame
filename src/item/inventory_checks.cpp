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
//
// ---
//
// **It counts pages and not tables**, and that is a correction rather than a tidy-up.
// It used to weigh the whole item table against one page, which was a fair proxy while
// there were nineteen items and no tab held more than half of them. Twelve tools
// arrived in one commit, the item table went to thirty-one, and the check failed a
// build in which not one page was more than three quarters full — a check that refuses
// a correct change is a check that gets deleted, and the next silent overflow goes
// unnoticed.
//
// The count comes from `Inventory::PageOf` itself rather than from a copy of the rule
// that sorts things onto pages. That rule lives in exactly one place and this must not
// become the second: a check that agreed with a private copy of the sort would pass
// while the palette it is checking dropped rows.
namespace {

std::string PalettePagesAreBigEnough() {
    std::string wrong;

    for (int t = 0; t < Inventory::kTabs; t++) {
        const auto tab = static_cast<Inventory::Tab>(t);

        const Inventory::Page page = Inventory::PageOf(tab);

        if (page.wanted <= Inventory::kSlotsPerPage) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("the palette's '") + Inventory::NameOf(tab) + "' tab no longer fits on one page ("
                 + std::to_string(page.wanted) + " of " + std::to_string(Inventory::kSlotsPerPage) + ")";
    }

    return wrong;
}

const registry::Checker fits{PalettePagesAreBigEnough};

} // namespace
