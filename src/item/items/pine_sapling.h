#pragma once

#include "item/item_def.h"

// See `oak_sapling.h` for why there is one of these per species.
namespace items {

inline constexpr ItemDef kPineSapling = {
    .name   = "pine sapling",
    .colour = {92, 124, 56, 255},

    // Two tiers of needles on a leader, which is the conifer in miniature.
    .picture =
        {
            .tone = {{130, 158, 74, 255}, {62, 92, 42, 255}, {96, 62, 34, 255}, {70, 52, 34, 255}},
            .art =
                {
                    "..a...",
                    ".aab..",
                    "..bc..",
                    ".aab..",
                    "..c...",
                    ".ddd..",
                },
        },
    .stack     = 64,
    .placement = Placement::Plant,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item PineSapling() {
    static const Item id = item::Table().IdOf(&kPineSapling);

    return id;
}

} // namespace items
