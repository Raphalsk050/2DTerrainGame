#include "item/slots.h"

#include <algorithm>
#include <vector>

namespace {

// A slot that belongs to nobody, handed back for an index outside the bank.
//
// A bank is addressed by hit tests and by loops that already know its size, exactly as
// the inventory's own slots were — so this is a backstop and not a clamp. A clamp
// would turn an index that came out wrong into a stack quietly landing in the wrong
// slot, which is the one failure a store must not have.
Stack &Nowhere() {
    static Stack nothing{};

    nothing = {};

    return nothing;
}

} // namespace

bool slots::Bank::Join(Stack *at, int count) {
    if (at == nullptr || count <= 0) return false;
    if (used_ >= kMostParts) return false;

    parts_[static_cast<std::size_t>(used_)] = {.at = at, .count = count};

    used_++;
    size_ += count;

    return true;
}

int slots::Bank::PartSize(int part) const {
    if (part < 0 || part >= used_) return 0;

    return parts_[static_cast<std::size_t>(part)].count;
}

int slots::Bank::PartOf(int slot) const {
    int at = 0;

    for (int part = 0; part < used_; part++) {
        const int count = parts_[static_cast<std::size_t>(part)].count;

        if (slot < at + count) return part;

        at += count;
    }

    return -1;
}

Stack &slots::Bank::At(int slot) {
    if (slot < 0) return Nowhere();

    int at = 0;

    for (int part = 0; part < used_; part++) {
        const Part &run = parts_[static_cast<std::size_t>(part)];

        if (slot < at + run.count) return run.at[slot - at];

        at += run.count;
    }

    return Nowhere();
}

const Stack &slots::Bank::At(int slot) const {
    return const_cast<Bank *>(this)->At(slot);
}

void slots::Bank::Fill(Stack &stack, int from, int upto) {
    if (stack.Empty()) return;

    const int first = std::max(from, 0);
    const int last  = std::min(upto, size_);

    // Onto a stack of the same thing before an empty slot.
    //
    // Topping up first is the half that matters: filling empty slots first would
    // scatter one material over four of them while a half-full one of it sat in the
    // bar.
    for (int slot = first; slot < last && stack.count > 0; slot++) {
        Stack &into = At(slot);
        if (!into.Alike(stack)) continue;

        const int fits = std::min(into.Room(), stack.count);

        into.count += fits;
        stack.count -= fits;
    }

    for (int slot = first; slot < last && stack.count > 0; slot++) {
        Stack &into = At(slot);
        if (!into.Empty()) continue;

        const int fits = std::min(stack.Limit(), stack.count);

        into = stack.Some(fits);
        stack.count -= fits;
    }
}

int slots::Bank::Add(Stack stack) {
    Fill(stack, 0, size_);

    return stack.count;
}

int slots::Bank::Room(const Stack &stack) const {
    if (stack.Empty()) return 0;

    int room = 0;

    for (int slot = 0; slot < size_; slot++) {
        const Stack &at = At(slot);

        if (at.Alike(stack)) room += at.Room();
        else if (at.Empty()) room += stack.Limit();
    }

    return room;
}

int slots::Bank::Tally(const Stack &like) const {
    int held = 0;

    for (int slot = 0; slot < size_; slot++) {
        const Stack &at = At(slot);

        if (at.Alike(like)) held += at.count;
    }

    return held;
}

bool slots::Bank::Remove(const Stack &what) {
    if (what.Empty() || Tally(what) < what.count) return false;

    int owing = what.count;

    // From the back, so what is spent comes out of the store before it comes out of
    // the bar. The bar is what the player arranged; the grid is where the spare went.
    for (int slot = size_ - 1; slot >= 0 && owing > 0; slot--) {
        Stack &from = At(slot);
        if (!from.Alike(what)) continue;

        const int taken = std::min(from.count, owing);

        from.count -= taken;
        owing -= taken;

        if (from.count <= 0) from = {};
    }

    return true;
}

Stack slots::Bank::Take(int slot, int count) {
    Stack &from = At(slot);

    if (from.Empty() || count <= 0) return {};

    const int taken = std::min(count, from.count);

    // Through Some, so that what comes off a slot is the same *thing* that was in it —
    // including how worn it is. Built field by field, a pickaxe picked up off the
    // ground came back as good as new, which is a repair nobody meant to write.
    const Stack away = from.Some(taken);

    from.count -= taken;

    // Emptied all the way back to nothing rather than left holding a row with a count
    // of zero. Alike() and Empty() both read the count, but an empty slot that still
    // remembers what used to be in it would merge with the next stack of that thing to
    // come past and refuse every other, which is a slot that is full of nothing in
    // particular.
    if (from.count <= 0) from = {};

    return away;
}

