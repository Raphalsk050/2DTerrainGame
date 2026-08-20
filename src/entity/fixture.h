#pragma once

#include "core/config.h"
#include "core/picture.h"
#include "core/sheet.h"
#include "core/stack.h"
#include "item/item_def.h"
#include "item/slots.h"
#include "render/light.h"
#include "world/element.h"
#include "raylib.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

class World;
class Drops;

namespace save {
class Writer;
class Reader;
} // namespace save

// Things that stand in the world without being made of it.
//
// A third table, and the head of item.h and element.h both explain why a third
// one is not one too many. An element is a material: it has a field over the
// lattice, a threshold, a rank against every other material and a rule for where
// it generates. An item is a name, a picture and a count. A torch is neither. It
// has a *place* — one cell, chosen by a player — and it has a picture that is a
// picture of a thing rather than of a material's face.
//
// The world already had a torch as an element and it was the wrong shape twice
// over. Drawn as a material it was an eighteen-pixel square of flame colour,
// which says nothing about what it is; and laid by the brush it went into every
// vertex the brush covered, so one click put down a dozen torches and the room
// lit up like a furnace. The comment in the old row admitted as much.
//
// What this is not: an entity system. There is no update, no velocity and no
// behaviour. A fixture is a row of a table and a cell it sits in, and everything
// else about it is looked up.
//
// ---
//
// **A chest is the second kind, and it is what a fixture was already shaped for.**
// It is put in a cell by a player, it stands on a surface, it comes down when that
// surface is dug away, it is made from an item and it is taken back as one. Every one
// of those sentences was already written here for the torch. What a chest adds is that
// it *remembers* — see `Def::slots` — and joining, which is `Def::joins`. Neither is a
// new kind of thing; both are a field on a row.
namespace fixture {

// Where a fixture can be fixed.
//
// A bitmask rather than a single answer, because "any surface" is a real answer
// and the commonest one — it is what the player asked of the torch. A chest wants
// Floor alone, and a sign will want Wall alone, and both are one field in the table
// below.
enum Anchor : unsigned char {
    kFloor  = 1 << 0, // solid ground under it
    kWall   = 1 << 1, // solid to the left or the right
    kRoof   = 1 << 2, // solid above it
    kBehind = 1 << 3, // a background wall in its own cell

    kAnywhere = kFloor | kWall | kRoof | kBehind,
};

enum class Kind : std::uint8_t { Torch, Chest, Count };

inline constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

// Which piece of a joined run one unit is.
//
// A bank of chests is one store to the player and a row of separate objects on the
// hillside, so the art has to be able to say which end of it this one is. Four
// answers rather than two, because the middle of a run of three is neither end.
enum class Piece : std::uint8_t { Alone, Left, Middle, Right };

struct Def {
    const char *name;

    // The item this is put up from.
    //
    // Named rather than matched. It used to be found by comparing this row's `name`
    // against the item's, which worked and was a trap with a very long fuse: the two
    // tables were tied together by a *string*, so renaming either — the sort of thing
    // done without thinking, in a commit about wording — silently broke the link, and
    // what a player saw was a torch that could no longer be placed. Nothing in either
    // file said the names had to agree.
    //
    // Adding a fixture is now: a row here that names its item, and a row in item.h
    // with `.placement = Placement::Fixture`. The startup check fails the program if
    // the second half is forgotten.
    const char *from;

    Picture picture;

    // The three faces a joined run is drawn from — its left end, its middle, and its
    // right end — or nothing for a fixture that never joins.
    //
    // **This is what makes a bank read as one chest instead of three.** Drawn from the
    // one `picture` above, three chests standing side by side are three boxes with two
    // seams down them; what the player built is one store and it has to look like one.
    // So the units that continue into a neighbour drop the side border on that edge,
    // the lid and the band run straight through, and the clasp sits on the join.
    //
    // Written out by hand rather than derived from `picture` by a rule. A rule that
    // opened one edge would be three lines and would be wrong the first time a fixture
    // has a shape the rule did not expect — and at six texels a side the whole face is
    // thirty-six characters, which is easier to change by eye than any rule that could
    // have produced it. It is `Picture::art`'s own argument, one table over.
    //
    // Authored art overrides it entirely: `Wardrobe::For` picks a strip per piece from
    // the same four names, and this is what a missing file falls back to.
    const Picture *joined = nullptr;


