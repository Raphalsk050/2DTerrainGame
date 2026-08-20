#include "entity/mob/warren.h"

#include "save/record.h"

#include "entity/mob/brains/whim.h"
#include "entity/mob/suits.h"
#include "world/terrain.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace {

// How many spots inside a patch are tried before a kind gives up on it for now.
//
// A patch is 512 by 384, so this is a probe every twenty pixels or so across it.
// Generous, because settling a kind into a cell happens once for the life of the
// world — the cost is paid on first entry and never again, and being stingy here
// buys nothing and loses animals.
constexpr int kTries = 24;

// How long before a cell that could not place a kind is asked about it again, in
// seconds of the weather clock.
//
// It has to be asked again: a shade cannot be placed in a meadow at noon and plainly
// should be at midnight, and the cell has no way of knowing when that changed. It
// must not be asked *every frame*, which is what this is for — twenty patches in
// view times twenty-four probes times a world lookup each is a millisecond spent
// finding out that it is still daytime.
constexpr float kAskAgainIn = 3.0f;

// How far apart a group is scattered inside its patch, in world pixels.
//
// Not a rule about crowding — that is `Haunt::crowd` — but about clumping: a sounder
// is a group, and four boars spread evenly over five hundred pixels is four lone
// boars. The first of a kind lands where it lands and the rest are drawn to it.
constexpr float kHerdTogether = 90.0f;

// How far past the bottom of a band the search for a floor is allowed to reach, in
// world pixels.
//
// A band describes where a creature may *stand*, and the ground under the last
// acceptable spot is a little below it. Without the margin a band that ends exactly at
// the surface finds no floor at all.
constexpr float kUnderfoot = 24.0f;

// And how far above the floor a creature is set down, in world pixels.
//
// A body's anchor is its feet and the ground is a contour rather than a line, so a
// creature placed exactly on the drawn surface is a creature placed a fraction inside
// it. Two pixels is under the lattice step and under any material's texel.
constexpr float kStandOff = 2.0f;

} // namespace

std::int64_t mob::Warren::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

int mob::Warren::ColumnOf(float x) {
    return static_cast<int>(std::floor(x / kSpan));
}

int mob::Warren::RowOf(float y) {
    return static_cast<int>(std::floor(y / kRise));
}

