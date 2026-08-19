#pragma once

#include "raylib.h"

#include <cstddef>

// A small hand-drawn picture, and what it means to draw one.
//
// Two tables need a picture at this size and for the same reason: an item lying
// in the grass and a material sitting in an inventory slot both have to be
// recognised at a glance from across a clearing, and neither is trying to be to
// scale. A log drawn to scale against a tree would be four pixels of brown.
//
// It is a type of its own rather than a field on either table because a slot
// holds one or the other, and a slot cannot have one of them be a drawing and
// the other a flat coloured rectangle.

// Side of a picture, in texels.
//
// Six, which at the plant grid is twelve world pixels — the width of the
// character.
inline constexpr int kPictureSide = 6;

// How many tones one picture is drawn from.
//
// Four is what this size carries: a lit face, a body, a shadow and one accent,
// which is exactly the vocabulary the canopy uses and for the same reason.
inline constexpr std::size_t kPictureTones = 4;

struct Picture {
    // Lit face, body, shadow, accent. Every picture is drawn from its own four
    // and nothing else, so recolouring one is a single row.
    Color tone[kPictureTones];

    // One row of characters per row of texels: `a` through `d` are the tones
    // above and a full stop is nothing.
    //
    // Written out rather than generated. A log and an apple have nothing in
    // common to generate *from*, and at six texels a side the whole picture is
    // thirty-six characters — which is smaller, and far easier to change by eye,
    // than any rule that could have produced it.
    const char *art[kPictureSide];
};

// The tone a mark stands for, or nothing where the mark is blank.
//
// The encoding lives here rather than in each caller because there are two
// kinds of caller and they cannot share a drawing routine: one paints through
// raylib, the other writes texels straight into an image for the contact sheet.
// What they can share is what a character means.
inline constexpr const Color *ToneAt(const Picture &picture, char mark) {
    if (mark < 'a' || mark >= static_cast<char>('a' + kPictureTones)) return nullptr;

    return &picture.tone[static_cast<std::size_t>(mark - 'a')];
}

// Whether every row of the art is exactly as long as the picture is wide.
//
// Checked rather than trusted, and each table asserts it over its own rows. A
// row one character short is not a syntax error and not a crash: the loop meets
// the terminator, the mark reads as blank, and the picture draws with a hole
// down one side. That is precisely the kind of fault that survives being looked
// at, since a picture with a hole in it still looks like a picture.
inline constexpr bool IsSquare(const Picture &picture) {
    for (int row = 0; row < kPictureSide; row++) {
        int length = 0;
        while (picture.art[row][length] != '\0') length++;

        if (length != kPictureSide) return false;
    }

    return true;
}

// Draws the picture with its top-left corner at `at`, one square per texel.
//
// The corner rather than the centre, and left unsnapped, because only the caller
// knows which grid it is drawing against: a pickup lying in the world is
// anchored to the world so that it does not crawl as the view scrolls, while a
// slot is anchored to the frame and moves with it.
void DrawPicture(const Picture &picture, Vector2 at, float pixel);
