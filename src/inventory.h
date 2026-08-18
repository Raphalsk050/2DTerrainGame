#pragma once

#include "mode.h"
#include "stack.h"

#include <array>

// Everything the player is carrying.
//
// One flat run of slots with the bar at the front of it, rather than a bar and a
// grid kept apart as two containers. Moving a stack between the two is the
// commonest thing an inventory is ever asked to do, and as two containers every
// such move is a special case that has to know which side it started on; as one
// run it is an exchange of two indices and there is no side.
//
// It is also what makes the row along the bottom of the screen a *view* of the
// first nine slots rather than a store of its own, so there is exactly one
// answer to what the player has and the bar cannot disagree with the panel.
class Inventory {
public:
    // The Minecraft arrangement: nine across, three rows of it, and the bar as a
    // fourth row set a little apart.
    static constexpr int kColumns = 9;
    static constexpr int kRows    = 3;

    // The bar is the first nine, and it is first rather than last so that a slot
    // index and a key on the number row agree without arithmetic.
    static constexpr int kOnHand = kColumns;
    static constexpr int kSlots  = kColumns * kRows + kOnHand;

    // Every index passed here is expected to be in [0, kSlots). Nothing clamps:
    // the callers are hit tests and loops that already know the bounds, and a
    // clamp would turn an index that came out wrong into a stack quietly landing
    // in the wrong slot.

    // What one frame of clicking over the open panel came to.
    //
    // The panel cannot act on either of these itself. Throwing a stack into the
    // world needs the world and the player's position, and closing is the
    // caller's state to keep — so both are reported and neither is done here.
    struct Gesture {
        // A stack the player threw out of the panel, for the caller to put on
        // the ground.
        Stack thrown{};

        // Whether the click asked for the panel to be closed.
        bool close = false;
    };

    // The pages of the creative palette, and the tabs along the top of the panel
    // that choose between them.
    //
    // Three, and which page a thing lands on is *derived* rather than written into
    // the tables: a material is a block, an item that fixes to a surface is gear,
    // and everything else a plant leaves behind is nature. Deriving it is what
    // keeps a new row in either table from having to remember to name its tab —
    // and if a fourth page is ever wanted, it is a rule here and not a field on
    // twenty-three rows.
    enum class Tab { Blocks, Nature, Gear, Count };

    static constexpr int kTabs = static_cast<int>(Tab::Count);

    // Reads the mouse over the open panel: picking a stack up onto the cursor,
    // putting it down, splitting it, sweeping it between the grid and the bar,
    // and throwing it away.
    //
    // In creative the grid is not the player's own slots but the palette — see
    // Draw — so a click on one of them copies rather than moves. Everything the
    // bar does is the same in both modes, which is what lets one panel serve them.
    Gesture Update(Gamemode mode);

    // In survival, the player's own thirty-six slots.
    //
    // In creative, the same panel with the grid replaced by a page of every
    // material in the game and a row of tabs over it — Minecraft's arrangement,
    // and it is the same panel rather than a second one so that the bar, the
    // cursor, the tips and the layout cannot drift apart between the two modes.
    void Draw(Gamemode mode) const;

    // Where the tabs sit, above the panel's top edge.
    static Rectangle TabBounds(int tab);

    // What is on one page, in the order the tables give it.
    //
    // A page is the twenty-seven slots of the grid and there are twenty-three
    // things in the world, so nothing is cut off today. If either table outgrows
    // its page the tail of it stops being reachable, and the fix is another tab
    // rather than a scrollbar.
    struct Page {
        std::array<Stack, kColumns * kRows> at{};
        int count = 0;
    };

    static Page PageOf(Tab tab);

    // Where the panel is on screen this frame, and whether a point is on it.
    static Rectangle Bounds();
    static Rectangle SlotBounds(int slot);
    static bool Contains(Vector2 screen);

    const Stack &Carried() const { return carried_; }

    // Takes whatever is on the cursor away, for a caller that is closing the
    // panel. A stack left on the cursor when the panel shuts would be held by
    // nothing and drawn nowhere, which is how an inventory eats things.
    Stack Release();

    // Puts as much of `stack` away as will fit and returns what would not go.
    //
    // Nothing is dropped silently. A caller that ignores the remainder has to
    // have decided that losing it is right, and mostly it is not — what does not
    // fit belongs on the ground.
    int Add(Stack stack);

    // How many of `stack` would go in, without putting any of it away.
    int Room(const Stack &stack) const;

    // How many of a thing there are across every slot.
    int Tally(const Stack &like) const;

    // Takes `count` of a thing from wherever it is lying, in whatever number of
    // slots that means, and says whether there was enough. All or nothing: a
    // half-spent cost leaves the world changed and the player charged for
    // something that did not happen.
    bool Remove(const Stack &what);

    const Stack &At(int slot) const { return slots_[static_cast<std::size_t>(slot)]; }
    Stack &At(int slot) { return slots_[static_cast<std::size_t>(slot)]; }

    // Lifts up to `count` off one slot and returns what came away.
    Stack Take(int slot, int count);

    // Puts `stack` into one slot, returning whatever was displaced — which is
    // the whole of the exchange the cursor performs, including the case where
    // the two merge and nothing comes back.
    Stack Put(int slot, Stack stack);

    int Selected() const { return selected_; }

    // Wrapped rather than clamped, so stepping past either end of the bar
    // continues into the other, the way a bar is expected to behave.
    void Select(int slot);

    // The slot the bar points at, which is what the brush and the hand act
    // through.
    const Stack &Held() const { return slots_[static_cast<std::size_t>(selected_)]; }

    void Clear();

    // Tops the inventory up with a full stack of every material.
    //
    // The terrain in this project is tuned by walking around in it and building
    // against it, which the palette used to make possible by handing out every
    // material endlessly. Survival took that away, and this is what gives it
    // back — a key for aiming the world, in the same family as the probes and
    // the F3 overlays, and not a way to play.
    //
    // Whatever there is no room for is discarded, which is the one place in here
    // that losing something is right: it is a debug key, and what it is for is
    // having enough of everything rather than an exact amount of anything.
    void Stock();

    static constexpr bool OnHand(int slot) { return slot >= 0 && slot < kOnHand; }

private:
    // Pours as much of `stack` as will fit into the slots in [from, upto), and
    // leaves the rest in it.
    //
    // The range is what makes a shift-click one call: sweeping a stack out of
    // the bar means offering it to the grid and nowhere else, and offering it to
    // everywhere would put it straight back where it came from.
    void Fill(Stack &stack, int from, int upto);

    // Moves the whole of one slot to the other half of the inventory — grid to
    // bar, bar to grid — and leaves behind whatever would not go.
    void Sweep(int slot);

    std::array<Stack, kSlots> slots_{};

    // Which page of the palette is up. Held here rather than in the loop for the
    // reason `selected_` is: it is a thing the player set and expects to find as
    // they left it, and the panel is what they set it on.
    Tab tab_ = Tab::Blocks;

    // What the cursor is holding.
    //
    // Kept here rather than beside the panel because it is the player's
    // property and not the panel's: it has to survive the panel closing, and
    // there has to be one place that knows the whole of what is carried.
    Stack carried_{};

    int selected_ = 0;
};
