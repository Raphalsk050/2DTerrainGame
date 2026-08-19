#pragma once

#include "entity/mob/life.h"
#include "entity/mob/patch.h"
#include "raylib.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

class World;

namespace mob {

// Where the creatures of a world are, including the ones nobody is looking at.
//
// This replaced a spawner that was *stateless*, and the difference is the whole
// point. The old one tried a random spot every second or so and let anything outside
// the view go; walking a few screens and back therefore gave a different set of
// animals every time, killing one was undone by turning round, and a place had no
// population so much as a rate. None of that is what a world is.
//
// Here a place has a population, the population is a fact about the place, and what
// happens to it sticks. Read `patch.h` first — the design is one field of it.
//
// **Nothing here is a function of the view.** The view decides which patches are
// paged in, and that is all it decides. Which creatures exist, where they are and
// whether they are alive are answers the ground holds.
//
// What this is not: a save file. Nothing is written to disk yet, so a world still
// starts fresh — but it starts *the same* fresh, because the first roll of a cell is
// a pure function of the cell and the seed. When there are saves, this map is what
// goes in one, and its shape was chosen with that in mind.
class Warren {
public:
    // How big a square of country is remembered as one unit, in world pixels.
    //
    // Wide enough that a screen spans a handful rather than dozens — paging is the
    // one thing here with a cost, and it is paid per patch. Tall enough that the
    // surface band and the cave band under it are separate records, so a player
    // walking about on top does not keep waking the bats underneath.
    //
    // Not the world's chunk size, deliberately. A chunk is how the ground is
    // streamed and its size is a rendering decision; tying the two together would
    // mean a change to one silently rearranged the other.
    static constexpr float kSpan = 512.0f;
    static constexpr float kRise = 384.0f;

    // How far outside the simulated region a patch is still woken, and how far
    // outside that one is put back to sleep.
    //
    // Two figures rather than one, and the gap between them is the whole reason
    // there are two: a single edge means a creature standing on it is woken and
    // slept on alternate frames as the camera breathes, which is the churn this
    // module exists to remove. The wake edge is also outside the view on purpose, so
    // that a creature never appears in front of the player — it walks in.
    static constexpr float kWakeOut  = kSpan;
    static constexpr float kSleepOut = kSpan * 2.5f;

    void Configure(int seed) { seed_ = seed; }

    // Which patches the view now covers, settling and waking what it has to.
    //
    // Returns the creatures that should be brought to life this frame; the caller
    // puts them in the herd. It is handed back rather than pushed because the
    // warren has no business knowing how a herd stores anything.
    void Wake(const World &world, Rectangle active, float now, std::vector<Life> &out);

    // Puts one creature back to rest, filed under wherever it has got to.
    //
    // Wherever it has got to, and not where it came from: a boar that walked two
    // patches east belongs in the patch it is standing in, or coming back the way
    // you left would find it in a place it had already walked away from.
    void Rest(const Life &life);

    // Tells the warren a creature is gone for good.
    //
    // There is nothing to erase — the record of a creature is its absence — so this
    // only keeps the count the display and `--mobcheck` read. That it needs no more
    // than that is the design working: a dead creature does not come back because
    // its cell will never be asked for that kind again, not because anything wrote
    // down that it died.
    void Lose(Vector2 at);

    // Marks every patch outside the sleeping edge as no longer awake, and reports
    // which ones they were, so the caller can rest their creatures into them.
    //
    // The order matters and is the caller's to get right: rest the creatures first,
    // then close the patches. A patch closed while its creatures are still in the
    // herd is a patch that will wake an empty version of itself.
    bool Sleeping(Rectangle active, Vector2 at) const;

    void Close(Rectangle active);

    void Clear() {
        patches_.clear();
        awake_.clear();

        asked_   = 0;
        tried_   = 0;
        suited_  = 0;
        resting_ = 0;
        rolled_  = 0;
        lost_    = 0;
    }

    // Every creature resting inside a region, for `--mobcheck` and nothing else.
    //
    // A census has to be able to see the ones nobody is looking at, or the only thing
    // it can check is that the view still works — which was never in doubt. It is the
    // sleeping half only; the herd adds the live ones.
    void Census(Rectangle where, std::vector<Life> &out) const;

    // How many creatures have ever been rolled into the cells a region touches.
    //
    // The precise question `--mobcheck` has to ask about the dead, and a census
    // cannot answer it: a creature standing where a dead one was might have been
    // rolled afresh — the fault — or might have walked in from next door, which is
    // an animal doing what animals do. This counts rolls, so the two are told apart.
    int RolledIn(Rectangle where) const;

    int Remembered() const { return static_cast<int>(patches_.size()); }
    int Awake() const { return static_cast<int>(awake_.size()); }

    // Running totals rather than sums over the map.
    //
    // The map grows with the ground explored and never shrinks, so anything that
    // walks it is a cost that gets worse the longer a session lasts — which is the
    // worst shape a cost can have, because it is invisible in every short test.
    // What settling actually did, for `--mobcheck` and nothing else.
    //
    // A world that comes out with no animals in it has failed somewhere between "this
    // cell holds boars" and "here is one", and the counts say where: no cells asked
    // means the chance gate refused everything, spots tried with none suited means the
    // ground is wrong, suited with none placed means the patch box rejected them.
    // Without them the only thing to do is guess, which cost an hour.
    int Asked() const { return asked_; }
    int Tried() const { return tried_; }
    int Suited() const { return suited_; }

    int Resting() const { return resting_; }
    int Rolled() const { return rolled_; }
    int Lost() const { return lost_; }

private:
    static std::int64_t Key(int cx, int cy);

    static int ColumnOf(float x);
    static int RowOf(float y);

    // Rolls whichever kinds have not been rolled here yet and can be now.
    //
    // The roll is a pure function of `(patch, kind, seed)` — never of the clock, the
    // view or the order patches were visited — so the same world always holds the
    // same animals in the same places. What *is* a function of when you arrived is
    // which kinds have had their conditions met yet, and that is honest: a shade is
    // placed the first time night falls on a cell you are standing in, and there is
    // no other answer that is both consistent and has night creatures in it.
    void Settle(const World &world, Patch &patch, float now);

    std::unordered_map<std::int64_t, Patch> patches_;

    // The keys of the patches currently paged in.
    //
    // Held rather than found by walking the map, and it is the same argument the
    // totals above make: closing patches used to walk every cell ever visited, once a
    // frame, so the cost of standing still grew with how far the player had ever
    // walked. It measured 0.78 ms of frame after a couple of minutes and would only
    // have got worse.
    std::vector<std::int64_t> awake_;

    int asked_   = 0;
    int tried_   = 0;
    int suited_  = 0;
    int resting_ = 0;
    int rolled_  = 0;
    int lost_    = 0;

    int seed_ = 0;
};

} // namespace mob
