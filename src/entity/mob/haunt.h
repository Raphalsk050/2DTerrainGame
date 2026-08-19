#pragma once

#include "world/element.h"

// Where a creature comes from: the whole spawn rule, on the creature's own row.
//
// It is written against the same fields every other placement in this world is
// written against, and that is deliberate — CLAUDE.md §8 makes the case at length
// and this is one more table joining the agreement. A boar wants the country the
// meadow grass wants because both read `ElementClimate` through the same
// `ClimateBell`; a bat wants the depth an ore wants because both are counted from
// the same generated surface. There is no biome table for creatures either, and
// there must not be one: the moment a mob names a biome, the boar's country and
// the grass's country are two facts that can disagree.
//
// Nothing here is checked at spawn time by the creature. The spawner reads the
// row and the creature never learns why it is standing where it is.
namespace mob {

struct Haunt {
    // Whether this creature is ever tried at all.
    //
    // A row with `chance` at zero is a creature that exists, can be spawned by
    // hand and by a command, and never appears on its own — which is a real and
    // useful state for something being drawn, and is why the default is nothing
    // rather than something.
    float chance = 0.0f;

    // Depth below the generated surface, in world pixels, that it may stand at.
    //
    // Counted from `terrain::Height` rather than from what has been dug, on the
    // same argument §17.3 makes about the horizon: a player who digs a shaft has
    // not moved the surface, and a cave mob welling up out of a staircase because
    // the ground above it was carried away is a mob that follows the shovel.
    //
    // A surface creature asks for a band that straddles zero. A cave creature asks
    // for one that starts below the crust.
    float fromDepth = -64.0f;
    float toDepth   = 0.0f;

    // The light it will appear in, in the solver's own [0,1] level.
    //
    // A band and not a threshold, because both ends are real questions: a thing of
    // the dark asks for a ceiling, and a thing of the day asks for a floor. The
    // defaults let anything through, so a row that does not care says nothing.
    float darkerThan   = 1.0f;
    float brighterThan = 0.0f;

    // Which country. The same bell the covers, the woods, the drought and the
    // snow are placed by — see `ElementClimate`, whose defaults mean no preference
    // at all.
    ElementClimate climate{};

    // How many arrive together. A herd is what makes a meadow read as inhabited
    // rather than as a place one animal happens to be standing in.
    int least = 1;
    int most  = 1;

    // How many arrive together is the whole of the cap now, and it is per cell.
    //
    // There was a `crowd` here — a ceiling on how many of a kind could be alive at
    // once anywhere — and it went with the stateless spawner it belonged to. It
    // cannot survive this model and should not: whether a creature exists is a fact
    // about the ground it stands on, and a global ceiling would make it depend on how
    // many others happened to be awake somewhere else. Walk far enough and the same
    // meadow would hold a different number of boars.
    //
    // What bounds the population now is `most` against the size of a cell, which is a
    // density rather than a total — and a density is what a country has.
};

} // namespace mob