void mob::Warren::Settle(const World &world, Patch &patch, float now) {
    const Rectangle box = {static_cast<float>(patch.cx) * kSpan, static_cast<float>(patch.cy) * kRise, kSpan, kRise};

    const int rows = kinds::Count();

    bool tried = false;

    for (int r = 0; r < rows; r++) {
        const std::uint32_t bit = 1u << r;

        // Already rolled here. Never rolled twice, which is the whole of "the dead
        // stay dead" — see `patch.h`.
        if ((patch.settled & bit) != 0u) continue;

        const Kind kind = Kind{r};
        const Def &def  = kinds::Of(kind);

        if (def.haunt.chance <= 0.0f) continue;

        // A stream of this cell's own, mixed from the cell, the kind and the world
        // seed. Not the herd's running stream: the promise is that a cell holds the
        // same animals in every session of a world, and a running stream depends on
        // everything that happened to be rolled before it.
        // **Both halves of the key**, folded together.
        //
        // It was the low thirty-two bits alone, and the key packs the column into the
        // high half — so every cell in a row shared one stream, made one chance roll,
        // and answered identically. One unlucky roll therefore emptied *every cell at
        // that height in the world*, which is what "no creatures anywhere in twenty
        // thousand pixels" was: not a placement fault, not the light, not the ground,
        // but a hash that had been handed half its input.
        //
        // Found by `--mobcheck`, and only because it reported the counts rather than a
        // verdict: 279 cells asked, 6696 spots tried, none suited — with the very spot
        // it was standing on passing `Suits` when asked directly. Nothing else in the
        // report could have told a bad hash from bad ground.
        const std::uint64_t key = static_cast<std::uint64_t>(Key(patch.cx, patch.cy));

        std::uint32_t stream = Spread(static_cast<std::uint32_t>(seed_) ^ static_cast<std::uint32_t>(r * 7919),
                                      static_cast<std::uint32_t>(key ^ (key >> 32)));

        Roll(stream);

        // Whether this cell holds any of them at all. A pure function of the cell,
        // the kind and the seed — so it is the same answer every time it is asked,
        // and asking again after a refusal costs one hash and no world lookups.
        if (Chance(stream) >= def.haunt.chance) {
            // Marked settled even though nothing was placed. The answer is fixed and
            // asking again would only cost the hash — but a cell that says no to
            // boars says no for good, and recording that is what keeps the retry
            // below from walking the whole table every three seconds for ever.
            patch.settled |= bit;

            continue;
        }

        // Past here every attempt reads the world, so a kind that cannot be placed
        // *yet* has to be rate limited. See `kAskAgainIn`.
        if (now < patch.askAgainAt) continue;

        tried = true;

        asked_++;

        const int wanted = Count(stream, def.haunt.least, def.haunt.most);

        Vector2 first{};

        int placed = 0;

        for (int t = 0; t < kTries && placed < wanted; t++) {
            tried_++;

            // Drawn towards the first of its kind, so a group is a group. The first
            // is drawn from the whole patch.
            const float x = (placed == 0)
                              ? Between(stream, box.x, box.x + box.width)
                              : std::clamp(first.x + Between(stream, -kHerdTogether, kHerdTogether), box.x,
                                           box.x + box.width);

            const float surface = terrain::Height(x, world.Settings());

            // The floor inside the row's own band, **found** rather than guessed at.
            //
            // Sampling a height in the band and hoping it landed on the ground is the
            // §21.2b mistake in its second form, and it was worse than the first: a
            // boar's band is 56 px of mostly open air, `Suits` demands something solid
            // within two pixels under its feet, and so about one try in twenty could
            // ever succeed. What that produced was three hundred and fifty cells
            // visited and *nothing rolled anywhere* — a county with no animals in it,
            // no error, and `--mobs` cheerfully reporting the ground as suitable,
            // because the ground was suitable and nothing had asked it.
            //
            // Walking down from the top of the band finds the floor by construction,
            // so every try lands somewhere a creature could actually stand and the
            // refusals are left to be about light, climate and headroom.
            float y = 0.0f;

            if (def.build.floats) {
                // Nothing that flies wants a floor, and looking for one is how a cave
                // full of headroom comes out empty.
                y = surface + Between(stream, def.haunt.fromDepth, def.haunt.toDepth);
            } else if (def.haunt.fromDepth < 0.0f) {
                // A band that reaches above the ground is a creature of the surface, and
                // the surface is what it stands on. `World::SurfaceOf` is the question
                // for that — the first solid thing looking down from the open sky, and
                // the one that accounts for what has been built.
                if (!world.SurfaceOf(x, y)) continue;

                y -= kStandOff;
            } else {
                // A band wholly under the ground is a creature of the caves, and there
                // the floor has to be walked down to.
                const float band = std::max(8.0f, def.haunt.toDepth - def.haunt.fromDepth);

                if (!world.FootingUnder({x, surface + def.haunt.fromDepth}, band + kUnderfoot, y)) continue;

                y -= kStandOff;
            }

            // And it still has to be in this patch, or a cell would settle its
            // neighbours' creatures for them and both would hold the same animal.
            if (y < box.y || y > box.y + box.height) continue;

            if (!Suits(world, def, {x, y})) continue;

            suited_++;

            if (placed == 0) first = {x, y};

            Life life;

            life.kind      = kind;
            life.at        = {x, y};
            life.health    = def.hardy;
            life.wits.seed = Spread(stream, static_cast<std::uint32_t>(placed * 131 + 17));

            patch.asleep.push_back(life);

            patch.rolled++;
            rolled_++;
            resting_++;

            placed++;
        }

        // Only counted as settled where the ground actually took one.
        //
        // A cell that suits the kind and simply came up empty on every probe is a
        // cell that has to be asked again — otherwise one unlucky run of twenty-four
        // probes writes a kind out of a place for the life of the world, which is a
        // fault nothing on screen could ever explain.
        if (placed > 0) patch.settled |= bit;
    }

    if (tried) patch.askAgainAt = now + kAskAgainIn;
}

void mob::Warren::Wake(const World &world, Rectangle active, float now, std::vector<Life> &out) {
    const Rectangle waking = {active.x - kWakeOut, active.y - kWakeOut, active.width + kWakeOut * 2.0f,
                              active.height + kWakeOut * 2.0f};

    const int fromX = ColumnOf(waking.x);
    const int toX   = ColumnOf(waking.x + waking.width);
    const int fromY = RowOf(waking.y);
    const int toY   = RowOf(waking.y + waking.height);

    for (int cx = fromX; cx <= toX; cx++) {
        for (int cy = fromY; cy <= toY; cy++) {
            Patch &patch = patches_[Key(cx, cy)];

            patch.cx = cx;
            patch.cy = cy;

            // Settled every time the cell is in view rather than only the first time,
            // and the `settled` bits are what make that cheap: a cell that has already
            // answered for every kind costs one mask test each. What it buys is the
            // kind whose conditions were not met before — a shade the first night the
            // player spends standing there.
            Settle(world, patch, now);

            if (patch.awake) continue;

            patch.awake = true;

            awake_.push_back(Key(cx, cy));

            for (const Life &life : patch.asleep) out.push_back(life);

            resting_ -= static_cast<int>(patch.asleep.size());

            // Handed over rather than copied. While a patch is awake its creatures
            // belong to the herd, and a second copy here would be two answers to
            // where a boar is.
            patch.asleep.clear();
        }
    }
}

