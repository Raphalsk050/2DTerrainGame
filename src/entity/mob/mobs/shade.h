#pragma once

#include "entity/mob/mob_def.h"

// A shade: the thing in the dark, and the one creature here that starts something.
//
// It exercises the seams the other two do not — striking, being struck, dropping
// something, and being destroyed by a condition rather than by a blow. Its speed is
// the whole of the fight it offers: very nearly the character's walk and no sprint
// at all, so it is escapable by running and not by strolling. The player always has
// an answer and always has to use it.
namespace mobs {

inline constexpr mob::Def kShade = {
    .name = "shade",

    // Tall, narrow, the character's own height. That is what makes it read as a
    // threat rather than as an animal — nothing else about the picture has to. The
    // two pale marks are its eyes and are the only bright thing on it.
    .look =
        {
            .tone   = {{78, 72, 104, 255}, {48, 44, 70, 255}, {28, 26, 42, 255}, {206, 226, 255, 255}},
            .width  = 6,
            .height = 8,
            .art =
                {
                    "..aa..",
                    ".adda.",
                    ".abba.",
                    "abbbba",
                    "abbbba",
                    ".bbbb.",
                    ".bccb.",
                    "..cc..",
                },
        },

    .build =
        {
            .width        = 14.0f,
            .height       = 24.0f,
            .crouchHeight = 24.0f,

            .runSpeed    = 200.0f,
            .crouchSpeed = 200.0f,
            .sprintSpeed = 200.0f,

            .groundAccel = 1400.0f,

            // Enough to follow the character up a terrace, which is what stops a
            // hillside being a wall it gives up at.
            .jumpSpeed = 420.0f,
        },

    .temper = "stalker",

    .hardy = 14,

    .hits  = 4,
    .knock = 220.0f,
    .lift  = 160.0f,

    .reach = 22.0f,
    .rest  = 0.8f,

    .notices = 300.0f,

    .spoils = {.each = {{.item = "resin", .least = 0, .most = 1}}},

    .haunt =
        {
            .chance = 0.5f,

            // From a little above the surface to well under it, which is what
            // "comes out at night and lives in caves" is as one band rather than as
            // two rules.
            .fromDepth = -32.0f,
            .toDepth   = 2400.0f,

            // The dark, wherever it is. At night that is the open meadow; by day it
            // is only underground. One number, and the difference between the two
            // falls out of the light solver rather than out of a clock read here.
            //
            // Measured with `--mobs`, not chosen. It was 0.22 first, on the reasoning
            // that a threshold for darkness should be a small number — and the open
            // meadow at midnight reads brighter than that, so the shade appeared at
            // one spot in twenty and the one hostile creature in the game was
            // effectively not in it. At 0.35 it takes every unlit surface spot at
            // night and none at all by day, which is the whole of what the field is
            // for.
            .darkerThan = 0.35f,

            .least = 1,
            .most  = 2,

            .crowd = 6,
        },

    // And the sun ends it. A field rather than a behaviour: a creature does not
    // *decide* to burn, and tying it to the brain would mean a peaceful thing of
    // the dark could not have it without also hunting the player.
    .burnsInDaylight = true,
};

inline mob::Kind Shade() {
    static const mob::Kind id = mob::kinds::Table().IdOf(&kShade);

    return id;
}

} // namespace mobs
