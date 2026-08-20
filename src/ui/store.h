#pragma once

#include "item/inventory.h"
#include "item/slots.h"
#include "ui/pack.h"
#include "raylib.h"

#include <vector>

// The chest, standing open beside the pack.
//
// It draws a run of slots and knows nothing at all about chests. What it is handed is
// a `slots::Bank` and a rule per row, both of which point into whatever owns them —
// so this file can be photographed against an array made up on the spot, with no
// world, no fixture and no cell anywhere near it. That is `--chest`'s whole
// possibility, and it is the same argument `Crafting::Draw` makes about being drawn by
// the probe rather than reproduced by it (§28.7).
//
// **The cursor is not its own.** There is one carried stack and it lives on the
// `Inventory`, because the player has one hand and because a stack lifted out of a
// chest and put down in the bag is one gesture rather than a handover between two
// containers. This panel acts through `Inventory::Carrying`, and it is why the pack is
// passed in rather than only the bank.
class Store {
public:
    // What one frame of clicking on the panel came to.
    struct Gesture {
        // Whether the click landed on the panel at all. The caller uses it to keep the
        // same press from also being read by the pack beside it — `Crafting::Gesture`'s
        // `took`, and for its reason.
        bool took = false;

        // Whether the search field has the keyboard this frame.
        //
        // Reported rather than asked for, because what it gates is in the loop: while
        // somebody is typing a search, Tab must not close the panel and the number row
        // must not move the hotbar. The loop already keeps exactly this flag for the
        // console.
        bool typing = false;
    };

    // `bank` is the store's slots and `rules` is one pointer per row, in the same
    // order. Both are views into the chests themselves, so what the player puts in a
    // slot is in the chest the moment they let go and there is nothing to write back.
    Gesture Update(Inventory &pack, slots::Bank &bank, const std::vector<slots::Kind *> &rules,
                   const pack::Layout &at);

    void Draw(const Inventory &pack, const slots::Bank &bank, const std::vector<slots::Kind *> &rules,
              const pack::Layout &at) const;

    bool Typing() const { return typing_; }

    // Whether the rules column is showing.
    bool Ruling() const { return ruling_; }

    // Shows or hides it, for a caller that is not a click.
    //
    // `Crafting::Open` exists for exactly this and says why: the panel opens its own
    // modes from a press, and a probe has no mouse to press with. Without it `--chest`
    // could photograph the headers only by reproducing them, which is a picture of the
    // reproduction (§25.5).
    void Ruling(bool showing) { ruling_ = showing; }

    // What is being searched for, likewise for a caller that has no keyboard.
    //
    // The search is the one thing in this panel that leaves no trace in the world: it
    // is a filter over a drawing, so a probe that could not set it could not photograph
    // it, and the whole point of §28.7 is that a mechanic nobody can photograph is a
    // mechanic nobody checks.
    void Seeking(const char *text);

    // Called when the chest is shut.
    //
    // The search and the rule mode are about *this look at this chest*, not about the
    // chest: a player who opened a store, searched it for wood and shut it has finished
    // that question, and finding the filter still on the next time they open one is a
    // panel that appears to be broken. The rules themselves are not touched — those are
    // the chest's and are meant to outlive being shut.
    void Shut();

private:
    // Whether a stack answers what is being searched for.
    bool Matches(const Stack &stack) const;

    // As many letters as fit in the field at the smallest layout, and one for the
    // terminator. A search longer than this is a search for one thing.
    static constexpr int kMostLetters = 24;

    char find_[kMostLetters + 1]{};
    int letters_ = 0;

    bool typing_ = false;
    bool ruling_ = false;
};
