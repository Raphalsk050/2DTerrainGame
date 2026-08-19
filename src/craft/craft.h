#pragma once

#include "core/stack.h"
#include "craft/recipe_def.h"
#include "item/inventory.h"

#include <optional>
#include <vector>

// What can be made, and making it.
//
// Kept apart from both the table and the panel, and that division is the point.
// The table says what a recipe is; this says what a recipe *means* against a
// particular inventory; the panel says what that looks like. A rule about whether
// something can be built has to have exactly one home, or the button that is drawn
// dim and the click that is refused end up asking different questions — which is
// the same argument `item_def.h` makes about `Placement`.
namespace craft {

// A recipe with every name resolved into the stack it stands for.
//
// Built once, after `content::Open` has frozen the tables, and kept. Resolving a
// name is a walk of two tables and the panel would otherwise do it for every
// ingredient of every recipe every frame — but that is not really why: it is that a
// resolved bill is a thing that either exists or does not, so the failure of a bad
// name happens once, at startup, where `Verify` can report it.
struct Bill {
    Recipe id{};

    // What comes out, `yields` of it already counted in.
    Stack makes{};

    Stack needs[kMaxNeeds]{};
    int count = 0;

    const RecipeDef &Def() const { return Of(id); }
};

// Every recipe in the game, in the table's own order.
const std::vector<Bill> &Bills();

// Where a recipe stands against what the player is carrying.
//
// Three states rather than two, and the middle one is the whole of the mechanic
// asked for: a recipe the player has *some* of is a recipe they can see and work
// towards, and one they have none of is not yet part of their world.
enum class Standing {
    // At least one ingredient is not in the pack at all. Not listed.
    Absent,

    // Every ingredient is in the pack, and at least one of them is short. Listed,
    // and drawn as a thing that cannot be built yet.
    Short,

    // Enough of everything.
    Ready,
};

Standing StandingOf(const Bill &bill, const Inventory &pack);

// How many of one ingredient the player has, for the panel to print beside how
// many the recipe wants.
inline int Held(const Stack &need, const Inventory &pack) {
    return pack.Tally(need);
}

// What came of pressing build.
struct Made {
    bool done = false;

    // What the pack had no room for. Never dropped here — the caller has the world
    // and the player's position, which is what putting it on the ground takes, and
    // the rule this project keeps is that nothing is lost silently.
    Stack overflow{};
};

// Spends the ingredients and hands back the product.
//
// Refuses outright unless the standing is `Ready`, so the check the panel draws and
// the check the click makes are the same function. All the ingredients are counted
// before any of them is taken: a half-spent bill leaves the player charged for
// something that did not happen, which is the rule `Inventory::Remove` already
// keeps one level down.
Made Make(const Bill &bill, Inventory &pack);

// The stack a name stands for, searching the item table and then the materials.
//
// Public because `recipe_checks.cpp` needs the same lookup to say which names do
// not resolve, and a check written against a second copy of a lookup checks the
// copy. Nothing where no such row exists in either table.
std::optional<Stack> Named(const char *name, int count);

// Whether a name is a row of *both* tables, which is the one way the lookup above
// can be quietly wrong: it would answer the item and the material would never be
// reachable. There is no such name today and the check is what keeps it that way.
bool Ambiguous(const char *name);

} // namespace craft
