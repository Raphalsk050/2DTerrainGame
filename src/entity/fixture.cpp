#include "entity/fixture.h"

#include "save/record.h"

#include "core/config.h"
#include "entity/drop.h"
#include "item/item_def.h"
#include "core/stack.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace fixture {
namespace {

// How large a fixture is drawn, in world units per texel of its picture.
//
// Three, so a six-texel picture comes to eighteen — one cell exactly. A fixture
// stands in one cell and has to look like it stands in one cell, and this is the
// only figure that makes the drawing say the same thing the placement does.
//
// It is also config::kPixelSize, which is not a coincidence worth relying on but
// is worth noting: the flame and the ground behind it are drawn on one grain, so
// a torch does not read as a sprite pasted over a pixel-art world.
constexpr float kTexel = 3.0f;

// Radius of the light a fixture offers, in world pixels.
//
// Well past its own cell, since what a torch is for is the room and not the wall
// it hangs on. Six cells is a hundred and eight, which is the reach — a player
// can light exactly as far as they can build, which is the span they are working
// in anyway.
constexpr float kGlowRadius = 6.0f * config::kBuildCell;

// Where a fixture's art lives, and what the four strips are always called.
//
// Written here once rather than on every row: a table full of file paths is a table
// full of things that can be misspelt, and the misspelling shows up as a fixture drawn
// from its fallback with nothing saying why.
const char *kUnder = "assets/fixtures/";

std::unordered_map<std::string, Wardrobe> &Racks() {
    static std::unordered_map<std::string, Wardrobe> racks;

    return racks;
}

sheet::Strip Wear(const std::string &folder, const char *clip, int wide) {
    const std::string path = std::string(kUnder) + folder + "/" + clip + ".png";

    return sheet::Load(path.c_str(), wide);
}

light::Radiance Glow(const ElementLight &light) {
    constexpr float kByte = 1.0f / 255.0f;

    return {light.glow.r * kByte * light.strength, light.glow.g * kByte * light.strength,
            light.glow.b * kByte * light.strength};
}

} // namespace

const sheet::Strip &Wardrobe::For(Piece piece) const {
    switch (piece) {
    case Piece::Left: return left.Ready() ? left : alone;
    case Piece::Middle: return middle.Ready() ? middle : alone;
    case Piece::Right: return right.Ready() ? right : alone;
    case Piece::Alone: break;
    }

    return alone;
}

const Wardrobe &Dressed(const Def &def) {
    static const Wardrobe bare;

    if (def.art == nullptr) return bare;

    Wardrobe &rack = Racks()[def.art];

    if (rack.tried) return rack;

    rack.tried = true;

    rack.alone  = Wear(def.art, "alone", def.artWide);
    rack.left   = Wear(def.art, "left", def.artWide);
    rack.middle = Wear(def.art, "middle", def.artWide);
    rack.right  = Wear(def.art, "right", def.artWide);

    if (!rack.Any()) {
        // Said once, and only once, because `tried` is already set.
        //
        // A warning rather than a refusal: a fixture with no art still has the
        // hand-drawn `picture` on its row and is perfectly playable from it. What must
        // not happen is silence — art that quietly did not load is a thing that looks
        // wrong for a reason nobody can see.
        TraceLog(LOG_WARNING, "fixture '%s': no art found under %s%s/", def.name, kUnder, def.art);
    }

    return rack;
}

void Undress() {
    for (auto &[folder, rack] : Racks()) {
        sheet::Unload(rack.alone);
        sheet::Unload(rack.left);
        sheet::Unload(rack.middle);
        sheet::Unload(rack.right);

        rack.tried = false;
    }

    Racks().clear();
}

Piece Joined::PieceAt(int index) const {
    if (count <= 1) return Piece::Alone;
    if (index <= 0) return Piece::Left;
    if (index >= count - 1) return Piece::Right;

    return Piece::Middle;
}

std::int64_t Fixtures::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

const Placed *Fixtures::Find(int cx, int cy) const {
    const auto found = placed_.find(Key(cx, cy));

    return (found == placed_.end()) ? nullptr : &found->second;
}

Placed *Fixtures::Find(int cx, int cy) {
    const auto found = placed_.find(Key(cx, cy));

    return (found == placed_.end()) ? nullptr : &found->second;
}

