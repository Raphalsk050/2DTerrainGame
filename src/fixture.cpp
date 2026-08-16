#include "fixture.h"

#include "config.h"
#include "drop.h"
#include "item.h"
#include "stack.h"
#include "world.h"

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

light::Radiance Glow(const ElementLight &light) {
    constexpr float kByte = 1.0f / 255.0f;

    return {light.glow.r * kByte * light.strength, light.glow.g * kByte * light.strength,
            light.glow.b * kByte * light.strength};
}

} // namespace

std::int64_t Fixtures::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

bool Fixtures::Place(Kind kind, int cx, int cy) {
    const auto [it, fresh] = placed_.try_emplace(Key(cx, cy), Placed{.kind = kind, .cx = cx, .cy = cy});

    return fresh;
}

bool Fixtures::Remove(int cx, int cy, Kind &what) {
    const auto found = placed_.find(Key(cx, cy));
    if (found == placed_.end()) return false;

    what = found->second.kind;
    placed_.erase(found);

    return true;
}

std::optional<Kind> Fixtures::At(int cx, int cy) const {
    const auto found = placed_.find(Key(cx, cy));
    if (found == placed_.end()) return std::nullopt;

    return found->second.kind;
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

void Fixtures::Draw(Rectangle view) const {
    for (const auto &[key, one] : placed_) {
        const Rectangle at = World::CellBounds(one.cx, one.cy);

        if (!CheckCollisionRecs(at, view)) continue;

        DrawPicture(Of(one.kind).picture, {at.x, at.y}, kTexel);
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

void Fixtures::Undermine(const World &world, Drops &drops, float now) {
    falling_.clear();

    for (const auto &[key, one] : placed_) {
        if (Holds(world, one.kind, one.cx, one.cy)) continue;

        falling_.push_back(key);
    }

    for (const std::int64_t key : falling_) {
        const auto found = placed_.find(key);
        if (found == placed_.end()) continue;

        const Placed one    = found->second;
        const Rectangle at  = World::CellBounds(one.cx, one.cy);
        const Vector2 where = {at.x + at.width * 0.5f, at.y + at.height * 0.5f};

        placed_.erase(found);

        // Onto the ground rather than into the bag. The player may be nowhere near
        // — a wall dug out from under a torch is often a wall dug from the other
        // side — and a pickup waiting where the thing fell is what every other
        // loss in this world does.
        drops.Scatter(ItemsOf(Item::Torch, 1), where, 1.0f, now);
    }
}

} // namespace fixture