    // Where the authored art for this row lives, under `assets/fixtures/`, and how
    // wide one frame of it is. Nothing where the row is drawn from the `picture` above
    // instead.
    //
    // **A folder and not a file**, which is §24.2's rule and for its reason: there is
    // more than one picture inside. What is in there is **one picture per bank size** —
    // `small_<name>.png` for a fixture standing alone, then `medium_`, `large_` and
    // `huge_`, as many as the row `joins`.
    //
    // That is the contract the art arrived in, and it is the better one. The first
    // design was three *pieces* — a left end, a middle and a right end — to be laid
    // side by side, which is how a tiling engine would do it and which makes joining a
    // problem of hiding the seams. A picture of the whole bank has no seams to hide: two
    // chests joined are one drawing of two chests joined, and what the artist drew is
    // what the player sees. The hand-drawn `picture` still tiles, because a fallback has
    // to work at any size (§30.10b) — but where there is art, the art is the bank.
    //
    // Each file's frames are the lid opening: frame nought shut, the last one wide open.
    // A one-frame file is a chest that never appears to open and is still correct.
    //
    // **Only the drawn-on part is drawn**, and it stands on the floor of the cells the
    // bank occupies — see `sheet::Strip::solidX`. So these may be authored on the same
    // 64-square canvas every tool is drawn on (§29.1) with the chest sitting anywhere in
    // it, and `artWide` is that canvas rather than a cell.
    //
    // **It is drawn front-on and never mirrored.** A fixture has no facing: a chest
    // seen from the side could not show the player which of its neighbours it had
    // joined with, which is the whole of what a bank has to say for itself.
    const char *art = nullptr;
    int artWide     = config::kBuildCell;

    // Which surfaces will hold it.
    unsigned char anchors = kAnywhere;

    // What it gives off, on the same terms as an element's own light — so a
    // fixture and a material describe a lamp the same way and the solver is told
    // nothing new.
    ElementLight light;

    // How many slots one of these remembers, and nought for one that remembers
    // nothing — which is a torch and everything else in this table today.
    //
    // It has to be a whole number of rows of `slots::kAcross`, and the startup check
    // says so: a sorting rule is about a row, and a row that straddled two chests
    // would have nowhere to live that survives one of them being dug up.
    int slots = 0;

    // How many of these will stand side by side as one store, itself counted.
    //
    // One means it never joins. Three is the chest, and a placement that would make a
    // run of four is refused rather than quietly left out of the bank — see `Joins`,
    // and the head of `Fixtures::Run` for why the alternative is worse.
    int joins = 1;

    // How long the lid takes to swing, in seconds.
    float opens = 0.18f;

    bool Remembers() const { return slots > 0; }

