#pragma once

#include "world/element.h"
#include "entity/fixture.h"
#include "flora/grove.h"
#include "item/inventory.h"
#include "core/mode.h"
#include "raylib.h"
#include "core/stack.h"
#include "world/world.h"

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
    // `mode` is the whole of what separates the two ways of playing, and it is
    // three lines inside: a block comes away at a touch instead of at its
    // hardness, nothing is charged for what is put down, and nothing is handed
    // back for what is taken up. Everything else about the hand is the same in
    // both, which is the point of passing a mode rather than writing a second
    // editor.
    //
    // `overUi` is "the pointer is on a panel, so this click is not about the world".
    // Handed in rather than worked out here, and that is the fix to a small version
    // of §25.1's fault: this file used to ask `hotbar::Contains` itself, which was
    // correct exactly while the bar was the only thing drawn over the world. It is
    // not any more — see `ui/crafting.h` — and two files each deciding what counts
    // as the interface is two of them disagreeing the first time either moves.
    const char *Update(World &world, Inventory &inventory, Grove &grove, fixture::Fixtures &fixtures,
                       const Camera2D &camera, Rectangle body, Gamemode mode, bool overUi, float now);

    // Outline of the area the next click affects, drawn in world space so it
    // sits over the material it is about to change.
    //
    // And, where the hand holds something that goes into the world whole rather
    // than by the fistful, a ghost of the thing itself standing where it would
    // stand. The grove draws that: what a sapling looks like is the wood's to
    // know, and the ghost has to be the same sprite the real one will be or it is
    // a promise about a different tree.
    //
    // Never called where the pointer is on a panel: `render::Scene`'s `aiming` is
    // the same fact this file used to test for itself.
    void DrawCursor(const Inventory &inventory, const Grove &grove, flora::Season season,
                    const Camera2D &camera) const;

    // How many cells across the hand works, and which cells those are.
    //
    // The brush was a circle of a radius in pixels and is now a square block of
    // whole cells, for the reason everything else here is: the world the hand
    // writes into is a grid now, and a round tool over a square grid can only ever
    // leave the corners of the cells it crosses. That was visible as bites out of
    // a wall long before anyone went looking for a cause.
    int Span() const { return span_; }

    // The block of cells the next action covers, in cell coordinates. Empty where
    // the cursor is not over the world at all.
    bool Block(int &outX0, int &outY0, int &outX1, int &outY1) const;

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

    // Whether the hand holds something that goes into the world a cell at a time.
    //
    // This is the whole of the build mode, and it is *derived* rather than
    // switched. It comes on because a building material is in the slot and goes
    // off because the slot ran out — so there is no key to press, no badge to say
    // which mode this is, and no way to be in the wrong one. The player asked for
    // "put the item down and stay there until the stack is gone"; a stack that is
    // gone is a slot that no longer holds a building material, and the mode ends
    // itself without anybody having to remember to end it.
    //
    // It is the same argument the head of this file makes about the two hands: what
    // the right hand does depends on what is in it.
    bool Building() const { return building_; }

    // Whether a piece put down now would be taken — in reach, and with something
    // to fix it to.
    bool Buildable() const { return buildable_; }

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
    static constexpr float kReach = 8.0f * kBlockSide;

    // Half-side of the square the cursor asks the wood about, in world pixels.
    //
    // A point would be honest and unusable: a trunk is a few pixels wide on screen
    // and a player aiming at one with a point-sized cursor would miss it more often
    // than not. This is about a block, which is the slack a hand deserves — and it
    // is only used to *choose* the tool, so being generous costs a click aimed at
    // the ground beside a trunk becoming a chop, and never a mis-swing: what the
    // blow actually hits is settled by Grove::Strike against the same rectangles.
    static constexpr float kAimSlack = 10.0f;

    // Brush sizes, in whole cells of the build grid.
    //
    // Counted in cells rather than pixels because there is nothing else left for a
    // brush to be measured in: every write the hand makes is a whole cell, so a
    // size between two of them describes nothing. One is a single block, which is
    // the size building wants; eight is a hundred and forty-four pixels across,
    // which is about the widest stroke that still lands where it was aimed.
    static constexpr int kMinSpan = 1;
    static constexpr int kMaxSpan = 8;

    // Turns a vertex yield into whole blocks and throws them on the ground at
    // `at`, keeping the fraction for the next stroke.
    //
    // On the ground, and never into the bag, however much room the bag has. What
    // was dug becomes an object in the world that the player walks over to pick
    // up — Minecraft's rule — and what it buys is that digging *happens
    // somewhere* rather than being a number changing in a corner of the screen. A
    // misfired stroke is also recoverable for the first time: material thrown out
    // of a seam is lying where it fell until somebody collects it.
    //
    // This is not a new path. It is the one a full bag already took, promoted to
    // the only one: `Drops::Update` drifts a settled pickup to a nearby player and
    // collects it, and leaves it lying when there is no room. So a full bag
    // behaves exactly as it did, an empty one now goes through the same code, and
    // there is no longer a branch where the two differ.
    //
    // The fraction still matters even though a placed cell is exactly one block:
    // digging a cell out of a *hillside* frees however many of its nine vertices
    // the ground happened to fill, and the surface crosses cells at every angle.
    void Bank(const World::Yield &freed, Drops &drops, Vector2 at, float away, float now);

    // Fills the block of cells under the cursor with the material in hand, one
    // block of it per cell, and stops when the stack runs out.
    //
    // One routine for laying ground and for building, because they stopped being
    // different things when the brush became square: both write whole cells and
    // both spend one block for each cell they newly fill. What still differs is
    // the rule about where — a plank has to have something to fix to and a shovel
    // of soil does not — and that is asked once, of the cell under the cursor.
    //
    // Nothing is charged where a cell already held the material, which is what
    // lets the button be held down and dragged along a wall without the stack
    // draining while the cursor sits still.
    const char *Spend(World &world, Inventory &inventory, Drops &drops, Rectangle body, Gamemode mode, float away,
                      float now);

    // Whether the cell holds something that was built rather than generated.
    static bool Built(const World &world, int cx, int cy);

    // Whether a cell has anything to fix a piece to.
    //
    // Any of the four cells it shares a side with holding ground a body cannot
    // walk through — the world's own, or a piece already built. Terraria's rule,
    // and it is what makes the grid a set of *valid* positions rather than a
    // quadrille ruled over the open sky: a wall grows out of the ground it stands
    // on, and nothing is left hanging in the air.
    //
    // Corners do not count. Two squares meeting at a point hold nothing up, in
    // this world or in any building that has ever been built.
    bool Founded(const World &world, int cx, int cy) const;

    // Draws the grid of cells within reach, and the block of them the next action
    // covers.
    void DrawGrid(const Stack &held, float zoom) const;

    // How the ground gives way: not at once, but to steady work against one place.
    //
    // A bite is against the *stroke* — the block of cells the brush covers — and it
    // costs the sum of what is in them, so digging runs at the same cells per second
    // whatever the span is set to. See ElementDef::hardness.
    //
    // It is thrown away the moment the aim moves off the block it was started
    // against, which is the mechanic and not a shortcoming of it: what makes
    // breaking a thing feel like work is that it has to be worked at. `let` is what
    // says so on screen — see Editor::Biting.
    struct Bite {
        int cx = 0;
        int cy = 0;
        int w  = 1;
        int h  = 1;

        // What the block held when the bite began. A cell that changed under the
        // cursor — dug by something else, or flooded — is not the thing the player
        // started on, and finishing the old bite would break something they never
        // aimed at.
        std::array<int, kElementCount> was{};

        float done  = 0.0f;   // seconds of work put in
        float takes = 0.0f;   // seconds the block needs; zero when there is nothing to break

        // The bar letting go: seconds left of the ease, and the health it had when
        // the player stopped. It refills to full and fades as it does, which is the
        // whole message — the work is lost, the *block* is not.
        float let     = 0.0f;
        float letFrom = 0.0f;

        int letX = 0;
        int letY = 0;
        int letW = 1;
        int letH = 1;
    };

    // How long the bar takes to refill and fade after the player stops.
    //
    // Short. It is an answer to a question the player asked by letting go, and an
    // answer still arriving a second later is being given to somebody who has
    // already moved on.
    static constexpr float kLetGo = 0.28f;

    Bite bite_{};

    // Gives the bite up, and starts the bar refilling from wherever it had got to.
    // Called from every path where the player is not working this frame, and it is
    // cheap and idempotent on purpose: a rule spread over a dozen early returns has
    // to be safe to state a dozen times.
    void LetGo();

    int span_ = 1;

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

    // How far into breaking the aimed block the player is, as the health left in
    // it, and how much of that to draw.
    //
    // Two numbers because the bar has two lives: while the button is down it is the
    // work done, and after it comes up it is an animation of that work being given
    // back. Nothing outside needs to know which of the two it is looking at.
