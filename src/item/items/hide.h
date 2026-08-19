#pragma once

#include "item/item_def.h"

// The first thing in this table that comes off a creature rather than a plant,
// and it needed nothing said about it here to be one. What drops it is written on
// the boar's own row under `entity/mob/mobs/`, and this end of the join knows only
// that it is a thing that can be carried — the same arrangement a sapling and its
// tree already have.
namespace items {

inline constexpr ItemDef kHide = {
    .name   = "hide",
    .colour = {172, 132, 96, 255},

    // A cured skin, pinned flat: pale at the edges where it has been stretched
    // and darker through the middle where it has not. The four corners are what
    // say pinned rather than folded.
    .picture =
        {
            .tone = {{206, 170, 128, 255}, {166, 126, 90, 255}, {116, 84, 58, 255}, {228, 206, 178, 255}},
            .art =
                {
                    ".d..d.",
                    "daaaad",
                    "aabbba",
                    "abbbca",
                    "dabbcd",
                    ".d..d.",
                },
        },
    .stack     = 64,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item Hide() {
    static const Item id = item::Table().IdOf(&kHide);

    return id;
}

} // namespace items