bool mob::Warren::Sleeping(Rectangle active, Vector2 at) const {
    const Rectangle keep = {active.x - kSleepOut, active.y - kSleepOut, active.width + kSleepOut * 2.0f,
                            active.height + kSleepOut * 2.0f};

    return !CheckCollisionPointRec(at, keep);
}

namespace {

// One creature, written down. Shared by the two places a `Life` reaches the file —
// the ones already resting and the ones the herd is still walking about — because
// they are the same record and a second copy of these ten fields is a second thing to
// forget a field on.
void PutLife(save::Writer &out, const mob::Life &life) {
    out.Tag("rest")
        .Text(mob::kinds::Of(life.kind).name)
        .Real(life.at.x)
        .Real(life.at.y)
        .Int(life.health)
        .Int(life.wits.mood)
        .Real(life.wits.holds)
        .Real(life.wits.since)
        .Real(life.wits.lean)
        .Real(life.wits.rested)
        .Int(life.wits.seed)
        .Done();

    // `wits.legs` is deliberately not written. It is a leap already in the air, which
    // is a fraction of a second of state — and a creature that was mid-jump when the
    // game was saved is a creature that was, from the world's point of view, about to
    // be asleep anyway. The navigator re-plans on the first frame and nothing can see
    // the difference.
}

} // namespace

void mob::Warren::Save(save::Writer &out, const std::vector<Life> &living) const {
    // One entry per cell that has anything to say, which is every cell with a record
    // *and* every cell a live creature is standing in.
    //
    // The second half is not an edge case, and leaving it out was a real bug the round
    // trip caught: a creature put down by hand — the console's `/spawn`, or a probe —
    // is in the herd immediately and its cell has no record until the view leaves it.
    // Written against the patches alone, that creature is filed under a patch that does
    // not exist yet and is simply dropped.
    //
    // Merged into one sorted list rather than written in two passes, because the second
    // save of the same world *would* have those cells as records — so two passes would
    // put them in a different order the second time round and the round trip would
    // disagree with itself.
    struct Cell {
        int cx = 0;
        int cy = 0;

        const Patch *patch = nullptr;
    };

    std::vector<Cell> cells;

    cells.reserve(patches_.size() + living.size());

    for (const auto &[key, patch] : patches_) cells.push_back({.cx = patch.cx, .cy = patch.cy, .patch = &patch});

    for (const Life &life : living) {
        const int cx = ColumnOf(life.at.x);
        const int cy = RowOf(life.at.y);

        if (patches_.find(Key(cx, cy)) != patches_.end()) continue;

        bool already = false;

        for (const Cell &cell : cells) already = already || (cell.cx == cx && cell.cy == cy);

        if (!already) cells.push_back({.cx = cx, .cy = cy, .patch = nullptr});
    }

    // Sorted, for `World::Save`'s reason.
    std::sort(cells.begin(), cells.end(), [](const Cell &a, const Cell &b) {
        if (a.cy != b.cy) return a.cy < b.cy;

        return a.cx < b.cx;
    });

    out.Tag("patches").Done();

    for (const Cell &cell : cells) {
        out.Tag("patch")
            .Int(cell.cx)
            .Int(cell.cy)
            .Real((cell.patch != nullptr) ? cell.patch->askAgainAt : 0.0f)
            .Int((cell.patch != nullptr) ? cell.patch->rolled : 0)
            .Int((cell.patch != nullptr) ? cell.patch->lost : 0)
            .Done();

        if (cell.patch != nullptr) {
            // The settled bits, by name. A bitmask written as a number would be read by
            // the next build as a different set of creatures the moment a row is added,
            // which is a meadow that suddenly holds animals it has already been hunted
            // clear of — §21.2's whole design, undone by one integer.
            for (int k = 0; k < kinds::Count(); k++) {
                if ((cell.patch->settled & (1u << k)) == 0) continue;

                out.Tag("settled").Text(kinds::Of(Kind{k}).name).Done();
            }

            for (const Life &life : cell.patch->asleep) PutLife(out, life);
        }

        // And whatever the herd is still walking about in this cell.
        for (const Life &life : living) {
            if (ColumnOf(life.at.x) != cell.cx || RowOf(life.at.y) != cell.cy) continue;

            PutLife(out, life);
        }
    }
}

