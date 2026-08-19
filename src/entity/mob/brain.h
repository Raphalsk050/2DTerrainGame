#pragma once

#include "core/registry.h"
#include "entity/body/intent.h"
#include "entity/mob/sense.h"
#include "entity/mob/wits.h"

#include <optional>

namespace mob {

// How a creature decides what it wants.
//
// The Strategy pattern, and what it replaces is a `switch` over creature names in
// the file that moves creatures. That switch is *defensible* — how a boar behaves
// is a fact about boars — and it is the same mistake `scuff.cpp` made about what a
// ground kicks up (CLAUDE.md §16.1): it puts a question about a creature in a file
// about movement, and answers it silently for anything added later.
//
// **A brain is stateless and shared.** One instance answers for every creature of
// its temper, so it must hold nothing about any of them — everything it wants to
// remember goes in the `Wits` it is handed. That is what keeps a hundred bats from
// being a hundred allocations, and it makes the layer safe to run across the cores
// later, since nothing a brain touches belongs to anybody else.
//
// **A brain returns a wish, not a move.** `body::Intent` is a statement of want;
// what the creature can actually do about it is the body's business, and that is
// where the ledge, the water and the wall are known about. A brain that moved a
// creature itself would be a second copy of the walk — see `body/intent.h`.
struct Brain {
    virtual ~Brain() = default;

    // What this creature would like to do this frame.
    virtual body::Intent Think(const Sense &sense, Wits &wits) const = 0;

    // Whether it would strike right now, given that something is in reach.
    //
    // Asked separately from the intent because striking is not a move: it has its
    // own cadence, it is decided against the quarry rather than against the ground,
    // and folding it in would put a field on `body::Intent` that only creatures
    // ever fill in.
    virtual bool WouldStrike(const Sense &, const Wits &) const { return false; }

    // Whether this behaviour ever reads `Sense::seesQuarry`.
    //
    // A performance question that had to become a design one. Working out whether a
    // creature can *see* the player means walking the straight line between the two
    // and asking the world about every step of it — some thirty lookups, per creature,
    // per frame, and it was the single most expensive thing a creature did.
    //
    // Two of the three behaviours never look at the answer: a drifter notices nothing
    // by definition, and a skittish animal reacts to being *hit* rather than to being
    // approached, which is what makes a boar something you can walk up to. Only a
    // hunter needs it.
    //
    // So the brain says, and `Mob::Update` skips the walk. Declaring it here rather
    // than testing the creature's name anywhere is the same rule the rest of the
    // module keeps: the fact belongs to whoever it is a fact about.
    virtual bool Notices() const { return false; }

    // A short word for what it is doing, for `--mobs` and the debug overlay.
    //
    // It exists because a behaviour is the one thing in this project that cannot be
    // checked by comparing two pictures: a creature standing still is either calm
    // or broken, and the two look exactly alike.
    virtual const char *Mood(const Wits &) const { return "-"; }
};

// One behaviour, in the table of them.
//
// A row like any other, so a behaviour registers itself exactly the way an item
// does and a creature names one the way a fixture names an item. Adding a
// behaviour is a file under `brains/`; adding a creature that uses an existing one
// costs nothing at all.
struct BrainDef {
    const char *name;

    // The shared instance. A pointer and not a value, because a `Brain` is
    // polymorphic and a table of them has to hold the derived thing.
    const Brain *mind;

    static constexpr const char *kLabel = "brains";
};

namespace brain {

inline registry::Table<BrainDef> &Table() {
    return registry::Table<BrainDef>::The();
}

// The behaviour of a given name, or nothing.
//
// Nothing rather than a fallback, deliberately. A creature naming a behaviour that
// does not exist is a content fault reported by `Verify` at startup — and a silent
// fallback to "stands still" is exactly the `default:` branch this design exists to
// remove.
inline const Brain *Find(const char *name) {
    if (name == nullptr) return nullptr;

    const std::optional<registry::Handle<BrainDef>> id = Table().Find(name);

    if (!id.has_value()) return nullptr;

    return Table().At(*id).mind;
}

} // namespace brain

} // namespace mob
