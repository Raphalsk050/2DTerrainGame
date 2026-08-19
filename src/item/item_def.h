#pragma once

#include "core/picture.h"
#include "core/registry.h"
#include "core/tool.h"
#include "raylib.h"

// What an item *is*, and nothing about which items there are.
//
// The two used to be one file, and that file was on its way to being the thing
// this project keeps warning about: `kItems[]` with every row in it, an `enum`
// naming the positions, and a new item meaning an edit in the middle of both. It
// is split now — the shape of a row lives here, each row lives in its own file
// under `items/`, and the table assembles itself. See `core/registry.h` for how,
// and for the one thing that was given up doing it.
//
// ---
//
// **Why an item is not an element.** An element is a material the world is made
// of: it has a field over the lattice, a threshold, a rank against every other
// material and a rule for where it generates. An apple has none of those and would
// need all four invented for it, which is how a good table stops being one. What an
// item has is a name, a picture and a count.

// What an item does when the right hand puts it into the world.
//
// A field on the row rather than a test in the editor, and that is the point: what
// "the player can put this down" means has to be one fact in one place, or the hand
// that places it, the hand that decides whether it can be placed, and the ghost
// that shows where it would go all end up asking different questions.
//
// Nothing to do with materials. A material is terrain, it goes down under a brush
// by the fistful, and it has an element rather than an item.
enum class Placement {
    // Only ever carried. Wood, apples, resin, fibre.
    None,

    // Takes root standing on the ground under the cursor, and grows.
    Plant,

    // Fixes to a surface in one cell of the build grid, and stays there.
    //
    // The row that says which surfaces, and what it gives off, is a fixture's
    // rather than anything here — see the head of `entity/fixture.h` for why a
    // torch is not a material and not just an item either. What this enumerator
    // does is send the hand to the right table.
    Fixture,
};

struct ItemDef {
    // What the registry sorts on, and what every other table names this row by.
    // It has to be unique across the item table; `Verify` says so if it is not.
    const char *name;

    // The one colour to use where only one will fit — a marker on a map, a tint on
    // a label. Not what the item is drawn from; that is the picture.
    Color colour;

    Picture picture;

    int stack;

    Placement placement = Placement::None;

    // What this is worth in the hand: which tool it counts as, how fast it goes
    // through what that tool is right for, and what it adds to a punch.
    //
    // A row that says nothing is not a tool, which is nearly all of them. See
    // `core/tool.h` — the whole of the reasoning is there, including why this is one
    // struct and not three fields.
    tool::Kit tool{};

    // What this kind of row is called, for the startup line. See
    // `registry::Table`'s constructor, which is the only thing that reads it.
    static constexpr const char *kLabel = "items";
};

// One item, as a number that is stable across builds.
//
// It replaces `enum class Item`, and the practical difference at a call site is one
// pair of brackets: `Item::Torch` is `items::Torch()`. What it buys is that the
// list of items is no longer written down anywhere.
using Item = registry::Handle<ItemDef>;

namespace item {

inline registry::Table<ItemDef> &Table() {
    return registry::Table<ItemDef>::The();
}

inline int Count() {
    return Table().Size();
}

// The row a handle names.
inline const ItemDef &Of(Item item) {
    return Table().At(item);
}

// By name, for the tables that name items rather than holding them — a fixture's
// `from`, a species' sapling, a creature's spoils. Nothing where no such item
// exists, which is a content fault for `Verify` to report rather than a crash.
inline std::optional<Item> Find(const char *name) {
    return Table().Find(name);
}

// The same, treating a null name as "says nothing" rather than as a lookup that
// failed.
//
// The two have to be told apart and this is where it is done, once. A row that
// leaves its item field empty is a row making no claim — a plant that does not sow,
// a creature that drops nothing — and it is not a fault. A row that names something
// missing is. Folding them together is exactly the mistake CLAUDE.md §16.2b
// describes, where a typo and a legitimate "none" became the same answer.
inline std::optional<Item> Named(const char *name) {
    if (name == nullptr) return std::nullopt;

    return Table().Find(name);
}

} // namespace item

// Kept as a free function with this spelling because it is what the rest of the
// codebase already calls, and because it reads the same as the one beside it in
// `element.h` — each table answering for its own rows.
inline const ItemDef &Def(Item item) {
    return item::Of(item);
}
