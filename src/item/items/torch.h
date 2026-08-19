#pragma once

#include "item/item_def.h"

// The first light a player carries that is not the one in their hand.
//
// `Placement::Fixture` sends the hand to the fixture table, and the row there
// names this item back. Neither file knows the other's contents — they agree on a
// name, and `Verify` says so at startup if they stop agreeing.
namespace items {

inline constexpr ItemDef kTorch = {
    .name   = "torch",
    .colour = {255, 198, 130, 255},

    // The same flame the fixture draws standing in the world, so what is in the
    // slot and what goes on the wall are recognisably one thing. The two are
    // written out separately because the tables are separate and neither may
    // reach into the other — but they are the same six rows, and retuning one
    // means retuning the other.
    .picture =
        {
            .tone = {{255, 242, 210, 255}, {255, 216, 150, 255}, {226, 170, 92, 255}, {196, 128, 44, 255}},
            .art =
                {
                    "..a...",
                    ".aab..",
                    ".abbc.",
                    "..bc..",
                    "..d...",
                    "..d...",
                },
        },
    .stack     = 64,
    .placement = Placement::Fixture,
};

// This row's id.
//
// Worked out on the first call and kept, which is after `content::Open` has frozen
// the table — see `core/registry.h` for why an id cannot be a compile-time constant
// once the table assembles itself.
inline Item Torch() {
    static const Item id = item::Table().IdOf(&kTorch);

    return id;
}

} // namespace items