    int Rows() const { return slots / slots::kAcross; }
};

// The chest's four tones, written once because five pictures are drawn from them.
inline constexpr Color kChestTones[kPictureTones] = {
    {198, 150, 92, 255}, {158, 112, 64, 255}, {104, 70, 40, 255}, {58, 44, 32, 255}};

// A joined chest, seen from the front: the left end, the middle, and the right end.
//
// Read them side by side and they are one long box. The end pieces keep the border on
// their outer edge and lose it on the inner one; the middle keeps neither, so the lid,
// the iron band and the body all run straight through the join.
//
// **Nothing dark stands at a seam**, and that is the rule rather than a detail. The
// borders of these pictures are the darkest tone, so any dark texel at the edge where
// two units meet is read by the eye as a border — which is exactly the "three boxes in
// a row" this exists to stop. The first draft put half a clasp on each inner edge; it
// was centred and symmetrical and it looked like a seam, which `--chest` measured and
// said so.
//
// So the clasp went to the middle of the **middle** face instead, which puts it dead
// centre of a run of three. A run of two has none, and a long box with a continuous
// band still reads as a chest — the ends are what say so. Authored art can put a lock
// wherever it likes; this is what a missing file falls back to.
//
// **And it sits on the band rather than on the body**, in the palest tone rather than
// the darkest, which is Minecraft's arrangement and is the second thing `--chest`'s
// sheet showed. Two dark texels notched into the body read as a gap in the box — at
// six texels a side a single chest came out looking like a doorway. A pale lock on a
// dark iron band reads as metal on wood, which is what it is.
inline constexpr Picture kChestFaces[3] = {
    // Left: bordered left, running on to the right.
    {
        .tone = {kChestTones[0], kChestTones[1], kChestTones[2], kChestTones[3]},
        .art =
            {
                ".ddddd",
                "daaaaa",
                "dddddd",
                "dbbbbb",
                "dbbbbb",
                ".ddddd",
            },
    },
    // Middle: open on both sides, and the clasp.
    {
        .tone = {kChestTones[0], kChestTones[1], kChestTones[2], kChestTones[3]},
        .art =
            {
                "dddddd",
                "aaaaaa",
                "ddaadd",
                "bbbbbb",
                "bbbbbb",
                "dddddd",
            },
    },
    // Right: running on to the left, bordered right.
    {
        .tone = {kChestTones[0], kChestTones[1], kChestTones[2], kChestTones[3]},
        .art =
            {
                "ddddd.",
                "aaaaad",
                "dddddd",
                "bbbbbd",
                "bbbbbd",
                "ddddd.",
            },
    },
};

inline constexpr Def kKinds[] = {
    {
        .name = "torch",
        .from = "torch",

        // The row the old element carried, kept exactly: a flame over a shaft,
        // with the shaft the darkest of its own four rather than a brown fetched
        // from somewhere else. It was the one picture in the element table that
        // was not a block face, which was the first sign it did not belong there.
        .picture =
            {
                .tone = {{255, 242, 210, 255}, {255, 216, 150, 255}, {226, 170, 92, 255}, {196, 128, 44, 255}},
                .art =
                    {
                        "..a...",
                        ".aab..",
                        ".abbc.",
                        "..bc..",
                        "..d...",
                        "..d...",
                    },
            },

        // Any surface at all, which is what was asked for and what Minecraft
        // allows. A torch is the first light a player has and the last thing that
        // should be fussy about where it goes.
        .anchors = kAnywhere,

        // Brighter than the sky, because it has to carry a room on its own while
        // daylight arrives from every direction at once. It can afford to be
        // brighter than the old element's was: that figure had to be small because
        // a brush laid a dozen of them at once, and this is one torch.
        .light = {.opacity = 0.0f, .glow = {255, 198, 130, 255}, .strength = 3.0f},
    },
    {
        .name = "chest",
        .from = "chest",

        // Front-on: a lid over a body with an iron band and a latch between them. It
        // is a fallback and it is drawn to be one — the moment `art` names a folder
        // this is what a missing file falls back to, and it is the one description of
        // the chest that cannot go out of date, being in the same file as everything
        // else about it (§24.2).
        //
        // Four tones from pale oak to iron, the same ramp the wood plank is painted
        // in, so a chest set into a plank wall reads as being made of the wall.
        .picture =
            {
                .tone = {kChestTones[0], kChestTones[1], kChestTones[2], kChestTones[3]},
                .art =
                    {
                        ".dddd.",
                        "daaaad",
                        "ddaadd",
                        "dbbbbd",
                        "dbbbbd",
                        ".dddd.",
                    },
            },

        // And the three faces a run of them is drawn from, so that joining chests joins
        // their pictures. See `Def::joined`.
        .joined = kChestFaces,

        // `assets/fixtures/chest/small_chest.png` and its two larger brothers, one per
        // bank size. The 64-square canvas is the one every tool is drawn on, and only
        // the drawn-on part of it reaches the screen — see `Def::art`.
        .art     = "chest",
        .artWide = 64,

        // The floor and nothing else. A chest hangs off no wall and off no ceiling: it
        // is the one fixture in here that is furniture rather than fitting, and a
        // chest stuck to a roof would be a thing the player cannot reach into.
        .anchors = kFloor,

        // Dark, like everything else that is not a lamp.
        .light = {.opacity = 0.0f, .glow = {0, 0, 0, 0}, .strength = 0.0f},

        // Thirty-two, which is four rows of eight. Minecraft's is twenty-seven and
        // this is not it: a cell here is about a quarter of a Minecraft block by area
        // (§13.1) and a player fills a bag several times as fast for it, so the store
        // that answers the same need is a larger one.
        .slots = 32,

        // Three side by side, so a full bank is ninety-six — which is two and a half
        // times what the player can carry, and the point at which a store stops being
        // somewhere to put the overflow and starts being somewhere to keep things.
        .joins = 3,
    },
};

static_assert(std::size(kKinds) == kKindCount, "every Kind needs exactly one row in kKinds");

inline constexpr const Def &Of(Kind kind) {
    return kKinds[static_cast<std::size_t>(kind)];
}

// Which fixture an item puts up, where it puts up one at all.
//
// Matched on the name, which is the same trick flora::SpeciesOf uses to tie a
// sapling to its tree. It looks flimsy and is the opposite: the alternative is an
// index written down twice, in two tables that are edited at different times by
// somebody thinking about different things, and the first row inserted above
// either of them silently makes torches out of apples.
inline std::optional<Kind> KindOf(Item what) {
    for (std::size_t k = 0; k < kKindCount; k++) {
        const std::optional<Item> mine = item::Named(kKinds[k].from);

        if (mine.has_value() && *mine == what) return static_cast<Kind>(k);
    }

    return std::nullopt;
}

// The item one of these is taken back as.
//
// One place, because there were two and both were the word `torch` written into a
// call: `Editor::Update` when a fixture is dug and `Fixtures::Undermine` when its
// footing goes. Both were correct for exactly as long as there was one fixture in the
// game, and both would have handed the player a torch for a chest — §16.2b's fault
// with the fuse already lit.
inline std::optional<Item> ItemOf(Kind kind) {
    return item::Named(Of(kind).from);
}

// What a bank of one, two, three or four is called on disk.
//
// Words rather than numbers, because that is how the art arrived and because it is what
// an artist would name them. The row's own `joins` says how many of these are ever
// looked for, so a fixture that never joins costs one file and one load.
inline constexpr const char *kSizes[] = {"small", "medium", "large", "huge"};

inline constexpr std::size_t kMostSizes = std::size(kSizes);

// The pictures one fixture is drawn from, loaded once and kept.
//
// `mob::Wardrobe`'s design, and the head of that file gives the reasons: it is asked
// for by row and never by path, so there is no filename anywhere in the table to
// misspell, and it is lazy because `content::Open` runs before there is a window and a
// texture needs one.
//
// One entry per bank size rather than one per piece — see `Def::art` for why the whole
// bank is a picture rather than a row of tiles.
struct Wardrobe {
    sheet::Strip banks[kMostSizes];

