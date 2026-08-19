#pragma once

#include "entity/nav/plan.h"

#include <cstdint>

namespace mob {

// A creature's memory, held by the creature and handed to its brain to write.
//
// The brains are stateless and shared — one Drifter answers for every bat in the
// world — so anything a behaviour has to remember between frames lives here, on the
// creature, where it is one small struct per mob rather than one object with a
// vtable per mob.
//
// The fields are deliberately generic and deliberately few. A brain that needed a
// field of its own would be a brain that has become a creature, and the answer to
// that is a row under `mobs/`, not a wider struct here.
struct Wits {
    // What it is doing, meaning whatever the brain in charge says it means. It is
    // an opaque number on purpose: the creature stores it, the probe prints the
    // word the brain gives for it, and neither is entitled to an opinion about
    // what 2 is.
    std::uint8_t mood = 0;

    // How long the current mood has left, and how long it has been in it. Seconds,
    // on the frame clock.
    float holds = 0.0f;
    float since = 0.0f;

    // Which way it is currently minded to go, in [-1,1]. Kept across frames so that
    // a wander is a walk somewhere rather than a twitch.
    float lean = 0.0f;

    // How long until it may strike again. Counted here rather than in the brain for
    // the reason the rest of this struct exists: the brain that decides to strike
    // is shared by every creature of its temper.
    float rested = 0.0f;

    // What the navigator has to carry between frames: how long a leap it has
    // committed to has left to run. One number, and it lives here for the reason the
    // rest of this struct does — the navigator is a free function shared by every
    // creature, so it cannot hold anything about any of them. See `nav::Legs`.
    nav::Legs legs{};

    // A stream of its own, set when the creature is born.
    //
    // Every brain needs to differ from its neighbours — two boars that decide to
    // turn on the same frame are one boar drawn twice — and there is no global
    // random in this project by design. See `brains/whim.h` for why this one is
    // allowed to exist when everything else is a function of position.
    std::uint32_t seed = 0;
};

} // namespace mob
