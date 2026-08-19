#pragma once

#include "item/item_def.h"

#include <cstddef>

// What a creature leaves behind, on the creature's own row.
//
// A table inside a table rather than a function anywhere, and the precedent is
// `ElementDef::yields` (CLAUDE.md §11.1): what a thing gives up when it is broken
// is a fact about that thing, so it goes on its row and is read through one
// accessor. A `switch` in the herd's death path would be the same mistake one
// level up, and it would answer silently for every creature added after it.
//
// Three entries is the bound and it is generous for what this world holds. A
// creature that drops nothing leaves them all empty, which is a legitimate answer
// and the default — a predator is the danger, and being paid for surviving one is
// a decision to make on purpose rather than by forgetting a field.
namespace mob {

// Most kinds of thing one creature may leave.
inline constexpr std::size_t kMostSpoils = 3;

struct Spoil {
    // The item, by name. `nullptr` means this entry is unused.
    //
    // A name and not a handle, and that is what lets this table stay `constexpr`
    // while the item table assembles itself at startup: an id is not known until
    // the rows have been sorted, and a creature is described long before that. The
    // join is checked by `Verify` — a spoil naming an item that does not exist is
    // reported before the window opens, with both names in the line.
    //
    // `nullptr` rather than an empty string, so that "says nothing" and "names a
    // row called nothing" cannot be confused. That is CLAUDE.md §16.2b's argument
    // about a species' sapling, in the one form still available here.
    const char *item = nullptr;

    // Inclusive. Equal figures are a fixed drop; `least` at zero is a chance of
    // getting nothing, which is how a rare thing is written without a second
    // probability field.
    int least = 1;
    int most  = 1;
};

// Everything one creature leaves.
struct Spoils {
    Spoil each[kMostSpoils]{};
};

} // namespace mob
