#pragma once

#include "entity/mob/mob_def.h"

// A boar: something to meet in a meadow, and the first creature in this world that
// is neither scenery nor a hazard.
//
// It is here to prove a claim rather than to be interesting, and the claim is that
// a creature is a row. Nothing outside this file and its `.cpp` knows a boar
// exists — not the spawner, not the herd, not the draw, not the build. Read
// `entity/mob/mob_def.h` for what each field means and, more usefully, for what a
// missing one would cost.
namespace mobs {

inline constexpr mob::Def kBoar = {
    .name = "boar",

    // Low and long, which is the whole silhouette: at this size a creature is
    // recognised by its proportions well before any of its detail is, and the
    // boar's is a body wider than it is tall on short legs. The snout reaching past
    // the body is the one mark that says which way it is facing.
    .look =
        {
            .tone   = {{146, 112, 84, 255}, {104, 78, 58, 255}, {62, 46, 36, 255}, {224, 214, 200, 255}},
            .width  = 8,
            .height = 6,
            .art =
                {
                    "...aaa..",
                    "..aaaaa.",
                    ".aabbbbb",
                    ".abbbbbd",
                    ".bbbbbb.",
                    "..c..c..",
                },
        },

    // The cut-down sprite pack, four idle frames, six of walk and five of run.
    //
    // Twenty-eight pixels across and twenty-one tall, drawn one art pixel to one world
    // pixel — see the head of `core/sheet.h` for why authored art is allowed a finer
    // texel than the ground it stands on. That comes out the same size as the six-texel
    // drawing above, which was 24 by 18: near enough that nothing else on the row had
    // to move.
    .art     = "boar",
    .artWide = 28,

    // Eight pixels a frame. The walk is six frames, so a full cycle is forty-eight
    // pixels of ground — a little over two of its own body lengths, which is what a
    // short-legged animal covers in a stride.
    .stride = 8.0f,

    // Twenty by sixteen, which is the drawn figure less its margin. A collider
    // wider than the picture is a creature stopped by walls it visibly is not
    // touching; one narrower is a creature standing inside them.
    .build =
        {
            .width        = 20.0f,
            .height       = 16.0f,
            .crouchHeight = 16.0f,

            // Slower than the character on purpose. A prey animal the player cannot
            // catch is not prey.
            .runSpeed    = 70.0f,
            .crouchSpeed = 70.0f,

            // Its bolt. Under the character's own sprint, so fleeing buys the boar
            // distance and not escape.
            .sprintSpeed = 170.0f,

            .groundAccel = 900.0f,
            .airAccel    = 500.0f,

            // Enough to clear the world's 24 px terrace riser and nothing more. A
            // boar that could clear a wall would climb out of every pen.
            .jumpSpeed = 350.0f,

            .stepHeight   = 14.0f,
            .unstickReach = 32.0f,
        },

    .temper = "skittish",

    .hardy = 10,

    // It does not attack. A boar that turned and fought would be a different
    // animal, and this row is one field away from being that animal.
    .hits = 0,

    .notices = 220.0f,

    .spoils = {.each = {{.item = "hide", .least = 1, .most = 2}}},

    .haunt =
        {
            .chance = 0.15f,

            // At the surface and a little above it: the band straddles zero so that
            // a spot on a slope still qualifies.
            .fromDepth = -48.0f,
            .toDepth   = 8.0f,

            // Daylight only. The floor is what makes a boar a thing seen in a
            // meadow in the morning rather than a thing found in a cave.
            .brighterThan = 0.35f,

            // Temperate, and neither parched nor drowned — the same country the
            // meadow cover is laid on. That is not a coincidence to be kept in step
            // by hand: it is two tables agreeing about one place, read through the
            // same bell. See CLAUDE.md §8.
            .climate = {.temperature      = 0.55f,
                        .humidity         = 0.55f,
                        .temperatureWidth = 0.30f,
                        .humidityWidth    = 0.35f,
                        .fullAt           = 0.45f,
                        .goneAt           = 0.20f},

            // Two or three. A sounder, which is what boars come in, and what makes a
            // meadow read as inhabited rather than as somewhere one animal happens to
            // be standing.
            //
            // Read against the chance above and the size of a cell: a settling cell is
            // 512 px, a screen is a little under four of them, and at 0.15 that is one
            // sounder every couple of screens in country that suits them at all. Any
            // denser and a walk is a procession of boars.
            //
            // The chance was 0.55 until the cell hash was fixed — see `warren.cpp`. It
            // had been tuned against a world where whole rows of cells could never hold
            // anything, so it was compensating for a bug and the real density was never
            // once seen.
            .least = 2,
            .most  = 3,

        },
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline mob::Kind Boar() {
    static const mob::Kind id = mob::kinds::Table().IdOf(&kBoar);

    return id;
}

} // namespace mobs
