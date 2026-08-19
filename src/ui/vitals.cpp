#include "ui/vitals.h"

#include <algorithm>
#include <cmath>

namespace {

// One heart, seven cells square.
//
// `o` is the outline, `a` the face. The outline is not decoration: a heart is drawn
// over whatever the world happens to be behind the hotbar, and a red shape on a red
// hillside at sunset is not a shape at all. It is the same argument `hud::Label` makes
// about the readout, in a picture instead of in text.
constexpr const char *kHeart[vitals::kSide] = {
    ".oo.oo.",
    "oaaoaao",
    "oaaaaao",
    "oaaaaao",
    ".oaaao.",
    "..oao..",
    "...o...",
};

constexpr Color kEdge  = {18, 20, 26, 235};
constexpr Color kEmpty = {74, 44, 48, 235};
constexpr Color kFull  = {222, 58, 62, 255};

// And a paler face on the last part-filled heart, so a half reads as a half rather than
// as a trick of the light.
constexpr Color kPart = {236, 108, 96, 255};

void One(float left, float top, float share) {
    // How many of the five face columns are filled. Five and not seven because the
    // outline takes a column each side, and rounding to the nearest is what gives the
    // half-heart its own step rather than a smear.
    const float faces = 5.0f;

    const int lit = static_cast<int>(std::round(std::clamp(share, 0.0f, 1.0f) * faces));

    for (int row = 0; row < vitals::kSide; row++) {
        for (int col = 0; col < vitals::kSide; col++) {
            const char mark = kHeart[row][col];

            if (mark == '.') continue;

            Color colour = kEdge;

            if (mark == 'a') {
                // Column one is the leftmost face. Filling from the left is what makes
                // a row of hearts drain the way a bar does, left to right.
                const int face = col - 1;

                colour = (face < lit) ? ((share < 1.0f && face == lit - 1) ? kPart : kFull) : kEmpty;
            }

            DrawRectangleV({left + static_cast<float>(col) * vitals::kCell,
                            top + static_cast<float>(row) * vitals::kCell},
                           {vitals::kCell, vitals::kCell}, colour);
        }
    }
}

} // namespace

void vitals::Draw(const life::Health &health, Gamemode mode, Rectangle where) {
    // God mode has nothing to say about how much of it is left. See the head of the
    // header for why this is a refusal rather than a full row.
    if (mode != Gamemode::Survival) return;

    const float tall = kSide * kCell;

    // Snapped to whole pixels, because the art is two-pixel cells and half of one is a
    // heart with columns alternating one wide and two.
    const float left = std::floor(where.x);
    const float top  = std::floor(where.y + (where.height - tall) / 2.0f);

    // How much of a heart each one is worth. Taken from the character's own maximum
    // rather than assumed to be two, so a row's meaning follows `kHealth` instead of
    // silently going wrong the day it moves.
    const float each = static_cast<float>(health.most) / static_cast<float>(kHearts);

    for (int h = 0; h < kHearts; h++) {
        const float from = static_cast<float>(h) * each;

        One(left + static_cast<float>(h) * (kSide * kCell + kApart), top,
            (static_cast<float>(health.now) - from) / std::max(each, 1e-3f));
    }
}