int Fixtures::Reach(Kind kind, int cx, int cy, int step) const {
    int run = 0;

    // Bounded by the row's own join, so a wall of chests laid end to end costs the
    // length of one bank to walk and not the length of the wall.
    for (int i = 1; i <= Of(kind).joins; i++) {
        const Placed *at = Find(cx + i * step, cy);

        if (at == nullptr || at->kind != kind) break;

        run++;
    }

    return run;
}

bool Fixtures::Joins(Kind kind, int cx, int cy) const {
    const Def &def = Of(kind);

    if (def.joins <= 1) return true;

    const int run = Reach(kind, cx, cy, -1) + 1 + Reach(kind, cx, cy, +1);

    return run <= def.joins;
}

bool Fixtures::Place(Kind kind, int cx, int cy) {
    if (Find(cx, cy) != nullptr) return false;

    // Refused rather than placed and left out of the bank, which is the other reading
    // and is the worse one. A chest that stood beside a full run without joining it
    // would make "which three of these four are one store" a question whose answer is
    // the order they happened to be built in — and there is nowhere honest to write
    // that down. A run that cannot grow says so, and the player leaves a gap.
    if (!Joins(kind, cx, cy)) return false;

    const Def &def = Of(kind);

    Placed one{.kind = kind, .cx = cx, .cy = cy};

    if (def.Remembers()) {
        one.kept.resize(static_cast<std::size_t>(def.slots));
        one.rows.resize(static_cast<std::size_t>(def.Rows()));
    }

    placed_.emplace(Key(cx, cy), std::move(one));

    return true;
}

bool Fixtures::Remove(int cx, int cy, Placed &what) {
    const auto found = placed_.find(Key(cx, cy));
    if (found == placed_.end()) return false;

    what = std::move(found->second);
    placed_.erase(found);

    // A store that has just been dug out cannot be the one standing open. Cleared here
    // rather than left to the caller because there is one place a fixture goes away and
    // this is it, and a bank pointing at a chest that is gone is a panel drawing from
    // freed memory.
    if (open_.Any() && cy == open_.cy && cx >= open_.cx && cx < open_.cx + open_.count) open_ = {};

    return true;
}

std::optional<Kind> Fixtures::At(int cx, int cy) const {
    const Placed *one = Find(cx, cy);

    if (one == nullptr) return std::nullopt;

    return one->kind;
}

Joined Fixtures::Run(int cx, int cy) const {
    const Placed *one = Find(cx, cy);

    if (one == nullptr || !Of(one->kind).Remembers()) return {};

    const int left  = Reach(one->kind, cx, cy, -1);
    const int right = Reach(one->kind, cx, cy, +1);

    return {.cx = cx - left, .cy = cy, .count = left + 1 + right};
}

slots::Bank Fixtures::Store(const Joined &run) {
    slots::Bank bank;

    for (int i = 0; i < run.count; i++) {
        Placed *one = Find(run.cx + i, run.cy);

        if (one == nullptr || one->kept.empty()) continue;

        bank.Join(one->kept.data(), static_cast<int>(one->kept.size()));
    }

    return bank;
}

int Fixtures::Rows(const Joined &run) const {
    int rows = 0;

    for (int i = 0; i < run.count; i++) {
        const Placed *one = Find(run.cx + i, run.cy);

        if (one != nullptr) rows += static_cast<int>(one->rows.size());
    }

    return rows;
}

void Fixtures::Rules(const Joined &run, std::vector<slots::Kind *> &out) {
    out.clear();

    for (int i = 0; i < run.count; i++) {
        Placed *one = Find(run.cx + i, run.cy);
        if (one == nullptr) continue;

        // In bank order, unit by unit, which is the same order `Store` lays the slots
        // out in — so row `r` of the grid and rule `r` are the same row, and the rule
        // sits in the very chest whose slots that row is drawn from. Dig up the middle
        // chest of three and the other two keep their rules, still about the rows they
        // were always about.
        for (slots::Kind &rule : one->rows) out.push_back(&rule);
    }
}

