#pragma once

#include "core/figure.h"
#include "core/registry.h"
#include "entity/body/build.h"
#include "entity/mob/haunt.h"
#include "entity/mob/spoil.h"

// One creature, described completely.
//
// This is the row, and the whole design of the mob layer is that it is the *only*
// thing a new creature is. Everything else is derived from it with no existing file
// edited: the spawner reads `haunt`, the herd reads `build`, the brain is looked up
// from `temper`, the draw reads `look`, the death reads `spoils`, the hand reads
// `hardy`. The same bargain the item table makes, and for the same reason — where
// it has held, adding a thing has never needed a second file opened; where it has
// not, the failure is always the same shape, which is a creature that works
// everywhere except the one place written as a list of names.
//
// **The rule to keep**: a fact about a creature goes on the creature's row. If
// answering a question about a boar means writing `if (name == "boar")` anywhere
// outside the boar's own file, the field is missing.
namespace mob {

struct Def {
    // What the registry sorts on, what the world calls it, and what anything
    // referring to this creature names it by.
    const char *name;

    // How it is drawn, at the world's own texel. See `core/figure.h` for why this
    // is not the six-square `Picture` an item carries.
    figure::Figure look;

    // How it moves. The same struct the player is built from, so a creature that
    // walks, jumps, swims and steps over ledges gets all four for free — and gets
    // exactly the four the character gets, which is the point of sharing it.
    body::Build build{};

    // Which behaviour drives it, by name. Resolved once when the creature is born;
    // `Verify` has already established at startup that the name is one of the rows
    // under `brains/`.
    const char *temper = "drifter";

    // How much it can take. A blow from a bare hand is `player_config::kFistDamage`,
    // so read these as a number of punches.
    int hardy = 10;

    // What it does to whatever it strikes, and how hard it throws it.
    //
    // Zero damage is a creature that cannot hurt anything, which is most of them. A
    // hunter with zero here is a nuisance rather than a bug — it will still chase,
    // and that is a legitimate creature.
    int hits    = 0;
    float knock = 180.0f;
    float lift  = 140.0f;

    // How close it has to be to strike, in world pixels, and how long between
    // blows. The reach is measured centre to centre, so it has to clear both
    // bodies' half-widths before it means anything.
    float reach = 18.0f;
    float rest  = 0.9f;

    // How far it notices something, in world pixels.
    //
    // Read by the brains and not by the herd, because what "noticing" means is a
    // behaviour: a hunter uses it to start a chase and a skittish creature uses the
    // same number to decide it has got far enough away.
    float notices = 260.0f;

    // What it leaves when it dies.
    Spoils spoils{};

    // Where it comes from, and how often.
    Haunt haunt{};

    // Whether daylight destroys it.
    //
    // A field rather than a behaviour, deliberately: burning in the sun is not
    // something a creature decides to do, and tying it to the brain would mean a
    // peaceful thing of the dark could not have it without also hunting the player.
    bool burnsInDaylight = false;

    static constexpr const char *kLabel = "mobs";
};

// One kind of creature, as a number that is stable across builds.
using Kind = registry::Handle<Def>;

namespace kinds {

inline registry::Table<Def> &Table() {
    return registry::Table<Def>::The();
}

inline int Count() {
    return Table().Size();
}

inline const Def &Of(Kind kind) {
    return Table().At(kind);
}

inline std::optional<Kind> Find(const char *name) {
    return Table().Find(name);
}

} // namespace kinds

} // namespace mob
