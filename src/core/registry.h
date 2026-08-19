#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// A table nobody writes down.
//
// Every kind of content in this world used to be an array with every row in it —
// `kElements`, `kItems`, `kKinds`, and a parallel `enum` naming the positions. It
// worked, and it has one fault that gets worse with every row added: adding a
// thing means opening a file that already works and inserting into the middle of
// it. That is the Open/Closed principle stated as a complaint — the file is closed
// for a reason, it is full of things that are correct, and every edit to it is a
// chance to break one of them.
//
// So a row registers itself. A new item, creature or material is a **new file**,
// and no existing file learns about it: the row is defined there, a registrar
// beside it puts it in this table, and the build picks the file up because the
// build globs the directory rather than listing it.
//
// ---
//
// **The identity problem, which is the whole difficulty.**
//
// A row's id is stored in `Stack::what` and will be stored in save data, so it has
// to be the same number on every build of the same content. Static initialisers
// run in an order the standard does not fix and the linker is free to change, so
// *registration order is not an identity*. Assigning ids as rows arrive would mean
// a world saved today loading as something else tomorrow, silently, with stone
// where the diamond was.
//
// The fix is that ids are assigned by `Freeze`, after every registrar has run, by
// sorting on the row's own name. A name is the one thing about a row that is
// already required to be what it says, and the resulting numbering is a pure
// function of *the set of rows* — nothing to do with the order they arrived in,
// the order the files were compiled in, or the platform.
//
// **Freezing happens once, from `content::Open` at startup.** Reading an id before
// that is a fault the table catches outright rather than a race that shows up as
// odd behaviour a week later.
//
// ---
//
// **What was given up, and what replaced it.**
//
// The old tables were `constexpr`, and a handful of `static_assert`s walked them at
// compile time — that every item picture was six rows of six, that every fixture
// named an item the hand would put down. Those cannot survive a table assembled at
// run time, and pretending otherwise would be worse than losing them.
//
// They are checks registered with `OnVerify` now, run together at startup, and they
// **report every fault and then stop the program**. That is a weaker guarantee than
// a compile error and a much stronger one than nothing: a bad row is found before
// the window opens, named, and impossible to play past. What must never happen is
// a check quietly downgraded to a warning — the whole argument in CLAUDE.md §16.2b
// is that a row which looks added and is not costs a day of looking in the wrong
// place.
namespace registry {

// Everything that has to be sealed before the game reads content, and everything
// that has to be checked once it is.
//
// Function pointers in a list rather than calls in a startup routine, and for the
// reason the whole file exists: a startup routine naming every table is a central
// list, and a new kind of content would mean editing it. A table puts its own
// freezer in here the first time anybody touches it.
std::vector<void (*)()> &Freezers();

// A check returns an empty string when it is happy, and otherwise says what is
// wrong in a line a person can act on.
std::vector<std::string (*)()> &Checks();

// What each table says it holds, for the startup line. See `content::Summary` for
// why a count is worth printing at all.
std::vector<std::string (*)()> &Counters();

inline void OnFreeze(void (*freeze)()) {
    Freezers().push_back(freeze);
}

inline void OnVerify(std::string (*check)()) {
    Checks().push_back(check);
}

inline void OnCount(std::string (*count)()) {
    Counters().push_back(count);
}

// A row's position, once the table is frozen.
//
// A struct rather than a bare int so that an item id cannot be passed where a
// material id is wanted. That mistake is otherwise silent — both are small
// integers, and the wrong one is a valid index into the wrong table.
template <class Def>
struct Handle {
    int index = -1;

    bool Known() const { return index >= 0; }

    friend bool operator==(Handle a, Handle b) { return a.index == b.index; }
    friend bool operator!=(Handle a, Handle b) { return a.index != b.index; }
};

template <class Def>
class Table {
public:
    // The one table for this kind of row.
    //
    // A function-local static rather than a namespace-scope object, which is the
    // whole defence against the static initialisation order fiasco: a registrar in
    // any translation unit can call this during its own construction and is
    // guaranteed to get a constructed table, whatever order the units were linked
    // in.
    static Table &The() {
        static Table one;

        return one;
    }

