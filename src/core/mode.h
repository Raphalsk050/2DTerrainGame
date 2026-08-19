#pragma once

// How a world is played, chosen when it is made and never after.
//
// Two rules and not two games. Everything in the world generates, grows, flows
// and is lit the same way in both; what changes is what the hand costs — whether
// breaking a block takes work, whether placing one spends anything, and whether
// the inventory is what the player has gathered or every material there is.
//
// It lives in a header of its own, tiny as it is, because three modules that
// should not know about each other have to agree on it: the editor, which is
// where the two rules actually differ; the inventory, which is a palette in one
// mode and a bag in the other; and the menu, which is where the choice is made.
// Putting it in any one of those would make the other two depend on that one.
enum class Gamemode {
    // What the game has always been: a block costs the seconds its hardness is
    // written as, and what goes into the world comes out of what was dug.
    Survival,

    // Minecraft's: blocks come away at a touch, nothing is spent putting them
    // back, and the inventory is every material in the table rather than the few
    // that have been carried up a hill.
    Creative,
};

inline const char *NameOf(Gamemode mode) {
    return (mode == Gamemode::Creative) ? "creative" : "survival";
}
