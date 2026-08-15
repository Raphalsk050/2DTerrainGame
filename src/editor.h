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
    // What the left button is doing this press.
    //
    // One button for both jobs, and the mode is worked out from what the cursor is
    // over rather than chosen by the player: pointing at a trunk chops it, pointing
    // at ground digs it. That is the arrangement every game with one hand and two
    // jobs uses, and it is worth saying why it needs a *latch* rather than a test
    // per frame — a stroke wanders. Digging out from under a tree, the cursor
    // crosses the trunk halfway through and the hand would turn into an axe in the
    // middle of the hole; a player laying into a trunk drifts off it as the trunk
    // narrows and would start cutting the hillside behind. So the question is asked
    // once, on the press, and the answer is held until the button comes up.
    enum class Hand { Idle, Dig, Chop };

    // Reads the mouse and applies whichever hand was used. What it digs up goes
    // into the inventory, and what will not fit goes on the ground.
    //
    // `body` is the character, and it does two jobs: the reach is measured from
    // its middle, and nothing solid is laid inside it. The whole rectangle rather
    // than the point, because where the reach starts and where the character
    // stands are the same fact asked twice, and a hand holding only the point
    // cannot answer the second.
    //
    // Returns what the world had to say back, or nothing. A refusal that is
    // never spoken is the same to a player as a button that is broken, and the
    // caller is where the notice is already kept.
    const char *Update(World &world, Inventory &inventory, Grove &grove, const Camera2D &camera, Rectangle body,
                       float now);

    // Outline of the area the next click affects, drawn in world space so it
    // sits over the material it is about to change.
    //
    // And, where the hand holds something that goes into the world whole rather
    // than by the fistful, a ghost of the thing itself standing where it would
    // stand. The grove draws that: what a sapling looks like is the wood's to
    // know, and the ghost has to be the same sprite the real one will be or it is
    // a promise about a different tree.
    void DrawCursor(const Inventory &inventory, const Grove &grove, flora::Season season,
                    const Camera2D &camera) const;

    float Radius() const { return radius_; }

    // What the cursor is currently over, or nothing where the space is open.
    // Held from the last update so the head-up display and the cursor agree on
    // one answer rather than each asking the world separately.
    std::optional<Element> Under() const { return under_; }

    // Where the cursor is pointing, in world space.
    Vector2 Aim() const { return aim_; }

    // Whether that point is close enough to act on.
    bool Reachable() const { return reachable_; }

    // Which tool the left button became when it went down, and still is.
    //
    // The caller swings the character's arm on this and lands the blow itself,
    // because what an axe hits is the wood's business and this module knows only
    // the ground. It reports the decision; it does not act on it.
    Hand Left() const { return left_; }

    // Where a thing put down now would come to rest, or nothing where there is no
    // ground under the cursor within reach of it.
    //
    // Worked out once a frame and read by both hands that need it — the ghost that
    // shows the player where it will go, and the click that puts it there. One
    // answer, because two would be a preview that lies.
    std::optional<Vector2> Footing() const { return footing_; }

    // Whether the ground there will take the seed in hand.
    //
    // Whatever the species itself says it roots in — see flora::SpeciesGround —
    // read against the world as built rather than as generated, so a bed of soil
    // the player carried into a desert is ground a tree will take. That is the
    // whole of the restriction, and it is the same one Minecraft puts on a
    // sapling: everything else about where a tree may go is the player's to
    // decide, and a seed in a hand is one particular tree wherever it is carried.
    bool Rooted() const { return rooted_; }

private:
    // How far below the cursor the ground is looked for, in world pixels.
    //
    // Half a screen. What this bounds is a click in open sky: a player pointing at
    // a cloud is not asking to plant a tree on the hillside a long way beneath it,
    // and past this the honest answer is that there is nothing to put it on.
    static constexpr float kDropReach = 320.0f;

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

    // Half-side of the square the cursor asks the wood about, in world pixels.
    //
    // A point would be honest and unusable: a trunk is a few pixels wide on screen
    // and a player aiming at one with a point-sized cursor would miss it more often
    // than not. This is about a block, which is the slack a hand deserves — and it
    // is only used to *choose* the tool, so being generous costs a click aimed at
    // the ground beside a trunk becoming a chop, and never a mis-swing: what the
    // blow actually hits is settled by Grove::Strike against the same rectangles.
    static constexpr float kAimSlack = 10.0f;

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
    // say, or nothing. `body` is left out of the stroke — see World::Place.
    const char *Lay(World &world, Inventory &inventory, Drops &drops, Vector2 target, Rectangle body, float away,
                    float now);

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

    Hand left_ = Hand::Idle;

    // Whether the cursor is over something the axe would bite. Held from the last
    // update so the cursor and the click agree about which tool this is.
    bool timber_ = false;

    std::optional<Element> under_;
    Vector2 aim_{};

    // The ground under the cursor, where the hand holds something that would be
    // stood on it. Empty otherwise, so nothing is worked out for a hand that has
    // no use for it.
    std::optional<Vector2> footing_;
    bool rooted_ = false;

    bool reachable_ = false;
};
