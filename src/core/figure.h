#pragma once

#include "core/picture.h"
#include "raylib.h"

#include <cstddef>

// A hand-drawn picture of a creature, at whatever size the creature is.
//
// The second art type in this project, and the head of picture.h is half the
// reason there are two. A `Picture` is exactly six texels square because it is an
// *icon*: a thing in a slot, a thing lying in the grass, something recognised at
// a glance and deliberately not to scale. A boar is the opposite question — it
// stands in the world, it is collided with, and it has to be as wide as its own
// collider or the drawing is a lie about where the animal is.
//
// So a figure declares its own width and height, and it is drawn at
// `config::kPixelSize` like everything else in the world (see CLAUDE.md §12): one
// texel per world square, on the world's own grid, so a mob standing beside a
// wall is drawn on the same lattice the wall is.
//
// It is not a merge of the two. An icon that could be any size would need every
// slot, every hotbar cell and every pickup to ask how big this one is, and the
// answer would always be six.
namespace figure {

// Most texels a figure may be across or tall.
//
// Twelve, which at the world's three-pixel texel is thirty-six pixels — a little
// over the character's height and comfortably more than anything meant to be
// fought at this scale. A bound rather than a size: a bat is four.
inline constexpr int kMostSide = 12;

// How many tones a figure is drawn from. The same four as an icon, and for the
// same reason: a lit face, a body, a shadow and one accent is the vocabulary this
// resolution carries.
inline constexpr std::size_t kTones = kPictureTones;

struct Figure {
    // Lit face, body, shadow, accent. Recolouring a creature is one row.
    Color tone[kTones];

    // In texels. Both are checked against the art at compile time by
    // `IsWellFormed` below, which every table asserts over its own rows.
    int width  = 0;
    int height = 0;

    // One row of characters per row of texels: `a` through `d` are the tones and a
    // full stop is nothing. Rows past `height` are never read and are written as
    // nullptr.
    const char *art[kMostSide];
};

// The tone a mark stands for, or nothing where the mark is blank.
inline constexpr const Color *ToneAt(const Figure &fig, char mark) {
    if (mark < 'a' || mark >= static_cast<char>('a' + kTones)) return nullptr;

    return &fig.tone[static_cast<std::size_t>(mark - 'a')];
}

// Whether the declared size is the size that was actually drawn.
//
// Checked rather than trusted, for the reason `IsSquare` gives one file over: a
// row one character short is not a syntax error and not a crash — the loop meets
// the terminator, the mark reads as blank, and the creature is drawn with a hole
// down one side. That is precisely the kind of fault that survives being looked
// at, because a picture with a hole in it still looks like a picture.
inline constexpr bool IsWellFormed(const Figure &fig) {
    if (fig.width <= 0 || fig.width > kMostSide) return false;
    if (fig.height <= 0 || fig.height > kMostSide) return false;

    for (int row = 0; row < fig.height; row++) {
        if (fig.art[row] == nullptr) return false;

        int length = 0;
        while (fig.art[row][length] != '\0') length++;

        if (length != fig.width) return false;
    }

    return true;
}

// Draws the figure with its bottom-centre at `at` — which is where a body's own
// anchor is, so a creature is drawn standing on its feet rather than hung from
// its top-left corner.
//
// `facing` is +1 for right and -1 for left, and it mirrors the art rather than
// asking for a second copy of it drawn the other way round. `tint` multiplies
// every tone, which is what the hurt flash and the night are made of; pass WHITE
// for none.
void Draw(const Figure &fig, Vector2 at, float pixel, int facing, Color tint);

} // namespace figure
