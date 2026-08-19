#pragma once

#include "item/item_def.h"

// What an apple tree gives up in autumn.
namespace items {

inline constexpr ItemDef kApple = {
    .name   = "apple",
    .colour = {214, 66, 58, 255},

    // Round, lit from the upper left, with the stalk still in it.
    .picture =
        {
            .tone = {{226, 92, 78, 255}, {176, 46, 44, 255}, {118, 28, 32, 255}, {96, 152, 70, 255}},
            .art =
                {
                    "..dc..",
                    ".aab..",
                    "aaabb.",
                    "aaabb.",
                    ".abbc.",
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
inline Item Apple() {
    static const Item id = item::Table().IdOf(&kApple);

    return id;
}

} // namespace items
