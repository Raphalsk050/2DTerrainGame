#pragma once

#include "raylib.h"

// The colours every panel in this game is drawn from.
//
// They were written out separately in `hotbar.cpp` and `inventory.cpp`, and the
// amber of a stack's count was already in both — the same three numbers twice, in
// two files, with nothing saying they had to agree. That is §25.1's complaint about
// the foot of the screen said one layer further in: things that are meant to look
// like one interface have to *be* one interface somewhere, or the first retune
// makes two of them.
//
// A header of constants and nothing else, so a panel includes it and owes it
// nothing.
namespace skin {

// The body of a panel, and the line round it.
inline constexpr Color kPanel = {24, 27, 34, 245};
inline constexpr Color kEdge  = {96, 104, 120, 255};

// The bar along the foot, which is opaque where a panel is not: the world keeps
// scrolling behind it and a translucent strip makes that movement read as the strip
// itself glitching.
inline constexpr Color kBar = {30, 34, 42, 255};

// The well one picture sits in, and the ring round the one in hand.
inline constexpr Color kSlot    = {60, 66, 78, 255};
inline constexpr Color kOutline = {255, 255, 255, 255};

// A tooltip's body, darker than a panel so it reads as being in front of one.
inline constexpr Color kTip = {18, 20, 26, 235};

// Text, in three weights: what is being read, what is beside it, and what is only
// there to be found.
inline constexpr Color kText  = {236, 240, 248, 255};
inline constexpr Color kMuted = {150, 158, 172, 255};
inline constexpr Color kDim   = {104, 112, 126, 255};

// The accent. A stack's count, a key cap's number, the face of a button that can be
// pressed — one colour for "this is the number that matters", used sparingly enough
// that it still means that.
inline constexpr Color kAccent = {255, 214, 110, 255};

// Under a number drawn over a picture. A count sits on top of whatever material is
// in the slot, and over sand or snow or gold a pale number on a pale face is not a
// number at all.
inline constexpr Color kShadow = {12, 14, 18, 220};

// Whether a requirement is met. Deliberately the one place this interface uses
// green and red, because it is the one question a glance has to answer without
// reading anything.
inline constexpr Color kMet   = {148, 206, 128, 255};
inline constexpr Color kShort = {226, 104, 92, 255};

} // namespace skin
