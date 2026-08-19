#include "core/registry.h"
#include "item/item_def.h"

#include <cstring>
#include <filesystem>
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

// Every row that names a file has one.
//
// The whole reason `ItemDef::art` may be a path rather than a folder is that there is
// one picture per item and a folder holding one file would say nothing — but the price
// of a path in a table is a path that can be misspelt, and §16.2b is the long version
// of what that costs. A row naming a file that is not there does not error and does not
// warn: the item is quietly drawn from the hand-drawn `picture` beside it, which looks
// like a perfectly good picture, and nothing anywhere says the art was meant to be
// used at all.
//
// So it is opened here, before the window, where a fault can still stop the program.
// Existence and not decoding, because decoding needs a graphics device and this runs
// before there is one — `icon::Art` warns for the rest.
std::string ArtIsWhereItSaysItIs() {
    std::string wrong;

    for (const ItemDef *def : item::Table().All()) {
        if (def->art == nullptr) continue;

        const std::string path = std::string("assets/") + def->art + ".png";

        if (std::filesystem::exists(path)) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("item '") + def->name + "' is drawn from '" + path + "', and there is no such file";
    }

    return wrong;
}

// Nothing that wears out is carried more than one to a slot.
//
// `Stack::wear` is a field on the slot rather than on the item, so a slot holding two
// of a thing that wears would be two tools sharing one lifetime: use one and the other
// is worn too, and breaking the pair takes both. There is no arrangement of a stack
// that makes sense of that, and the merge rules would reach it on their own —
// `Stack::Room` is what stops two tools ever pouring into one another, and it stops it
// only because the limit is one.
std::string WearingThingsDoNotStack() {
    std::string wrong;

    for (const ItemDef *def : item::Table().All()) {
        if (!def->tool.Wears() || def->stack <= 1) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("item '") + def->name + "' wears out and stacks to " + std::to_string(def->stack)
                 + " — a stack cannot share one lifetime";
    }

    return wrong;
}

const registry::Checker square{PicturesAreSquare};
const registry::Checker drawn{ArtIsWhereItSaysItIs};
const registry::Checker alone{WearingThingsDoNotStack};
const registry::Checker unique{NamesAreUnique};
const registry::Checker usable{StacksAreUsable};

} // namespace
