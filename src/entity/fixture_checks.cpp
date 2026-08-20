#include "core/registry.h"
#include "entity/fixture.h"
#include "item/item_def.h"
#include "item/slots.h"

#include <string>

// The joins between the fixture table and the item table, checked at startup.
//
// These were `static_assert`s and they were worth having: CLAUDE.md §16.2b is the
// account of what they caught, and of the day spent finding the fault they were
// added to make impossible. A table assembled at run time cannot carry them as
// compile errors, so they are here, run by `content::Open` before the window opens,
// and a failure stops the program.
//
// Two faults, and they are different:
//
//   - **A name that resolves to nothing.** The trap the old string match had, with
//     the fuse now cut: renaming an item in a commit about wording used to leave a
//     torch that could no longer be placed, and nothing in either file said the
//     names had to agree. It is a startup failure naming both rows now.
//   - **A name that resolves to the wrong sort of thing.** A fixture whose item is
//     not `Placement::Fixture` is a thing that can be carried and never put down,
//     and the only way to find that out was to carry it and click.
namespace {

std::string FixturesNameRealItems() {
    std::string wrong;

    for (std::size_t k = 0; k < fixture::kKindCount; k++) {
        const fixture::Def &def = fixture::kKinds[k];

        if (def.from == nullptr) {
            if (!wrong.empty()) wrong += "; ";

            wrong += std::string("fixture '") + def.name + "' names no item to be put up from";

            continue;
        }

        const std::optional<Item> from = item::Find(def.from);

        if (!from.has_value()) {
            if (!wrong.empty()) wrong += "; ";

            wrong += std::string("fixture '") + def.name + "' is put up from '" + def.from
                     + "', and there is no such item";

            continue;
        }

        if (Def(*from).placement != Placement::Fixture) {
            if (!wrong.empty()) wrong += "; ";

            wrong += std::string("fixture '") + def.name + "' names item '" + def.from
                     + "', whose placement is not Fixture — the hand will never put it down";
        }
    }

    return wrong;
}

const registry::Checker join{FixturesNameRealItems};

// A store's slots have to be a whole number of rows, and a bank of them has to fit
// one `slots::Bank`.
//
// Both are silent failures and neither is arithmetic anybody would check by playing.
// A chest of thirty slots would give its last row six of the eight the grid draws, so
// the two slots past the end would be drawn and unreachable — and worse, the row a
// sorting rule is about would straddle two chests, which is the one thing
// `slots::kAcross` exists to stop (see its comment, and `Fixtures::SetRule`). A row
// that joins more units than a bank has parts would simply lose the last chest of the
// run, with the player's things in it.
std::string StoresAreWholeRows() {
    std::string wrong;

    const auto fault = [&wrong](const std::string &said) {
        if (!wrong.empty()) wrong += "; ";

        wrong += said;
    };

    for (std::size_t k = 0; k < fixture::kKindCount; k++) {
        const fixture::Def &def = fixture::kKinds[k];

        if (def.slots < 0) fault(std::string("fixture '") + def.name + "' holds a negative number of slots");

        if (def.slots > 0 && (def.slots % slots::kAcross) != 0) {
            fault(std::string("fixture '") + def.name + "' holds " + std::to_string(def.slots)
                  + " slots, which is not a whole number of rows of " + std::to_string(slots::kAcross));
        }

        if (def.joins < 1) fault(std::string("fixture '") + def.name + "' joins fewer than one of itself");

        if (def.joins > slots::kMostParts) {
            fault(std::string("fixture '") + def.name + "' joins " + std::to_string(def.joins)
                  + " of itself, and a bank holds at most " + std::to_string(slots::kMostParts));
        }
    }

    return wrong;
}

const registry::Checker stores{StoresAreWholeRows};

} // namespace
