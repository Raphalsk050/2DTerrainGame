#pragma once

// What a body is being asked to do this frame, and nothing about who asked.
//
// This is the seam the whole mob layer hangs off, and it already existed: the
// player was deliberately written to know nothing about the keyboard, taking a
// snapshot of intentions instead. That indirection was put there for remapping
// and replays, and it turns out to be the thing that makes a mob free — an
// animal is a body with the same six wishes filled in by a brain rather than by
// a hand.
//
// So there is no separate "AI movement" anywhere in this project, and there must
// never be one. A creature that walked by some other route would drift away from
// the character's walk the first time either was tuned, and the symptom is a mob
// that climbs a ledge the player is stopped by, or falls through a floor the
// player stands on.
//
// Keep this a statement of *want*. `moveX = 1` is "I would like to go right",
// not "move me right": what a body can actually do about that is the body's
// business, and it is where the ledge, the water and the wall are known about.
namespace body {

struct Intent {
    // -1 left, +1 right. Magnitudes under one are a slower walk, which is what a
    // brain ambling somewhere asks for and what a keyboard can never say.
    float moveX = 0.0f;

    // -1 up, +1 down. Only read by a body that is not held down by gravity —
    // a floating creature, or the free flight. On the ground the same wish is
    // spelled by jumpHeld and crouchHeld, which mean different things.
    float moveY = 0.0f;

    // Held controls jump height, pressed triggers it. Both, because a jump is
    // two decisions and a brain has to be able to make them separately: an
    // animal hopping a fence wants the whole arc, one clearing a puddle does
    // not.
    bool jumpPressed = false;
    bool jumpHeld    = false;

    bool crouchHeld = false;

    // Asks for speed: a run on the ground, and a much faster crossing in flight.
    // One field for both, the way moveY above is one field for two meanings, and
    // for the same reason — the two can never be asked for at once, because a
    // flying body is not walking.
    bool sprintHeld = false;
};

} // namespace body