    // Whether the load has been attempted. Distinct from whether it worked: a fixture
    // with no art, and one whose art is missing, must both stop trying — a load retried
    // every frame is a file opened sixty times a second for as long as the game runs.
    bool tried = false;

    bool Any() const {
        for (const sheet::Strip &strip : banks) {
            if (strip.Ready()) return true;
        }

        return false;
    }

    // The picture for a run of `units`, or nothing where that size was never drawn.
    //
    // Nothing rather than a fallback to another size. A run of three drawn from the
    // picture of one is a chest that does not cover the ground it stands on, and a run
    // of three drawn from the picture of two is simply the wrong chest — where the size
    // is missing the hand-drawn faces tile, which at least covers the right cells.
    const sheet::Strip *For(int units) const {
        const std::size_t at = static_cast<std::size_t>(units - 1);

        if (units < 1 || at >= kMostSizes || !banks[at].Ready()) return nullptr;

        return &banks[at];
    }
};

const Wardrobe &Dressed(const Def &def);

// Which hand-drawn face a unit shows, given where in its run it stands.
//
// The row's own `picture` for a chest standing alone, and one of its three joined faces
// otherwise. One answer, asked by the draw and by the contact sheet, so a fixture
// cannot be drawn one way in the world and another on the sheet that checks it.
inline const Picture &FaceOf(const Def &def, Piece piece) {
    if (def.joined == nullptr || piece == Piece::Alone) return def.picture;

    return def.joined[static_cast<std::size_t>(piece) - 1];
}

// Gives back every texture. Called beside `mob::Undress` on the way out, and for the
// same reason: a texture outliving the window it was made in is a crash on exit that
// only ever happens on somebody else's machine.
void Undress();

// One fixture, standing in one cell of the build grid.
struct Placed {
    Kind kind = Kind::Torch;

    int cx = 0;
    int cy = 0;

    // What is in it, sized from the row when it was put up. Empty for anything that
    // remembers nothing, which costs a torch three pointers and no allocation.
    //
    // **Per unit and never per bank**, which is rule six of the chest and the reason
    // the units are not poured into one buffer when they join: breaking one has to
    // drop what is in *that* one and leave the rest standing where they are.
    std::vector<Stack> kept;

    // One kind set aside per row of its own grid, or nothing where that row takes
    // whatever comes. Sized from the row alongside `kept`.
    std::vector<slots::Kind> rows;

    // How far the lid has swung, nought shut to one open.
    //
    // On the unit rather than on the bank, so three joined chests open together
    // because they are all told to and not because one of them is keeping the state
    // for the other two.
    float lid = 0.0f;
};

// A run of joined units, as one store.
//
// Derived from where the fixtures actually are and never written down, which is the
// whole of why it is trustworthy: a stored membership would have to answer what
// happens when the middle of a run of three is dug out, and every answer to that is a
// pair of chests sharing a bag across a hole in the wall. A maximal run of neighbours
// has no such question in it.
//
// It is a horizontal run because a chest is drawn front-on and joins along the front.
// A stack of them going up the wall is a wall of separate chests, which is how a store
// room is built.
struct Joined {
    int cx    = 0; // the leftmost unit
    int cy    = 0;
    int count = 0;

    bool Any() const { return count > 0; }

