#pragma once

#include "core/stack.h"

#include <array>
#include <cstdint>

// A run of slots, and the rules for pouring things into it.
//
// These rules used to live in `Inventory` and were the whole of what an inventory
// did: top up an alike stack before filling an empty one, never merge two holes,
// take part of a stack away through `Stack::Some` so its wear goes with it, empty a
// slot all the way back to nothing rather than leaving it holding a row with a count
// of zero. Every one of those is a rule somebody had to get wrong once before it was
// written down, and CLAUDE.md §29.3 is the account of the last of them.
//
// A chest is a second container. Copying those rules into it would be a second set of
// answers to the same questions, and the two would part company the first time either
// was touched — which is the fault this project keeps naming, from the two copies of
// the marching-squares blend (§13.7) to the amber written out in two panels (§28.6).
// So the rules moved here and both containers are the same code seen twice.
//
// It owns nothing. What holds the storage is the inventory's own array, or one chest,
// or the run of chests a player has joined together — and that last one is why this is
// a *bank* of parts rather than a span. Three chests standing side by side are one
// store to the player and three separate arrays in memory, because breaking one has to
// drop that one's contents and leave the rest (rule 6 of the chest, and the reason the
// units are never merged into a single buffer).
namespace slots {

// How many separate runs make one bank.
//
// Three chests joined, and the player's own thirty-six is one. There is no reason for
// this to be larger than the longest join any fixture allows, and a bank that quietly
// dropped a part would be a store with slots nothing could reach.
inline constexpr int kMostParts = 4;

// How wide a store's grid is, in slots.
//
// Here rather than in the panel that draws it, because it is not only a drawing. A
// sorting rule is about a *row*, and a row has to belong to exactly one chest or the
// rule would have nowhere to live that survives the bank being taken apart — so the
// number that decides where a row ends is the same number the grid is drawn at, and
// there had better be one of it. `fixture::Def::slots` is checked against this at
// startup for the same reason.
inline constexpr int kAcross = 8;

// A kind of thing, without any count of it.
//
// What a sorting rule names, and what two stacks have in common when `Alike` says they
// may be poured together. It deliberately says nothing about wear: two pickaxes worn
// differently are the same *kind* and are still two things, which is exactly why they
// cannot merge — a tool's stack limit is one, so `Room` is nought and the pouring rules
// keep them apart without this having to know about it.
struct Kind {
    Holds holds       = Holds::Nothing;
    std::uint8_t what = 0;

    bool Any() const { return holds != Holds::Nothing; }

    bool Is(const Stack &stack) const {
        return Any() && !stack.Empty() && stack.holds == holds && stack.what == what;
    }

    bool Same(const Kind &other) const { return holds == other.holds && what == other.what; }

    static Kind Of(const Stack &stack) {
        if (stack.Empty()) return {};

        return {.holds = stack.holds, .what = stack.what};
    }
};

// One store, made of up to `kMostParts` runs laid end to end.
//
// Cheap to build and meant to be built where it is used rather than kept: it is a way
// of *addressing* storage somebody else owns, and a bank held across a frame is a bank
// pointing at a chest that may have been dug up.
class Bank {
public:
    Bank() = default;

    Bank(Stack *at, int count) { Join(at, count); }

    // Adds one run to the end of the bank. Anything past `kMostParts` is refused
    // rather than dropped quietly — see the head of this file.
    bool Join(Stack *at, int count);

    int Size() const { return size_; }
    bool Empty() const { return size_ == 0; }

    // How many parts, and how long each is. The panel needs this to know which chest a
    // row of the grid belongs to, which is what makes a sorting rule about a row
    // survive its bank being taken apart.
    int Parts() const { return used_; }
    int PartSize(int part) const;

    // Which part a slot falls in, and where that part starts.
    int PartOf(int slot) const;

    Stack &At(int slot);
    const Stack &At(int slot) const;

    // Pours as much of `stack` as will fit into the slots in [from, upto), and leaves
    // the rest in it.
    //
    // The range is what makes a shift-click one call: sweeping a stack out of the bar
    // means offering it to the grid and nowhere else, and offering it to everywhere
    // would put it straight back where it came from.
    void Fill(Stack &stack, int from, int upto);

    // Puts as much of `stack` away as will fit and returns what would not go.
    //
    // Nothing is dropped silently. A caller that ignores the remainder has to have
    // decided that losing it is right, and mostly it is not — what does not fit
    // belongs on the ground.
    int Add(Stack stack);

    // How many of `stack` would go in, without putting any of it away.
    int Room(const Stack &stack) const;

    // How many of a thing there are across every slot.
    int Tally(const Stack &like) const;

    // Takes `count` of a thing from wherever it is lying, in whatever number of slots
    // that means, and says whether there was enough. All or nothing: a half-spent cost
    // leaves the world changed and the player charged for something that did not
    // happen.
    bool Remove(const Stack &what);

    // Lifts up to `count` off one slot and returns what came away.
    Stack Take(int slot, int count);

    // Puts `stack` into one slot, returning whatever was displaced — which is the whole
    // of the exchange the cursor performs, including the case where the two merge and
    // nothing comes back.
    Stack Put(int slot, Stack stack);

    // Everything in it, counted. What `Sort` must not change, and what the probe
    // checks it against.
    int Held() const;

    void Clear();

private:
    struct Part {
        Stack *at = nullptr;
        int count = 0;
    };

    std::array<Part, kMostParts> parts_{};

    int used_ = 0;
    int size_ = 0;
};

// Tidies a bank: alike stacks poured together, and the rest laid out in one order.
//
// The order is `(holds, what)`, which is materials before items and alphabetical
// within each — not because anybody chose alphabetical, but because `content::Open`
// hands out ids by sorting on each row's own name (§19.1). Sorting on the id *is*
// sorting by name, so there is no second ordering written down anywhere to fall out of
// step with the first.
//
// `rows` is the sorting rules, one kind per row of `columns` slots, and `count` is how
// many rows there are. A row with a rule holds that kind and nothing else; everything
// else fills what is left, in order, and spills into the slack of a ruled row only
// where there is nowhere else for it to go. Nothing is ever lost: what came out of the
// bank goes back into it, and `Held()` is the same either side.
void Sort(Bank &bank, int columns, const Kind *rows, int count);

} // namespace slots
