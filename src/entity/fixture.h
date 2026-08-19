#pragma once

#include "world/element.h"
#include "item/item_def.h"
#include "render/light.h"
#include "core/picture.h"
#include "raylib.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

class World;
class Drops;

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
namespace fixture {

// Where a fixture can be fixed.
//
// A bitmask rather than a single answer, because "any surface" is a real answer
// and the commonest one — it is what the player asked of the torch. A workbench
// will want Floor alone, and a sign will want Wall alone, and both are one field
// in the table below when they arrive.
enum Anchor : unsigned char {
    kFloor  = 1 << 0, // solid ground under it
    kWall   = 1 << 1, // solid to the left or the right
    kRoof   = 1 << 2, // solid above it
    kBehind = 1 << 3, // a background wall in its own cell

    kAnywhere = kFloor | kWall | kRoof | kBehind,
};

enum class Kind : std::uint8_t { Torch, Count };

inline constexpr std::size_t kKindCount = static_cast<std::size_t>(Kind::Count);

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
    // with `.placement = Placement::Fixture`. The static_assert below fails the build
    // if the second half is forgotten.
    const char *from;

    Picture picture;

    // Which surfaces will hold it.
    unsigned char anchors = kAnywhere;

    // What it gives off, on the same terms as an element's own light — so a
    // fixture and a material describe a lamp the same way and the solver is told
    // nothing new.
    ElementLight light;
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
// either of them silently makes torches out of apples. A name is the one thing
// about a row that is already required to be what it says.
inline std::optional<Kind> KindOf(Item what) {
    for (std::size_t k = 0; k < kKindCount; k++) {
        const std::optional<Item> mine = item::Named(kKinds[k].from);

        if (mine.has_value() && *mine == what) return static_cast<Kind>(k);
    }

    return std::nullopt;
}

// One fixture, standing in one cell of the build grid.
struct Placed {
    Kind kind = Kind::Torch;

    int cx = 0;
    int cy = 0;
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
    // Puts one up. Returns false where the cell is taken.
    bool Place(Kind kind, int cx, int cy);

    // Takes one down, and says whether there was one. What it was is handed back
    // through `what` so the caller can pay the player for it.
    bool Remove(int cx, int cy, Kind &what);

    std::optional<Kind> At(int cx, int cy) const;

    // Whether the world will hold this kind in this cell.
    //
    // Static and taking the world, because the hand has to ask it a frame before
    // the click — the same one-answer rule the rest of the cursor follows.
    static bool Holds(const World &world, Kind kind, int cx, int cy);

    // Draws every fixture inside the view.
    void Draw(Rectangle view) const;

    // Offers each one's light to the world, for this frame.
    //
    // Re-offered every frame rather than registered once, which is the contract
    // World::AddLight already has with the lantern: a light that has to be renewed
    // to keep burning needs nothing told to it when what carries it is gone.
    void Illuminate(World &world, Rectangle view) const;

    // Drops any that have lost the surface they were fixed to, onto the ground as
    // pickups. The precedent is Grove::Undermine, which fells a tree whose footing
    // was dug away.
    void Undermine(const World &world, Drops &drops, float now);

    int Held() const { return static_cast<int>(placed_.size()); }

    void Clear() { placed_.clear(); }

private:
    static std::int64_t Key(int cx, int cy);

    std::unordered_map<std::int64_t, Placed> placed_;

    // Scratch for Undermine, so a frame that drops nothing allocates nothing.
    mutable std::vector<std::int64_t> falling_;
};

} // namespace fixture