    // Which piece the unit at `index` is.
    Piece PieceAt(int index) const;
};

// Everything the player has put up, and nothing else.
//
// Sparse and permanent, keyed by cell, on the model of Grove::remembered_ and
// World::edits_ — and for the same reason as both: the generator is a pure
// function of position and no function of position produces a torch somebody
// hung. A chunk that goes and comes back is rebuilt from noise, so anything not
// written down here is gone.
class Fixtures {
public:
    // Puts one up. Returns false where the cell is taken, or where joining here would
    // make a run longer than the row allows.
    bool Place(Kind kind, int cx, int cy);

    // Whether a run through this cell would stay within `Def::joins`.
    //
    // Asked by the hand a frame before the click as well as by `Place`, so the refusal
    // can be drawn rather than only spoken — the same one-answer rule the rest of the
    // cursor follows.
    bool Joins(Kind kind, int cx, int cy) const;

    // Takes one down, and says whether there was one. The whole record comes back
    // through `what` — kind and contents together — because a store that is taken down
    // has to pay out what was in it, and a caller handed only the kind would have no
    // way to.
    bool Remove(int cx, int cy, Placed &what);

    std::optional<Kind> At(int cx, int cy) const;

    // The run of joined units this cell belongs to. Empty where there is nothing here,
    // or where what is here remembers nothing.
    Joined Run(int cx, int cy) const;

    // That run's slots, as one store.
    //
    // The bank points into the units themselves, so it is a way of addressing them and
    // not a copy: what the player puts in a slot is in the chest the moment they let
    // go, and there is no writing back to forget.
    slots::Bank Store(const Joined &run);

    // How many rows that store has.
    int Rows(const Joined &run) const;

    // Each row's rule, in bank order, as pointers into the units that own them.
    //
    // Pointers rather than a copy, and rather than a get-and-set pair, because the
    // panel has to read and write them and must know nothing about fixtures to do it —
    // the same reason `Store` hands out a bank pointing into the chests rather than a
    // copy of their contents. What it buys is a panel that can be photographed against
    // an array made up on the spot, with no world anywhere near it.
    void Rules(const Joined &run, std::vector<slots::Kind *> &out);

    // Whether the world will hold this kind in this cell.
    //
    // Static and taking the world, because the hand has to ask it a frame before
    // the click — the same one-answer rule the rest of the cursor follows.
    static bool Holds(const World &world, Kind kind, int cx, int cy);

    // Which run is standing open, if any. Told rather than worked out, because what is
    // open is a fact about the screen and not about the world — §14's argument for
    // keeping `packOpen` in the loop.
    void Opened(const Joined &run) { open_ = run; }
    void Shut() { open_ = {}; }

    // Swings every lid towards where it should be.
    //
    // On the frame clock and outside the gate that stops the world while a panel is
    // up, for the reason the digging bar's ease is (§13.4): a lid is a message to the
    // player about a panel that is open, not a thing happening in the world. A lid
    // inside the gate would be a chest that never finishes opening, because opening it
    // is what stopped the clock.
    void Animate(float dt);

    // Draws every fixture inside the view.
    void Draw(Rectangle view) const;

    // Offers each one's light to the world, for this frame.
    //
    // Re-offered every frame rather than registered once, which is the contract
    // World::AddLight already has with the lantern: a light that has to be renewed
    // to keep burning needs nothing told to it when what carries it is gone.
    void Illuminate(World &world, Rectangle view) const;

    // Drops any that have lost the surface they were fixed to, onto the ground as
    // pickups — with whatever was in them. The precedent is Grove::Undermine, which
    // fells a tree whose footing was dug away.
    void Undermine(const World &world, Drops &drops, float now);

    int Held() const { return static_cast<int>(placed_.size()); }

    void Clear() {
        placed_.clear();
        open_ = {};
    }

    // Everything standing, and everything in it.
    //
    // The contents are written per unit, exactly as they are held — which is rule six
    // of the chest arriving on disk: a bank is three records and not one, so a save
    // reloaded and then broken pays out the same as one that was never saved.
    void Save(save::Writer &out) const;
    void Load(save::Reader &in);

private:
    static std::int64_t Key(int cx, int cy);

    const Placed *Find(int cx, int cy) const;
    Placed *Find(int cx, int cy);

    // How many same-kind neighbours run away from this cell in one direction.
    int Reach(Kind kind, int cx, int cy, int step) const;

    std::unordered_map<std::int64_t, Placed> placed_;

    Joined open_{};

    // Scratch for Undermine, so a frame that drops nothing allocates nothing.
    mutable std::vector<std::int64_t> falling_;
};

} // namespace fixture
