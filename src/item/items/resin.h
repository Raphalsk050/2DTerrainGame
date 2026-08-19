#pragma once

#include "item/item_def.h"

// What comes off a conifer.
namespace items {

inline constexpr ItemDef kResin = {
    .name   = "resin",
    .colour = {228, 176, 68, 255},

    // A bead that has run and set: heavier at the bottom, with the highlight
    // near the top where the surface turns over.
    .picture =
        {
            .tone = {{248, 214, 122, 255}, {216, 160, 56, 255}, {156, 104, 30, 255}, {255, 244, 206, 255}},
            .art =
                {
                    "..a...",
                    "..da..",
                    ".aabb.",
                    ".abbbc",
                    "..bbc.",
                    "..cc..",
                },
        },
    .stack     = 64,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item Resin() {
    static const Item id = item::Table().IdOf(&kResin);

    return id;
}

} // namespace items
