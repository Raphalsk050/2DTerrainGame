#pragma once

#include "entity/body/build.h"

// The character, as numbers.
//
// It was a namespace of thirty constants, which is exactly right while there is one
// body in the world and exactly wrong the moment there are two. Most of it is now a
// `body::Build` — the same struct every creature is described by — and what is left
// here is the handful that belong to *this* body and to nothing else: the swing,
// the fist, and how much the character can take.
//
// The split is the point. Everything in `Build()` below is shared machinery a boar
// gets for free; everything under it is the character.
//
// Read the figures against the character they were tuned for: 26 tall, 12 wide,
// jumps 72.
namespace player_config {

// How the character walks, falls, swims and is dug out of a hillside.
//
// A function returning a value rather than a constant, because a `Build` is a plain
// aggregate and this is the one place that says which values are the player's. Call
// it once, at construction.
inline constexpr body::Build Build() {
    return {
        .width        = 12.0f,
        .height       = 26.0f,
        .crouchHeight = 14.0f,

        .runSpeed    = 220.0f,
        .crouchSpeed = 90.0f,

        // A little over one and a half times the walk, which is the ratio a pair of
        // Terraria's boots gives and is about the smallest one that reads as a
        // different gait rather than as the same one tuned up. It is also what makes
        // the jump worth running into: the arc lasts six tenths of a second whatever
        // the speed, so a standing jump carries the body 132 pixels and a sprinting
        // one 228 — the difference between clearing a hole and landing in it.
        //
        // Measured against the frame rather than against the character, because what
        // a player is actually asking for when they ask to run is to spend less time
        // crossing ground they have already seen: at this speed the thousand pixels
        // of the window go by in two and a half seconds instead of four and a half.
        .sprintSpeed = 380.0f,

        .groundAccel = 2200.0f,
        .airAccel    = 1200.0f,

        .gravity      = 1600.0f,
        .maxFallSpeed = 900.0f,

        .jumpSpeed    = 480.0f,
        .jumpCutSpeed = 190.0f,

        // Set a little above the terrace riser, so a ledge meant to be walked up is
        // walked up. See `body::Build::stepHeight`.
        .stepHeight = 14.0f,

        .snapDistance = 14.0f,
        .cornerNudge  = 5.0f,

        // Two body heights, and no further. What this is for is the pixel or the
        // block that closed over the character, not tunnelling out of a mountain.
        .unstickReach = 52.0f,

        .coyoteTime = 0.10f,
        .jumpBuffer = 0.12f,

        .buoyancy  = 1.4f,
        .waterDrag = 5.0f,

        .swimThreshold = 0.25f,
        .swimSpeed     = 150.0f,
        .swimStroke    = 200.0f,
        .sinkStroke    = 170.0f,
        .waterMaxFall  = 200.0f,

        // The character walks; it does not hover. Free flight is a state it can be
        // put into and not a fact about it — see `body::Body::Ghost`.
        .floats = false,

        .flySpeed      = 420.0f,
        .flyBoostSpeed = 1600.0f,
        .flyAccel      = 6000.0f,
    };
}

// Window during which the attack hitbox deals damage, and the delay before the next
// attack may start. They are separate so a short strike can still have a long
// recovery.
inline constexpr float kAttackDuration = 0.15f;
inline constexpr float kAttackCooldown = 0.35f;

// Distance from the body centre to the centre of the strike box, and the side of
// that box. The box stays axis aligned whatever the aim angle, which is an
// approximation of a swing but keeps the hit test a rectangle intersection.
inline constexpr float kAttackReach = 22.0f;
inline constexpr float kAttackSize  = 18.0f;

// How far back the arm is raised at the top of a swing, in radians.
//
// A little over a third of a turn, which is enough to read as a swing at this size
// and short of the arm ending up behind the body. It comes down through the aim over
// kAttackDuration and stops there, rather than swinging back up: a chop lands, it
// does not wave.
inline constexpr float kSwingArc = 2.2f;

// What a bare hand does to a living thing.
//
// The unit every creature's `hardy` is written against, so a boar at ten is five
// punches. It is deliberately *not* the same figure as digging, which is a rate
// against a material's hardness rather than a number of hits — see CLAUDE.md §13.1.
// A pickaxe will divide the one and a sword will multiply the other, and neither
// will be this line.
inline constexpr int kFistDamage = 2;

// How much the character can take.
//
// Twenty, which is Minecraft's, and it is chosen to be *drawn* rather than to be a
// round number: the display is ten hearts and a heart is two points, so a bare fist at
// `kFistDamage` above takes exactly half of one. That correspondence is the whole of
// what makes a row of hearts readable at a glance — see `ui/vitals.h` — and it holds
// only while these numbers are chosen together.
//
// It was a hundred, which is a fine figure for a bar and a useless one for hearts: it
// makes a punch a fiftieth of the row, which is a step nothing can see.
inline constexpr int kHealth = 20;

} // namespace player_config