    // Puts a row in. Called from a registrar at static initialisation time, before
    // main, and never afterwards.
    void Add(const Def *def) {
        // Adding after the ids have been handed out would renumber the table under
        // whatever is already holding an id. There is no way to do that correctly,
        // so it is refused rather than handled.
        if (frozen_) {
            std::fprintf(stderr, "registry: '%s' registered after its table was frozen\n", def->name);
            std::abort();
        }

        rows_.push_back(def);
    }

    // Sorts by name and hands out the ids. Idempotent, so a second call is a no-op
    // rather than a renumbering.
    void Freeze() {
        if (frozen_) return;

        std::sort(rows_.begin(), rows_.end(),
                  [](const Def *a, const Def *b) { return std::strcmp(a->name, b->name) < 0; });

        frozen_ = true;
    }

    bool Frozen() const { return frozen_; }

    int Size() const { return static_cast<int>(rows_.size()); }

    const Def &At(Handle<Def> id) const { return *rows_[static_cast<std::size_t>(id.index)]; }

    const Def &At(int index) const { return *rows_[static_cast<std::size_t>(index)]; }

    // The id of a row, from the row itself.
    //
    // How an accessor written beside a row finds its own number: the row's address
    // is known at compile time, the number is not, and this is the join. Linear,
    // and deliberately so — it is called once per accessor per run, cached in a
    // function-local static by the accessor, and a binary search over a table of
    // forty would save nothing measurable while being one more thing to get wrong.
    Handle<Def> IdOf(const Def *def) const {
        Demand();

        for (int i = 0; i < Size(); i++) {
            if (rows_[static_cast<std::size_t>(i)] == def) return {i};
        }

        std::fprintf(stderr, "registry: '%s' was never registered — is its .cpp in the build?\n", def->name);
        std::abort();

        return {};
    }

    // The id of a row by name, or nothing.
    //
    // What one table uses to name a row in another — a fixture naming its item, a
    // creature naming its behaviour. It returns nothing rather than aborting,
    // because a missing name is a content fault to be *reported* with every other
    // one at startup and not a crash at the first of them.
    std::optional<Handle<Def>> Find(const char *name) const {
        Demand();

        for (int i = 0; i < Size(); i++) {
            if (std::strcmp(rows_[static_cast<std::size_t>(i)]->name, name) == 0) return Handle<Def>{i};
        }

        return std::nullopt;
    }

    // Every row, in id order, for walking the whole table.
    const std::vector<const Def *> &All() const { return rows_; }

private:
    Table() {
        // The table puts its own freezer in the list the moment it exists, which is
        // during the first registrar's constructor. Nothing has to know this table
        // is here for it to be sealed at startup.
        OnFreeze(+[] { Table::The().Freeze(); });

        // And its own count, under the label its row type carries. `Def::kLabel` is
        // the one thing a row type has to declare beyond a `name` field, and it is
        // there so that this line can be written without a central list naming
        // every table in the game.
        OnCount(+[] { return std::to_string(Table::The().Size()) + " " + Def::kLabel; });
    }

    void Demand() const {
        if (frozen_) return;

        std::fprintf(stderr, "registry: read before freezing — content::Open() has to run first\n");
        std::abort();
    }

    std::vector<const Def *> rows_;
    bool frozen_ = false;
};

// What puts one row in its table.
//
// Declared beside the row, in the row's own file, doing its work from its
// constructor before main runs. That is the whole of "a row registers itself":
// there is no list anywhere that has to name it, and no file that already works has
// to be opened to add one.
template <class Def>
struct Registrar {
    explicit Registrar(const Def &def) { Table<Def>::The().Add(&def); }
};

// What puts one check in the list, on the same terms.
struct Checker {
    explicit Checker(std::string (*check)()) { OnVerify(check); }
};

} // namespace registry
