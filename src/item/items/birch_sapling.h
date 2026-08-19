#pragma once

#include "item/item_def.h"

// See `oak_sapling.h` for why there is one of these per species.
namespace items {

inline constexpr ItemDef kBirchSapling = {
    .name   = "birch sapling",
    .colour = {126, 196, 92, 255},

    // Thin and open, on the one pale stem in the wood.
    .picture =
        {
            .tone = {{172, 224, 124, 255}, {88, 158, 72, 255}, {198, 200, 194, 255}, {70, 52, 34, 255}},
            .art =
                {
                    ".a..a.",
                    ".ab.b.",
                    "..ab..",
                    "..c...",
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
inline Item BirchSapling() {
    static const Item id = item::Table().IdOf(&kBirchSapling);

    return id;
}

} // namespace items
