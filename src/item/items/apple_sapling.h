#pragma once

#include "item/item_def.h"

// See `oak_sapling.h` for why there is one of these per species.
//
// The one whose `d` is a blossom rather than soil, since the stem colour does for
// both here.
namespace items {

inline constexpr ItemDef kAppleSapling = {
    .name   = "apple sapling",
    .colour = {96, 156, 70, 255},

    // In blossom already, which is the one thing that tells it from the oak at
    // this size — the two trees are the same broadleaf shape.
    .picture =
        {
            .tone = {{140, 196, 92, 255}, {58, 116, 52, 255}, {110, 74, 42, 255}, {244, 222, 232, 255}},
            .art =
                {
                    "..d...",
                    ".aaba.",
                    ".abba.",
                    "..bc..",
                    "..c...",
                    ".ccc..",
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
inline Item AppleSapling() {
    static const Item id = item::Table().IdOf(&kAppleSapling);

    return id;
}

} // namespace items
