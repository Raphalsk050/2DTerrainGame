#include "core/registry.h"
#include "item/item_def.h"

#include <cstring>
#include <string>

// What used to be `static_assert`s at the foot of the item table.
//
// They could not survive a table that assembles itself — see the head of
// `core/registry.h` — so they are checks now, run together by `content::Open`
// before the window opens, and a failure stops the program. The wording matters
// as much as the check: a line that names the offending row is the difference
// between a fault fixed in a minute and a day spent looking in the wrong file.
namespace {

// Every picture is six rows of six characters.
//
// A row one character short is not a syntax error and not a crash: the loop meets
// the terminator, the mark reads as blank, and the picture draws with a hole down
// one side. That is precisely the kind of fault that survives being looked at,
// since a picture with a hole in it still looks like a picture.
std::string PicturesAreSquare() {
    std::string wrong;

    for (const ItemDef *def : item::Table().All()) {
        if (IsSquare(def->picture)) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("item '") + def->name + "' is not six rows of six characters";
    }

    return wrong;
}

// No two rows share a name.
//
// The check the old table never needed and this one cannot do without: names are
// the identity now — they are what the ids are sorted by and what every other
// table refers to a row through. Two rows called the same thing means one of them
// is unreachable and which one is decided by a sort, which is to say arbitrarily.
std::string NamesAreUnique() {
    const auto &rows = item::Table().All();

    std::string wrong;

    for (std::size_t i = 1; i < rows.size(); i++) {
        // The table is sorted by name, so duplicates are adjacent.
        if (std::strcmp(rows[i - 1]->name, rows[i]->name) != 0) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("two items are both called '") + rows[i]->name + "'";
    }

    return wrong;
}

// Nothing carries a stack of nothing.
std::string StacksAreUsable() {
    std::string wrong;

    for (const ItemDef *def : item::Table().All()) {
        if (def->stack > 0) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("item '") + def->name + "' cannot be carried — its stack is " + std::to_string(def->stack);
    }

    return wrong;
}

const registry::Checker square{PicturesAreSquare};
const registry::Checker unique{NamesAreUnique};
const registry::Checker usable{StacksAreUsable};

} // namespace
