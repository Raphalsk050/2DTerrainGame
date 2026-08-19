#pragma once

#include "item/item_def.h"

// What a felled tree leaves. The first thing in the world a player has to
// build with, and the only one that arrives without a tool.
namespace items {

inline constexpr ItemDef kWood = {
    .name   = "wood",
    .colour = {138, 92, 52, 255},

    // Bark lit from above, bark in shadow, the dark underside, and the pale
    // cut face — which is the one mark that says log rather than stick.
    .picture =
        {
            .tone = {{166, 118, 68, 255}, {124, 82, 46, 255}, {82, 52, 28, 255}, {206, 174, 128, 255}},
            .art =
                {
                    "......",
                    ".daaa.",
                    "ddabbb",
                    "ddbbbc",
                    ".dccc.",
                    "......",
                },
        },
    .stack     = 64,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item Wood() {
    static const Item id = item::Table().IdOf(&kWood);

    return id;
}

} // namespace items
