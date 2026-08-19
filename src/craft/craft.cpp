#include "craft/craft.h"

#include "world/element.h"

#include <cstring>

namespace craft {
namespace {

// The material of that name, if there is one.
//
// A walk of `kElements` rather than a lookup, because the element table is still
// the one central array in the project — §26's note about what is left to move —
// and it has no `Find` of its own. Fourteen string compares, done once per
// ingredient at startup.
std::optional<Element> Material(const char *name) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (std::strcmp(kElements[e].name, name) == 0) return static_cast<Element>(e);
    }

    return std::nullopt;
}

} // namespace

std::optional<Stack> Named(const char *name, int count) {
    if (name == nullptr) return std::nullopt;

    // Items first, and the order is arbitrary *because* `Ambiguous` refuses the one
    // case where it could matter. Without that check this line would silently decide
    // which of two rows a recipe meant.
    if (const std::optional<Item> item = item::Find(name)) return ItemsOf(*item, count);

    if (const std::optional<Element> element = Material(name)) return BlocksOf(*element, count);

    return std::nullopt;
}

bool Ambiguous(const char *name) {
    if (name == nullptr) return false;

    return item::Find(name).has_value() && Material(name).has_value();
}

const std::vector<Bill> &Bills() {
    // Built on the first call, which is after the tables are frozen. The same
    // pattern an item's accessor uses, one table further out.
    static const std::vector<Bill> bills = [] {
        std::vector<Bill> made;

        made.reserve(static_cast<std::size_t>(Count()));

        for (int i = 0; i < Count(); i++) {
            const Recipe id{i};
            const RecipeDef &def = Of(id);

            Bill bill{};
            bill.id = id;

            // A name that does not resolve cannot happen in a build that started —
            // `Verify` has already refused to run. What is left here is a value
            // that has to be *something*, and an empty stack is the answer that
            // makes the rest of this loop harmless rather than undefined.
            bill.makes = Named(def.makes, def.yields).value_or(Stack{});

            for (const Need &need : def.needs) {
                if (!need.Asked()) continue;

                bill.needs[bill.count] = Named(need.what, need.count).value_or(Stack{});
                bill.count++;
            }

            made.push_back(bill);
        }

        return made;
    }();

    return bills;
}

Standing StandingOf(const Bill &bill, const Inventory &pack) {
    bool enough = true;

    for (int i = 0; i < bill.count; i++) {
        const int held = Held(bill.needs[i], pack);

        // None of it at all is what keeps a recipe off the list. Asked before the
        // count, because the two answers are different states and not degrees of
        // one: a player with no cobblestone has not met the stone axe, and a player
        // with two has.
        if (held <= 0) return Standing::Absent;

        if (held < bill.needs[i].count) enough = false;
    }

    // A recipe that asks for nothing is not free, it is unfinished. Refused rather
    // than granted, so a row half written is a row that does nothing instead of a
    // row that hands out something for no cost.
    if (bill.count <= 0) return Standing::Absent;

    return enough ? Standing::Ready : Standing::Short;
}

Made Make(const Bill &bill, Inventory &pack) {
    Made made{};

    if (StandingOf(bill, pack) != Standing::Ready) return made;
    if (bill.makes.Empty()) return made;

    // Counted above, spent here. `Inventory::Remove` is all-or-nothing per stack and
    // every one of these has just been shown to be there, so nothing in this loop
    // can leave the pack half charged.
    for (int i = 0; i < bill.count; i++) pack.Remove(bill.needs[i]);

    Stack product = bill.makes;

    product.count = pack.Add(product);

    made.done     = true;
    made.overflow = product.count > 0 ? product : Stack{};

    return made;
}

} // namespace craft