public:
    struct Progress {
        bool showing = false;

        float health = 1.0f;   // 1 is untouched, 0 is broken
        float ink    = 1.0f;   // fades out as the bar refills

        Rectangle over{};      // the block it belongs to
    };

    Progress Biting() const;

private:
    // Whether the cursor is over something the axe would bite. Held from the last
    // update so the cursor and the click agree about which tool this is.
    bool timber_ = false;

    // And whether the block under it holds anything at all to break.
    //
    // What the cursor is *for*: the mark is a promise about what the next click
    // does, so over open air there must not be one. Minecraft's rule, and it is
    // the difference between a cursor that says which block is selected and a
    // square that follows the mouse across the sky saying nothing.
    //
    // Asked of the world through the same walk the spade uses, and not of the
    // cell's middle — see CLAUDE.md §13.5. At a contour edge the middle is open
    // sky while a corner still holds ground, and a cursor that went blank there
    // would be blank over ground that is solid to a body and diggable by the
    // spade.
    bool holding_ = false;

    std::optional<Element> under_;
    Vector2 aim_{};

    // Where the reach is measured from, kept from the last update so the grid can
    // be drawn around the player without the drawing being handed the body again.
    Vector2 from_{};

    // The build grid's answer for this frame, worked out once beside footing_ and
    // read by both the grid that shows it and the click that acts on it. Two
    // answers to one question is a preview that lies — see Footing.
    bool building_  = false;
    bool buildable_ = false;
    bool onCell_    = false;
    int cellX_      = 0;
    int cellY_      = 0;

    // The three ways a cell refuses, kept apart because they ask the player for
    // three different things: one to build from something that holds, one to get out
    // of the way, and one to clear the space first.
    bool founded_ = false;
    bool roomy_   = false;
    bool vacant_  = false;


    // The ground under the cursor, where the hand holds something that would be
    // stood on it. Empty otherwise, so nothing is worked out for a hand that has
    // no use for it.
    std::optional<Vector2> footing_;
    bool rooted_ = false;

    bool reachable_ = false;
};
