#pragma once

#include "item/item_def.h"

// The first thing the player owns that is not carried.
//
// `Placement::Fixture` sends the hand to the fixture table, and the row there names
// this item back. Neither file knows the other's contents — they agree on a name, and
// the startup check says so if they stop agreeing.
//
// It stacks to sixty-four like everything else, and that is worth a word because it
// looks generous: a chest in the bag is a chest, and a player who has felled a wood
// and made a dozen of them is carrying a store room to build, not a store room. What
// bounds how much can be kept is how many will stand in a wall, not how many will fit
// in a pocket.
namespace items {

inline constexpr ItemDef kChest = {
    .name   = "chest",
    .colour = {158, 112, 64, 255},

    // The same front-on chest the fixture draws standing in the world, so what is in
    // the slot and what is on the floor are recognisably one thing. The two are
    // written out separately because the tables are separate and neither may reach
    // into the other — but they are the same six rows, and retuning one means retuning
    // the other.
    .picture =
        {
            .tone = {{198, 150, 92, 255}, {158, 112, 64, 255}, {104, 70, 40, 255}, {58, 44, 32, 255}},
            .art =
                {
                    ".dddd.",
                    "daaaad",
                    "ddaadd",
                    "dbbbbd",
                    "dbbbbd",
                    ".dddd.",
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
inline Item Chest() {
    static const Item id = item::Table().IdOf(&kChest);

    return id;
}

} // namespace items
