#pragma once

#include "item/item_def.h"

// What wood is cut down into before it is anything else.
//
// It is carried and nothing more — `Placement::None` — because a stick is not a
// thing that goes into the world. Every use it has is as an ingredient, which is
// a fact about a recipe's row and not about this one: see `craft/recipe_def.h`,
// where the two tables agree on a name and neither reaches into the other.
namespace items {

inline constexpr ItemDef kStick = {
    .name   = "stick",
    .colour = {150, 106, 62, 255},

    // Wood's own four tones, so that a stick and the log it came off read as one
    // material seen twice. The accent is the pale broken end rather than a cut
    // face — a stick is snapped, not sawn, and that is the one mark at this size
    // that says stick and not small log.
    .picture =
        {
            .tone = {{166, 118, 68, 255}, {124, 82, 46, 255}, {82, 52, 28, 255}, {206, 174, 128, 255}},
            .art =
                {
                    "....ab",
                    "...ab.",
                    "...ab.",
                    "..ab..",
                    "..ab..",
                    ".dc...",
                },
        },
    .stack = 64,
};

// This row's id. Worked out on the first call, which is after `content::Open` has
// frozen the table.
inline Item Stick() {
    static const Item id = item::Table().IdOf(&kStick);

    return id;
}

} // namespace items
