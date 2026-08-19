#pragma once

#include "item/item_def.h"

// What a scythed tuft of grass leaves.
namespace items {

inline constexpr ItemDef kFibre = {
    .name   = "fibre",
    .colour = {186, 176, 120, 255},

    // A hank of stripped bark, gathered in the middle. The tie is what stops it
    // reading as a smudge.
    .picture =
        {
            .tone = {{214, 206, 154, 255}, {170, 158, 104, 255}, {124, 112, 70, 255}, {148, 116, 66, 255}},
            .art =
                {
                    ".a..a.",
                    ".ab.b.",
                    "..aba.",
                    "..dd..",
                    ".ab.a.",
                    ".b..b.",
                },
        },
    .stack     = 64,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item Fibre() {
    static const Item id = item::Table().IdOf(&kFibre);

    return id;
}

} // namespace items
