#pragma once

#include "entity/mob/mob_def.h"

// A bat: the creature that proves a body does not have to fall.
//
// Everything about it that is different from a boar is one field — `build.floats` —
// and that is the point of it being here. Gravity, the ledge, the jump and the
// swim are all the body's, and a creature opts out of the first by saying so on its
// own row rather than by being a different kind of object.
namespace mobs {

inline constexpr mob::Def kBat = {
    .name = "bat",

    // Wings out, ears up, four texels tall. The one creature here read entirely by
    // its outline.
    .look =
        {
            .tone   = {{92, 78, 96, 255}, {58, 48, 64, 255}, {36, 30, 42, 255}, {198, 150, 160, 255}},
            .width  = 6,
            .height = 4,
            .art =
                {
                    "..dd..",
                    "a.bb.a",
                    "aabbaa",
                    ".acca.",
                },
        },

    .build =
        {
            .width        = 12.0f,
            .height       = 10.0f,
            .crouchHeight = 10.0f,

            // Gravity does not act on it and it still collides with the cave it is
            // in. Note that this is a *fact about the creature* — the player's free
            // flight is a different thing in a different place, and that one passes
            // through rock. See `body::Build::floats` and `body::Body::Ghost`.
            .floats     = true,
            .floatSpeed = 130.0f,
            .floatAccel = 420.0f,
        },

    .temper = "drifter",

    .hardy = 4,
    .hits  = 0,

    .notices = 180.0f,

    // Nothing. A bat is atmosphere, and paying the player for swatting one would
    // make it a resource.
    .spoils = {},

    .haunt =
        {
            .chance = 0.4f,

            // Under the crust and no further than a cave system reaches. The near
            // edge is past `caves.crust`, so a bat is never found in the few feet of
            // soil under a meadow.
            .fromDepth = 160.0f,
            .toDepth   = 2400.0f,

            // Dark, which underground is nearly everywhere and beside a torch is
            // not. A room the player has lit stops producing them, which is what
            // lighting a room is for — and it falls out of the light solver rather
            // than out of any rule written here.
            .darkerThan = 0.30f,

            .least = 1,
            .most  = 3,

            .crowd = 10,

            // Nearer than the others. A cave is a small place, and a distance tuned
            // for open country would mean no bat ever appears in one.
            .keepAway = 180.0f,
        },
};

inline mob::Kind Bat() {
    static const mob::Kind id = mob::kinds::Table().IdOf(&kBat);

    return id;
}

} // namespace mobs
