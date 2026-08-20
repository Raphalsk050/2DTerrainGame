#pragma once

#include "item/inventory.h"
#include "raylib.h"

// Where everything stands while the pack is open, as one function of the window.
//
// This is CLAUDE.md §25.1 met a second time. There, three files each put something
// above the hotbar and none of them knew about the others, so all three landed in the
// same twenty pixels and the only way to find out was to launch the game and take a
// screenshot. `bottom::Of()` was the answer: one layout, computed again in the draw
// rather than remembered, with every row measured off the thing beside it.
//
// A chest brings a second panel on screen at the same time as the pack, and with it a
// bin, a search field and two buttons. Laying those out beside a panel that decides
// its own position is the same coincidence waiting to happen — so the pair is laid out
// *together*, and neither half knows where it is until this says.
//
// ---
//
// **The slots shrink, and only ever together.** Three chests joined is ninety-six
// slots, which is twelve rows; at the bar's own forty-four-pixel slot that is six
// hundred and fifty pixels of grid, and the window opens six hundred tall. So the
// texel a slot's picture is drawn at is chosen here — the largest whole number of
// screen pixels at which the whole pair fits the frame — and both panels are laid out
// from the one figure.
//
// A *whole* number, always. `hotbar.h` gives the reason and it is the one thing that
// must never happen to art of this size: a picture authored on a six-texel grid drawn
// at a fraction of a pixel comes out with its columns alternating two pixels wide and
// three. Six is the bar's own, so a pack opened with no chest in front of it is laid
// out exactly as it always was and nothing about today's screen has moved.
namespace pack {

// How wide a chest's grid is, in slots.
//
// Eight, because a chest holds thirty-two and thirty-two is four rows of eight. That
// is not arithmetic for its own sake: a bank is up to three chests and a *row* has to
// belong to exactly one of them, or a sorting rule about a row would have nowhere to
// live that survives the bank being taken apart. Four rows to a unit, eight across,
// and row `r` belongs to chest `r / 4`.
inline constexpr int kStoreColumns = slots::kAcross;

// The smallest and largest texel a slot's picture is drawn at.
//
// Six is the bar's, and nothing is ever drawn larger. Two is the floor, and it is only
// ever reached by three chests in a window near `config::kMinScreenHeight` — at which
// point the honest choice is a small grid the player can reach every slot of, rather
// than a handsome one with a row of it off the bottom of the screen.
inline constexpr int kFinestPixel  = 2;
inline constexpr int kCoarsePixel  = 6;

struct Metrics {
    float pixel = 6.0f;
    float side  = 44.0f;
    float pad   = 6.0f;

    // Between the grid and the bar row under it, which is what sets the bar apart as
    // the row that is also on screen when the panel is not.
    float gap = 14.0f;

    // Text on a button or a label, at this size.
    int font = 14;

    static Metrics At(float pixel);
};

// Where each piece of the pack is, this frame.
//
// A plain struct of rectangles rather than a set of functions taking an index, for
// everything whose position does not depend on one. What does — a slot — is a method,
// so that the panel and the hit test cannot each do the arithmetic and get different
// answers.
struct Layout {
    Metrics metric{};

    // The player's own thirty-six, the bar row included.
    Rectangle panel{};

    // The bin above its right shoulder. Outside `panel`, exactly as the creative tabs
    // are, so both are asked about before the test that decides a click was aimed at
    // the world.
    Rectangle trash{};

    // The chest, and nothing at all when none is open.
    Rectangle store{};

    Rectangle search{};
    Rectangle find{};
    Rectangle sort{};
    Rectangle rules{};

    // The line under the store saying how a row is set aside.
    //
    // Reserved here rather than drawn wherever there happened to be room, and that is
    // §25.1 in one field: a line of text placed by the panel that draws it is a line
    // nothing else knows about, and the first layout that grows by ten pixels puts it
    // off the bottom of the screen where no screenshot at the developer's window size
    // will ever show it. It is reserved whether or not the rules are showing, for the
    // same reason their column is.
    Rectangle hint{};

    // How many rows the chest's grid has: four per unit joined, and nought for no
    // chest.
    int storeRows = 0;

    bool Storing() const { return storeRows > 0; }

    int StoreSlots() const { return storeRows * kStoreColumns; }

    Rectangle Slot(int slot) const;
    Rectangle Tab(int tab) const;

    Rectangle StoreSlot(int slot) const;

    // The header beside one row of the chest, where the kind that row is set aside for
    // is shown and set.
    //
    // Its column is reserved whether or not the rules are showing, so turning them on
    // does not shift the grid out from under the player's hand — §14's rule about a
    // layout that is computed again rather than remembered, met from the other side.
    Rectangle Row(int row) const;
};

// The whole of it, for a chest of `storeRows` rows — nought where none is open.
Layout Of(int storeRows);

} // namespace pack
