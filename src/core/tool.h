#pragma once

#include <cstdint>

// What is being swung, and how well.
//
// A header of its own, tiny as it is, for exactly the reason `core/mode.h` is one:
// the material table has to say which tool it gives way to, the item table has to
// say which tool a thing *is*, and the hand has to compare the two — and none of
// the three should have to include either of the others. An item row that had to
// include `world/element.h` in order to name a pickaxe would be an item row that
// depends on the whole of the ground.

// Which tool a material gives way to, and which tool a thing is.
//
// `Hand` in a *material's* row means **nothing helps**: soil comes away at the same
// rate whatever is held. It is not "a bare hand is right for it". `Hand` in an
// *item's* row means the thing is not a tool at all.
enum class Tool : std::uint8_t {
    Hand,
    Pick,
    Shovel,
    Axe,
    Sword,
};

namespace tool {

// Minecraft's own tier multipliers, and they are the whole of what a better tool
// buys. Taken from the wiki's own breaking article rather than guessed at.
//
// Gold above diamond is not a mistake and it is worth knowing before anybody
// "fixes" it: gold is the fastest material in the game and the most fragile, which
// is the trade Minecraft makes with it.
//
// **No netherite.** Not an oversight and not a tier left for later: netherite is
// smelted from ancient debris, which is found in the Nether, and there is no Nether
// here. A constant for a material the world cannot contain is a constant nobody can
// ever reach, and the day it means something is the day it is added — beside the
// ore that gives it, in the same commit.
inline constexpr float kHand    = 1.0f;
inline constexpr float kWood    = 2.0f;
inline constexpr float kStone   = 4.0f;
inline constexpr float kCopper  = 5.0f;
inline constexpr float kIron    = 6.0f;
inline constexpr float kDiamond = 8.0f;
inline constexpr float kGold    = 12.0f;

// Copper is *not* Minecraft's, because Minecraft has no copper tool, and it is here
// for the reason every other number in this file is not invented: there is copper in
// the ground already — `world/elements/copper.h` — and there is now a head drawn for
// it. A tier had to be chosen and the only honest place to choose it is between the
// two it sits between in the world: stone comes off a hillside and iron is dug for.
// Five is the one value that keeps every gap in the ladder at least one whole step.

// How many blows one tool has in it before it is gone.
//
// Minecraft's own durability figures, from the wiki's tools article, and they are the
// half of a tier that the speed above does not say: gold is the fastest material in
// the game *and* the most fragile, and neither number means anything without the
// other. A ladder written in speed alone would make gold strictly the best tool
// there is, which is precisely the trade Minecraft does not offer.
//
// Copper again has no figure to take, and again sits where it sits: between stone's
// hundred and thirty and iron's two hundred and fifty.
inline constexpr int kWoodLasts    = 59;
inline constexpr int kStoneLasts   = 131;
inline constexpr int kCopperLasts  = 190;
inline constexpr int kIronLasts    = 250;
inline constexpr int kGoldLasts    = 32;
inline constexpr int kDiamondLasts = 1561;

// What one held thing is worth to a hand.
//
// One struct rather than three fields scattered over the item row, because they are
// only ever read together and because a tool that had a speed and no kind would be
// a tool that is fast at nothing.
struct Kit {
    Tool kind = Tool::Hand;

    // The tier multiplier above. Divides the seconds a block takes, and **only
    // where the kind is the one the material asks for** — Minecraft's rule, and the
    // reason a pickaxe is no help at all against dirt. See BreakSeconds, which is
    // the one place it is applied.
    float speed = kHand;

    // Added to a bare fist when the thing is swung at something alive.
    //
    // Here rather than on a weapon table of its own, for the reason the speed is
    // here: a sword and a pickaxe are the same kind of object to everything that
    // carries them, and splitting them would mean two tables that have to agree
    // about what a slot holds.
    int damage = 0;

    // How many blows it has in it, and nought where it never wears out.
    //
    // Here beside the speed rather than on a table of its own, because the two are
    // the same fact seen twice — what a tier *is* is a rate and a lifetime together,
    // and gold is the proof: it is the fastest thing in the game and lasts a third
    // as long as wood. Split across two tables they would be two things to keep in
    // step; on one row they cannot disagree.
    //
    // Nought means "does not wear", which is what nearly every row says. It is the
    // same "says nothing" `nullptr` means in the tables that name each other, and it
    // is what makes a torch and a hide answer this question without being asked it.
    int lasts = 0;

    // Whether this is a tool at all, for the palette and for the checks. A row that
    // says nothing is not a tool, which is the great majority of them.
    constexpr bool Any() const { return kind != Tool::Hand || speed > kHand || damage > 0; }

    // Whether it wears out with use. See `Stack::wear`, which is where the count of
    // how much of that has happened to *one particular tool* lives — this row is
    // shared by every copy of it there has ever been and cannot hold that.
    constexpr bool Wears() const { return lasts > 0; }
};

} // namespace tool
