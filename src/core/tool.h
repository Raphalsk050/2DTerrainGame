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
inline constexpr float kIron    = 6.0f;
inline constexpr float kDiamond = 8.0f;
inline constexpr float kGold    = 12.0f;

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

    // Whether this is a tool at all, for the palette and for the checks. A row that
    // says nothing is not a tool, which is the great majority of them.
    constexpr bool Any() const { return kind != Tool::Hand || speed > kHand || damage > 0; }
};

} // namespace tool