void mob::Warren::Load(save::Reader &in) {
    Patch *into = nullptr;

    while (in.Next()) {
        if (in.Is("patch")) {
            const int cx = static_cast<int>(in.Int());
            const int cy = static_cast<int>(in.Int());

            Patch patch;

            patch.cx         = cx;
            patch.cy         = cy;
            patch.askAgainAt = in.Real();
            patch.rolled     = static_cast<int>(in.Int());
            patch.lost       = static_cast<int>(in.Int());

            // Asleep, always. See the head of `Save`.
            patch.awake = false;

            if (!in.Ok()) return;

            into = &patches_.insert_or_assign(Key(cx, cy), std::move(patch)).first->second;

            rolled_ += into->rolled;
            lost_ += into->lost;

            continue;
        }

        if (in.Is("settled")) {
            const std::string name = in.Text();

            if (!in.Ok()) return;

            const std::optional<Kind> kind = kinds::Find(name.c_str());

            // A creature this build has never heard of. The bit is dropped rather than
            // the save refused, and it is the one place in the whole format that a
            // missing name is survivable: what the bit says is "this cell has already
            // been asked about wolves", and a build with no wolves in it will never ask.
            // Refusing here would make removing a creature row (§21.1 did exactly that
            // to three of them) break every save ever written.
            if (kind.has_value() && into != nullptr) into->settled |= 1u << kind->index;

            continue;
        }

        if (in.Is("rest")) {
            const std::string name = in.Text();

            Life life;

            life.at.x        = in.Real();
            life.at.y        = in.Real();
            life.health      = static_cast<int>(in.Int());
            life.wits.mood   = static_cast<std::uint8_t>(in.Int());
            life.wits.holds  = in.Real();
            life.wits.since  = in.Real();
            life.wits.lean   = in.Real();
            life.wits.rested = in.Real();
            life.wits.seed   = static_cast<std::uint32_t>(in.Int());

            if (!in.Ok()) return;

            const std::optional<Kind> kind = kinds::Find(name.c_str());

            // Likewise dropped rather than refused, and for the same reason one step
            // on: a save holding a creature this build no longer has is a save whose
            // world is still entirely playable without it.
            if (!kind.has_value() || into == nullptr) continue;

            life.kind = *kind;

            into->asleep.push_back(life);
            resting_++;

            continue;
        }

        in.Again();
        break;
    }
}

void mob::Warren::Rest(const Life &life) {
    const int cx = ColumnOf(life.at.x);
    const int cy = RowOf(life.at.y);

    Patch &patch = patches_[Key(cx, cy)];

    patch.cx = cx;
    patch.cy = cy;

    patch.asleep.push_back(life);

    resting_++;

    // A creature can be rested into a cell nobody has ever entered — it walked into
    // one. Marking it awake would be wrong, and marking it settled would be worse:
    // the cell has not had a population of its own rolled and must still get one when
    // somebody arrives.
    patch.awake = false;
}

void mob::Warren::Lose(Vector2 at) {
    const int cx = ColumnOf(at.x);
    const int cy = RowOf(at.y);

    Patch &patch = patches_[Key(cx, cy)];

    patch.cx = cx;
    patch.cy = cy;
    patch.lost++;

    lost_++;
}

int mob::Warren::RolledIn(Rectangle where) const {
    int count = 0;

    for (const auto &[key, patch] : patches_) {
        const Rectangle box = {static_cast<float>(patch.cx) * kSpan, static_cast<float>(patch.cy) * kRise, kSpan,
                               kRise};

        if (!CheckCollisionRecs(where, box)) continue;

        count += patch.rolled;
    }

    return count;
}

void mob::Warren::Census(Rectangle where, std::vector<Life> &out) const {
    for (const auto &[key, patch] : patches_) {
        for (const Life &life : patch.asleep) {
            if (!CheckCollisionPointRec(life.at, where)) continue;

            out.push_back(life);
        }
    }
}

void mob::Warren::Close(Rectangle active) {
    const Rectangle keep = {active.x - kSleepOut, active.y - kSleepOut, active.width + kSleepOut * 2.0f,
                            active.height + kSleepOut * 2.0f};

    // Only the patches actually paged in, and not every cell the world has ever seen.
    // See `awake_`.
    std::size_t kept = 0;

    for (std::size_t i = 0; i < awake_.size(); i++) {
        const std::int64_t key = awake_[i];

        auto found = patches_.find(key);

        if (found == patches_.end()) continue;

        Patch &patch = found->second;

        // Already closed by `Rest`, which does that when a creature is filed into a
        // cell the view has left.
        if (!patch.awake) continue;

        const Rectangle box = {static_cast<float>(patch.cx) * kSpan, static_cast<float>(patch.cy) * kRise, kSpan,
                               kRise};

        if (!CheckCollisionRecs(keep, box)) {
            patch.awake = false;

            continue;
        }

        awake_[kept++] = key;
    }

    awake_.resize(kept);
}
