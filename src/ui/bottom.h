#pragma once

#include "raylib.h"

// The strip along the foot of the screen, laid out as one thing.
//
// It was three things laid out separately and they collided. The hotbar put the held
// item's name twenty pixels above itself, the brush badge sat at a hundred and four
// from the bottom, and the health bar at ninety-six — three constants, in two files,
// none of which knew about the others, all landing inside the same twenty pixels. What
// that looks like on screen is two lines of text touching each other with a bar behind
// them.
//
// So the strip is one function of the window size and every row comes out of it. Not a
// remembered layout: computed again in the draw, which is CLAUDE.md §14's rule for
// every screen in the menu and is the same rule here — a layout held between the input
// pass and the draw is a button drawn where it can no longer be clicked.
//
// **The arrangement is Minecraft's**, because it is the one every player of this kind
// of game already reads without being taught:
//
//     wood plank            <- what is held, centred over everything
//   ♥♥♥♥♥♥♥♥♥♥      1x1     <- vitals on the left, the hand on the right
//   [1][2][3][4][5][6][7]   <- the hotbar
//
// Minecraft puts hunger where the hand's state is here. That half of the row is for
// "the thing about you that is not your health", and this game's version of that is
// which tool you are holding and how wide it cuts.
namespace bottom {

struct Strip {
    // The hotbar itself, which every other row is measured from.
    Rectangle bar{};

    // The row directly over it. Aligned to the bar's own edges rather than to the
    // screen's, so the whole strip reads as one block however wide the window is.
    //
    // Split by what the vitals actually need rather than down the middle. A half is a
    // guess, and the moment a heart changes size the guess is either crowding the row
    // or wasting it — the hearts ask for their width and the hand takes the rest.
    Rectangle vitals{};
    Rectangle hand{};

    // And the name of what is held, over both.
    Rectangle name{};
};

// How tall each row is and how far apart they sit, in screen pixels.
//
// The gap is what the old layout had none of. Two rows of text with nothing between
// them read as one paragraph, which is exactly the complaint.
// Tall enough for a heart, which is the tallest thing that goes in it.
inline constexpr float kRowTall = 22.0f;
inline constexpr float kRowGap  = 6.0f;

Strip Of();

} // namespace bottom
