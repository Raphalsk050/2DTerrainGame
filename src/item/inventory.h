#pragma once

#include "world/element.h"
#include "item/item_def.h"
#include "item/slots.h"
#include "core/mode.h"
#include "core/stack.h"

#include <array>

namespace pack {
struct Layout;
}

namespace save {
class Writer;
class Reader;
} // namespace save

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
//
// **What it is not is the rules of pouring.** Those moved to `item/slots.h` the day
// there was a second container in the game, and everything below that reads like an
// inventory doing arithmetic is a call through to there. See the head of that file:
// a chest and a bag pour by exactly the same rules, and two copies of them would part
// company the first time either was touched.
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

        // Whether the click landed on the pack or on anything hanging off it — the
        // bin, a tab. The caller uses it to keep one press from also being read by
        // the panel beside this one.
        bool took = false;
    };

    // The pages of the creative palette, and the tabs along the top of the panel
    // that choose between them.
    //
    // Three, and which page a thing lands on is *derived* rather than written into
    // the tables: a material is a block, an item that fixes to a surface or answers
    // to a tool is gear, and everything else a plant leaves behind is nature. Deriving it is what
    // keeps a new row in either table from having to remember to name its tab —
    // and if a fourth page is ever wanted, it is a rule here and not a field on
    // twenty-three rows.
    enum class Tab { Blocks, Nature, Gear, Count };

    static constexpr int kTabs = static_cast<int>(Tab::Count);

    // Reads the mouse over the open panel: picking a stack up onto the cursor,
    // putting it down, splitting it, sweeping it between the grid and the bar,
    // throwing it away, and dropping it in the bin.
    //
    // In creative the grid is not the player's own slots but the palette — see
    // Draw — so a click on one of them copies rather than moves. Everything the
    // bar does is the same in both modes, which is what lets one panel serve them.
    //
    // `at` is where the pack is this frame, worked out by `pack::Of` for both panels
    // at once rather than by each of them separately — §25.1's rule, and the reason
    // this no longer decides its own position.
    //
    // `store` is the chest standing open beside it, or nothing. It is here for one
    // gesture only: a shift-click sweeps into the store rather than between the bar
    // and the grid, which is what shift-click means in every game that has a chest.
    // Nothing about a chest reaches this file — a store is a run of slots.
    Gesture Update(Gamemode mode, const pack::Layout &at, slots::Bank *store = nullptr);

    // In survival, the player's own thirty-six slots.
    //
    // In creative, the same panel with the grid replaced by a page of every
    // material in the game and a row of tabs over it — Minecraft's arrangement,
    // and it is the same panel rather than a second one so that the bar, the
    // cursor, the tips and the layout cannot drift apart between the two modes.
    void Draw(Gamemode mode, const pack::Layout &at) const;

    // The name of what the cursor is over, beside the cursor, and the stack riding on
    // the cursor itself.
    //
    // Drawn by the pack today and by the chest panel beside it, which is why they are
    // out here rather than at the end of `Draw`: whichever panel the pointer is over,
    // the tip and the carried stack have to be the last two things on the screen or
    // the panel next door is drawn over them.
    static void DrawTip(const Stack &stack, Vector2 mouse, int font);
    void DrawCarried(const pack::Layout &at) const;

    // What is on one page, in the order the tables give it.
    //
    // A page is the twenty-seven slots of the grid and there are twenty-three
    // things in the world, so nothing is cut off today. If either table outgrows
    // its page the tail of it stops being reachable, and the fix is another tab
    // rather than a scrollbar.
    struct Page {
        std::array<Stack, kColumns * kRows> at{};
        int count = 0;

        // How many wanted on, against `count` which is how many fitted.
        //
        // The two are the same number in every build that is allowed to start, and
        // that is exactly what makes the difference worth carrying: overflowing a page
        // is silent — the tail is simply not listed, and the row that fell off looks
        // perfectly fine in the file it was added to. Counting the ones turned away is
        // what lets `inventory_checks.cpp` say so, and it lets it say so about the
        // *page* rather than about the table, which is the constraint that is actually
        // real. Twelve tools arrived in one commit and took the item table past the
        // twenty-seven a page holds while no page was anywhere near full.
        int wanted = 0;
    };

    static Page PageOf(Tab tab);

    // What a tab is called, on its own label and in anything that reports about it.
    //
    // A member rather than a helper beside the panel because the startup check names
    // the tab it is refusing, and a second copy of these three words is a second thing
    // to rename.
    static const char *NameOf(Tab tab);

    // A page is the grid, and everything in the game has to fit on one.
    //
    // The rule is checked at startup rather than here — see `inventory_checks.cpp`
    // — because the tables assemble themselves now and their sizes are not known
    // until they are frozen. It is checked at all because the failure is silent:
    // the palette would simply stop listing whatever was added last, and the row
    // would look perfectly fine in the file it was added to. When it fires, the fix
    // is another tab — see Tab, where the rule that sorts things into them lives.
    static constexpr int kSlotsPerPage = kColumns * kRows;

    const Stack &Carried() const { return carried_; }

    // The same, for the panel beside this one.
    //
    // There is one cursor because the player has one hand: a stack lifted out of a
    // chest and put down in the bag is one gesture and not a handover between two
    // containers, and it is the pack that owns it because it is the pack that
    // outlives the chest being shut.
    Stack &Carrying() { return carried_; }

    // Takes whatever is on the cursor away, for a caller that is closing the
    // panel. A stack left on the cursor when the panel shuts would be held by
    // nothing and drawn nowhere, which is how an inventory eats things.
    Stack Release();

    // The player's thirty-six as a run of slots, for the rules in `item/slots.h`.
    slots::Bank Run() { return slots::Bank(slots_.data(), kSlots); }

    // Puts as much of `stack` away as will fit and returns what would not go.
    int Add(Stack stack);

    // How many of `stack` would go in, without putting any of it away.
    int Room(const Stack &stack) const;

    // How many of a thing there are across every slot.
    int Tally(const Stack &like) const;

    // Takes `count` of a thing from wherever it is lying, in whatever number of
    // slots that means, and says whether there was enough. All or nothing.
    bool Remove(const Stack &what);

    const Stack &At(int slot) const { return slots_[static_cast<std::size_t>(slot)]; }
    Stack &At(int slot) { return slots_[static_cast<std::size_t>(slot)]; }

    // Lifts up to `count` off one slot and returns what came away.
    Stack Take(int slot, int count);

    // Puts `stack` into one slot, returning whatever was displaced.
    Stack Put(int slot, Stack stack);

    int Selected() const { return selected_; }

    // Wrapped rather than clamped, so stepping past either end of the bar
    // continues into the other, the way a bar is expected to behave.
    void Select(int slot);

    // The slot the bar points at, which is what the brush and the hand act
    // through.
    const Stack &Held() const { return slots_[static_cast<std::size_t>(selected_)]; }

    // Puts `by` blows' worth of wear on whatever is in hand, and takes it away when
    // there is nothing left in it. Returns whether it broke on this one.
    //
    // Here rather than at each hand that swings, and that is the same argument
    // `World::PlaceCell` makes about refusing a cell: an inventory is the only thing
    // that may empty one of its own slots, and a caller doing it for itself is a
    // caller that can leave a slot holding a row with a count of nothing. There are
    // three things that wear a tool — a block broken, a tree struck, a creature hit —
    // and they must not be three answers to what happens on the last blow.
    //
    // Nothing at all where what is held does not wear, which is most of what a player
    // carries and every material.
    bool WearHeld(int by = 1);

    void Clear();

    // The thirty-six slots and which one is in hand.
    //
    // The cursor is deliberately not written. A stack on the cursor is a gesture
    // half-made, and the loop already puts one back into the world when the panel
    // shuts — so there is no moment at which a save can be taken with something held,
    // and writing the field would be writing a state the game cannot be in.
    void Save(save::Writer &out) const;
    void Load(save::Reader &in);

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
    // Moves the whole of one slot to the other half of the inventory — grid to
    // bar, bar to grid — or into the store standing open, and leaves behind
    // whatever would not go.
    void Sweep(int slot, slots::Bank *store);

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
