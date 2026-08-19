#pragma once

#include "core/registry.h"

// What a recipe *is*, and nothing about which recipes there are.
//
// A row per file under `recipes/`, registered by a registrar beside it, in the
// arrangement §19 sets out and for the same reason: adding a recipe must not mean
// opening a file that already works.
//
// ---
//
// **There is no grid.** Minecraft's recipes are a shape as much as a list — two
// planks stacked make sticks, three cobble in a row over two sticks make an axe —
// and that shape is a second thing to author, a second thing to draw and a second
// thing for a player to get wrong. What is kept from Minecraft is the *arithmetic*:
// what goes in, how much of it, and what comes out. What is dropped is where it
// goes. So a recipe here is a bill of materials and nothing else, which is the
// Don't Starve arrangement.
//
// **A row names other tables by name, never by handle.** `RecipeDef` is
// `constexpr` and an item's id is not known until `content::Open` has frozen the
// item table, so `makes` and every `Need::what` are `const char *` — §19.3's rule,
// arrived at from the same direction. `recipe_checks.cpp` establishes at startup
// that every one of those names resolves and that none of them resolves twice, so
// a lookup that fails at run time cannot happen in a build that started.
namespace craft {

// One ingredient.
//
// `what` names a row of *either* content table: "wood" is an item and
// "cobblestone" is a material, and a player carries both in the same nine places.
// Which table it is found in is worked out once, at startup, rather than written
// down here — a row that had to say which would be a row that could say the wrong
// one, and the name is already unique across both.
struct Need {
    const char *what = nullptr;
    int count        = 0;

    constexpr bool Asked() const { return what != nullptr && count > 0; }
};

// How many ingredients one recipe may ask for.
//
// Four, which is the width the panel draws in one row without wrapping. A fixed
// array rather than a span so the row stays a literal, and an unasked slot is one
// with a null `what` — the same "says nothing" that §19.3 gives `nullptr`.
inline constexpr int kMaxNeeds = 4;

struct RecipeDef {
    // What the registry sorts on, and what has to be unique across the recipe
    // table. Not what the panel prints: that is the *product's* own name, read off
    // whichever table it lives in, so a renamed item cannot leave a stale label
    // here.
    const char *name;

    // The item or material this makes, by name, and how many come out.
    const char *makes;
    int yields = 1;

    Need needs[kMaxNeeds]{};

    // One line under the name in the panel. What the thing is for, in the register
    // the rest of the interface uses — never a restatement of the ingredients,
    // which are drawn right beside it.
    const char *blurb = "";

    static constexpr const char *kLabel = "recipes";
};

// One recipe, as a number stable across builds.
using Recipe = registry::Handle<RecipeDef>;

inline registry::Table<RecipeDef> &Table() {
    return registry::Table<RecipeDef>::The();
}

inline int Count() {
    return Table().Size();
}

inline const RecipeDef &Of(Recipe recipe) {
    return Table().At(recipe);
}

} // namespace craft
