#pragma once

#include "entity/mob/brain.h"

namespace mob {

// Grazes until something hurts it, then runs from whatever did.
//
// Prey, and the whole of what makes an animal read as prey is in when it stops
// running: it bolts on being struck and not on being approached, so a player can
// walk up to a boar, and it forgets the fright after a while rather than fleeing
// for ever. A creature that ran from the sight of the player could never be caught
// and would never be seen close up; one that never forgot would spend the rest of
// its life pressed against a hillside.
//
// It is the drifter with a second state over the top, and it defers to the drifter
// outright when calm — see `Drifter` for why that is one object and not a copy.
struct Skittish : Brain {
    body::Intent Think(const Sense &sense, Wits &wits) const override;

    const char *Mood(const Wits &wits) const override;
};

// The one instance, shared by every creature of this temper.
//
// It registers itself under the name a creature's `temper` field names — see
// `skittish.cpp`. Nothing anywhere lists the behaviours that exist.
const Skittish &TheSkittish();

} // namespace mob