Stack slots::Bank::Put(int slot, Stack stack) {
    Stack &into = At(slot);

    if (stack.Empty()) return {};

    if (into.Empty()) {
        const int fits = std::min(stack.Limit(), stack.count);

        into = stack.Some(fits);
        stack.count -= fits;

        return (stack.count > 0) ? stack : Stack{};
    }

    if (into.Alike(stack)) {
        const int fits = std::min(into.Room(), stack.count);

        into.count += fits;
        stack.count -= fits;

        // What is left over stays on the cursor rather than going anywhere else. A
        // stack that overflowed into some other slot the player was not looking at is a
        // stack that has moved on its own.
        return (stack.count > 0) ? stack : Stack{};
    }

    // Two different things, so they exchange places. This is what makes a slot
    // reachable in one gesture when it is already occupied.
    const Stack displaced = into;
    into                  = stack;

    return displaced;
}

int slots::Bank::Held() const {
    int held = 0;

    for (int slot = 0; slot < size_; slot++) held += At(slot).count;

    return held;
}

void slots::Bank::Clear() {
    for (int slot = 0; slot < size_; slot++) At(slot) = {};
}

void slots::Sort(Bank &bank, int columns, const Kind *rows, int count) {
    if (bank.Empty() || columns <= 0) return;

    // Everything that was in it, as it was in it.
    //
    // Lifted whole rather than rebuilt from a kind and a total, which is the one
    // subtlety in here: a stack carries `wear`, and two pickaxes worn differently are
    // two things. Rebuilding from `(kind, count)` would hand the player back a pair of
    // brand new ones, which is §29.3's repair-by-being-dropped in another costume.
    std::vector<Stack> loose;

    loose.reserve(static_cast<std::size_t>(bank.Size()));

    for (int slot = 0; slot < bank.Size(); slot++) {
        const Stack &at = bank.At(slot);

        if (!at.Empty()) loose.push_back(at);
    }

    bank.Clear();

    // The rules first, each into its own row, so a row that was set aside for
    // cobblestone holds cobblestone even when there is not much of it. Sorting the
    // remainder into it afterwards would put whatever came alphabetically first in the
    // row the player reserved, which is the whole of what a rule is for.
    for (int row = 0; row < count; row++) {
        const Kind &rule = rows[row];
        if (!rule.Any()) continue;

        const int from = row * columns;
        const int upto = std::min(from + columns, bank.Size());

        if (from >= bank.Size()) continue;

        for (Stack &one : loose) {
            if (!rule.Is(one)) continue;

            bank.Fill(one, from, upto);
        }
    }

    // What is left, in the tables' own order.
    std::stable_sort(loose.begin(), loose.end(), [](const Stack &a, const Stack &b) {
        if (a.holds != b.holds) return static_cast<int>(a.holds) < static_cast<int>(b.holds);
        if (a.what != b.what) return a.what < b.what;

        // The fullest first, so topping up leaves at most one part stack of a thing
        // rather than several.
        return a.count > b.count;
    });

    // Which slots a rule has spoken for. A ruled row keeps its slack: an unruled
    // material poured into the gap at the end of the cobblestone row would be exactly
    // the thing the row was reserved to stop.
    const auto ruled = [&](int slot) {
        const int row = slot / columns;

        return row < count && rows[row].Any();
    };

    for (Stack &one : loose) {
        if (one.Empty()) continue;

        for (int slot = 0; slot < bank.Size() && one.count > 0; slot++) {
            if (ruled(slot)) continue;

            bank.Fill(one, slot, slot + 1);
        }
    }

    // And whatever still has nowhere to go takes the slack after all.
    //
    // Only here, and only because the alternative is losing it. A bank held all of this
    // a moment ago and pouring alike stacks together can only ever need fewer slots, so
    // this is unreachable in a bank that was not already full to the brim — but "cannot
    // happen" is not a place to put an item somebody dug for.
    for (Stack &one : loose) {
        if (one.Empty()) continue;

        bank.Fill(one, 0, bank.Size());
    }
}
