#pragma once

#include "entity/mob/brain.h"

namespace mob {

// Closes on whatever it has noticed and strikes it.
//
// The only one of the three behaviours that starts something. It is also the one
// where the difficulty is not the chasing — a wish to move towards a point is two
// lines — but everything around it:
//
//   - **It gives up.** A creature that never lost interest would follow the player
//     across the world for the rest of the session, and every one ever spawned
//     would end up in a queue behind them. It stops when the quarry is out of
//     reach for long enough, and goes back to wandering.
//   - **It jumps at terrain rather than at the player.** Aiming a jump at
//     something above you is how a creature spends its life bouncing under a
//     ledge. It jumps when it is *blocked*, which is the same reading the drifter
//     uses to turn at a wall, and the difference is that a hunter jumps where a
//     drifter turns away.
//   - **It does not walk off cliffs to reach you.** Or rather: it does, once its
//     quarry is close enough to be worth it. A predator that refused every drop
//     would be stopped by a one-block step, and one that refused none would empty
//     itself into the first ravine — so the brink test is kept and its answer is
//     overridden when the chase is nearly over.
struct Stalker : Brain {
    body::Intent Think(const Sense &sense, Wits &wits) const override;

    bool WouldStrike(const Sense &sense, const Wits &wits) const override;

    const char *Mood(const Wits &wits) const override;
};

// The one instance, shared by every creature of this temper.
//
// It registers itself under the name a creature's `temper` field names — see
// `stalker.cpp`. Nothing anywhere lists the behaviours that exist.
const Stalker &TheStalker();

} // namespace mob