bool Fixtures::Holds(const World &world, Kind kind, int cx, int cy) {
    const unsigned char anchors = Of(kind).anchors;

    // Its own cell has to be clear of ground — a torch is not driven into rock —
    // and something around it has to be solid.
    if (world.OverlapsSolid(World::CellBounds(cx, cy))) return false;

    if ((anchors & kBehind) != 0 && world.WalledAt(cx, cy)) return true;
    if ((anchors & kFloor) != 0 && world.OverlapsSolid(World::CellBounds(cx, cy + 1))) return true;
    if ((anchors & kRoof) != 0 && world.OverlapsSolid(World::CellBounds(cx, cy - 1))) return true;

    if ((anchors & kWall) != 0) {
        if (world.OverlapsSolid(World::CellBounds(cx - 1, cy))) return true;
        if (world.OverlapsSolid(World::CellBounds(cx + 1, cy))) return true;
    }

    return false;
}

void Fixtures::Animate(float dt) {
    if (dt <= 0.0f) return;

    for (auto &[key, one] : placed_) {
        const Def &def = Of(one.kind);

        if (!def.Remembers()) continue;

        const bool showing = open_.Any() && one.cy == open_.cy && one.cx >= open_.cx
                             && one.cx < open_.cx + open_.count;

        const float step = (def.opens > 0.0f) ? dt / def.opens : 1.0f;

        one.lid = std::clamp(one.lid + (showing ? step : -step), 0.0f, 1.0f);
    }
}

void Fixtures::Draw(Rectangle view) const {
    for (const auto &[key, one] : placed_) {
        const Rectangle at = World::CellBounds(one.cx, one.cy);

        if (!CheckCollisionRecs(at, view)) continue;

        const Def &def       = Of(one.kind);
        const Wardrobe &worn = Dressed(def);

        // Which piece of its bank this is, so a run of three is drawn as one chest with
        // two ends rather than as three chests standing in a row. Worked out before the
        // art is asked for, because the hand-drawn fallback needs it just as much —
        // that was the whole complaint: joined chests that did not look joined.
        const Joined run  = Run(one.cx, one.cy);
        const Piece piece = run.Any() ? run.PieceAt(one.cx - run.cx) : Piece::Alone;

        if (!worn.Any()) {
            DrawPicture(FaceOf(def, piece), {at.x, at.y}, kTexel);
            continue;
        }

        const sheet::Strip &strip = worn.For(piece);
        if (!strip.Ready()) continue;

        // The lid, as a frame of the strip. Nought is shut and the last is wide open, so
        // a chest that never opens is drawn from its first frame and an artist who ships
        // one frame gets a chest that works.
        const int frame = std::min(static_cast<int>(one.lid * static_cast<float>(strip.frames)), strip.frames - 1);

        // Bottom-centre of the cell, and `facing` is always forward. A fixture has no
        // side: mirroring one would put the left end of a bank on the right of it, and
        // there is nothing a mirrored chest could mean.
        sheet::Draw(strip, frame, {at.x + at.width * 0.5f, at.y + at.height}, 1.0f, +1, WHITE);
    }
}

void Fixtures::Illuminate(World &world, Rectangle view) const {
    for (const auto &[key, one] : placed_) {
        const Def &def = Of(one.kind);
        if (def.light.strength <= 0.0f) continue;

        const Rectangle at = World::CellBounds(one.cx, one.cy);

        // Only what the view can see. A light outside the solved region is a light
        // nothing will read, and offering it costs the solve a probe either way.
        if (!CheckCollisionRecs(at, view)) continue;

        world.AddLight({at.x + at.width * 0.5f, at.y + at.height * 0.5f}, Glow(def.light), kGlowRadius);
    }
}

void Fixtures::Save(save::Writer &out) const {
    std::vector<const Placed *> all;

    all.reserve(placed_.size());

    for (const auto &[key, one] : placed_) all.push_back(&one);

    // Sorted, for `World::Save`'s reason. Left to right within a row, which also puts
    // the units of a bank next to each other in the file the way they stand in the
    // world.
    std::sort(all.begin(), all.end(), [](const Placed *a, const Placed *b) {
        if (a->cy != b->cy) return a->cy < b->cy;

        return a->cx < b->cx;
    });

    out.Tag("fixtures").Done();

    for (const Placed *one : all) {
        // By name, never by `Kind`. The enum's numbering is source order and a row
        // inserted above another moves every number after it, which would turn a saved
        // chest into a torch and lose what was in it (§19.4).
        out.Tag("fixture").Text(Of(one->kind).name).Int(one->cx).Int(one->cy).Done();

        // Only the slots that hold something. A chest is thirty-two slots and mostly
        // empty, and writing the empties would be nine tenths of the file saying
        // nothing.
        for (std::size_t slot = 0; slot < one->kept.size(); slot++) {
            if (one->kept[slot].Empty()) continue;

            out.Tag("keep").Int(static_cast<long long>(slot));

            save::PutStack(out, one->kept[slot]);

            out.Done();
        }

        for (std::size_t row = 0; row < one->rows.size(); row++) {
            const slots::Kind &rule = one->rows[row];
            if (!rule.Any()) continue;

            // A rule is a kind with no count, and it is written as the stack it stands
            // for so that one reader answers for both.
            out.Tag("rule").Int(static_cast<long long>(row));

            save::PutStack(out, {.holds = rule.holds, .what = rule.what, .count = 1});

            out.Done();
        }
    }
}

