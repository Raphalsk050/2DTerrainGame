#pragma once

#include "element.h"
#include "grove.h"
#include "inventory.h"
#include "raylib.h"
#include "stack.h"
#include "world.h"

#include <array>
#include <optional>

// The two hands, and how far they can be reached.
//
// One button each: the left takes the world apart and the right puts something
// of the player's into it. That is the arrangement both Minecraft and Terraria
// settled on, and it replaces a brush with a mode.
//
// The mode was the right answer while the brush painted from an endless palette
// — placing and digging were used in long alternating stretches, and a keystroke
// beat swapping hands mid-stroke. What changed is that placing now spends
// something. Placing and digging stopped being two settings of one tool and
// became two tools, and a tool per button needs no switch, no badge to say which
// is on, and no way to be holding the wrong one.
//
// What the right hand does depends on what is in it: a material goes down as
// blocks under the brush, and a sapling goes into the ground as a tree. An empty
// hand does nothing, which is the whole of the survival rule — a wall has to be
// dug up before it can be built.
class Editor {
public:
    // Reads the mouse and applies whichever hand was used. What it digs up goes
    // into the inventory, and what will not fit goes on the ground.
    //
    // Returns what the world had to say back, or nothing. A refusal that is
    // never spoken is the same to a player as a button that is broken, and the
    // caller is where the notice is already kept.
    const char *Update(World &world, Inventory &inventory, Grove &grove, const Camera2D &camera, Vector2 player,
                       float now);

    // Outline of the area the next click affects, drawn in world space so it
    // sits over the material it is about to change.
    void DrawCursor(const Inventory &inventory, const Camera2D &camera) const;

    float Radius() const { return radius_; }

    // What the cursor is currently over, or nothing where the space is open.
    // Held from the last update so the head-up display and the cursor agree on
    // one answer rather than each asking the world separately.
    std::optional<Element> Under() const { return under_; }

    // Where the cursor is pointing, in world space.
    Vector2 Aim() const { return aim_; }

    // Whether that point is close enough to act on.
    bool Reachable() const { return reachable_; }

private:
    // How far the player can work from, measured centre to cursor.
    //
    // Six blocks. Minecraft gives four and a half and Terraria something near
    // six, and this sits with them — but the tool here is a brush covering an
    // area rather than a pick taking one cube, so the reach bounds where the
    // stroke is *aimed* and the brush still spends its own radius beyond that.
    //
    // Written against kBlockSide rather than as a number of pixels, because what
    // it means is a number of blocks and the pixels are how this world happens to
    // measure one.
    static constexpr float kReach = 6.0f * kBlockSide;

    // Brush sizes in pixels. Bounded at the small end by the lattice, since a
    // brush narrower than the spacing between vertices covers none of them and
    // silently does nothing.
    static constexpr float kMinRadius  = 8.0f;
    static constexpr float kMaxRadius  = 64.0f;
    static constexpr float kRadiusStep = 4.0f;

    // Turns a vertex yield into whole blocks and puts them away, keeping the
    // fraction for the next stroke. Whatever will not fit is thrown on the
    // ground at `at`.
    void Bank(const World::Yield &freed, Inventory &inventory, Drops &drops, Vector2 at, float away, float now);

    // Lays the material in hand under the brush, spending it. Returns what to
    // say, or nothing.
    const char *Lay(World &world, Inventory &inventory, Drops &drops, Vector2 target, float away, float now);

    float radius_ = 16.0f;

    // Vertices of each material held over from earlier strokes, always less than
    // one block's worth.
    //
    // The ground is a field and a slot holds blocks, and one stroke of a brush
    // almost never crosses a whole number of them — see kVerticesPerBlock. Round
    // the remainder away each stroke and a player digging with a small brush is
    // paid nothing at all, however long they dig; keeping it here is what makes
    // the exchange lossless in both directions.
    std::array<float, kElementCount> owed_{};

    std::optional<Element> under_;
    Vector2 aim_{};

    bool reachable_ = false;
};