void Fixtures::Load(save::Reader &in) {
    Placed *into = nullptr;

    while (in.Next()) {
        if (in.Is("fixture")) {
            const std::string name = in.Text();

            const int cx = static_cast<int>(in.Int());
            const int cy = static_cast<int>(in.Int());

            if (!in.Ok()) return;

            std::optional<Kind> kind;

            for (std::size_t k = 0; k < kKindCount; k++) {
                if (name == kKinds[k].name) kind = static_cast<Kind>(k);
            }

            if (!kind.has_value()) {
                in.Fail();
                return;
            }

            const Def &def = Of(*kind);

            Placed one{.kind = *kind, .cx = cx, .cy = cy};

            if (def.Remembers()) {
                one.kept.resize(static_cast<std::size_t>(def.slots));
                one.rows.resize(static_cast<std::size_t>(def.Rows()));
            }

            // Straight into the map rather than through `Place`, and that is
            // deliberate: `Place` refuses a run longer than the row allows, and a save
            // is a world that was already legal when it was written. Replaying it
            // through the rule that *makes* worlds legal would silently drop the third
            // chest of every bank, which is a store the player cannot get back.
            into = &placed_.insert_or_assign(Key(cx, cy), std::move(one)).first->second;

            continue;
        }

        if (in.Is("keep")) {
            const std::size_t slot = static_cast<std::size_t>(in.Int());
            const Stack stack      = save::GetStack(in);

            if (!in.Ok()) return;

            if (into != nullptr && slot < into->kept.size()) into->kept[slot] = stack;

            continue;
        }

        if (in.Is("rule")) {
            const std::size_t row = static_cast<std::size_t>(in.Int());
            const Stack stack     = save::GetStack(in);

            if (!in.Ok()) return;

            if (into != nullptr && row < into->rows.size()) into->rows[row] = slots::Kind::Of(stack);

            continue;
        }

        in.Again();
        break;
    }
}

void Fixtures::Undermine(const World &world, Drops &drops, float now) {
    falling_.clear();

    for (const auto &[key, one] : placed_) {
        if (Holds(world, one.kind, one.cx, one.cy)) continue;

        falling_.push_back(key);
    }

    for (const std::int64_t key : falling_) {
        const auto found = placed_.find(key);
        if (found == placed_.end()) continue;

        const Placed one    = std::move(found->second);
        const Rectangle at  = World::CellBounds(one.cx, one.cy);
        const Vector2 where = {at.x + at.width * 0.5f, at.y + at.height * 0.5f};

        placed_.erase(found);

        if (open_.Any() && one.cy == open_.cy && one.cx >= open_.cx && one.cx < open_.cx + open_.count) open_ = {};

        // Onto the ground rather than into the bag. The player may be nowhere near
        // — a wall dug out from under a torch is often a wall dug from the other
        // side — and a pickup waiting where the thing fell is what every other
        // loss in this world does.
        //
        // The item is the row's own rather than a name written into this call, which is
        // the fault this used to have: `items::Torch()` was correct for exactly as long
        // as there was one fixture, and a chest coming down would have paid out a torch.
        const std::optional<Item> what = ItemOf(one.kind);

        if (what.has_value()) drops.Scatter(ItemsOf(*what, 1), where, 1.0f, now);

        // And whatever was in it, which is rule six of the chest: this unit's contents
        // and only this unit's. Its neighbours are separate records and are untouched.
        for (const Stack &stack : one.kept) {
            if (!stack.Empty()) drops.Scatter(stack, where, 1.0f, now);
        }
    }
}

} // namespace fixture
