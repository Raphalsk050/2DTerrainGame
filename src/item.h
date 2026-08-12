#pragma once

#include "raylib.h"

#include <array>
#include <cstddef>
#include <iterator>

// Things a plant leaves behind when it is cut down or picked.
//
// Deliberately not rows in the element table. An element is a material the world
// is made of: it has a field over the lattice, a threshold, a rank against every
// other material and a rule for where it generates. An apple has none of those
// and would need all four invented for it, which is how a good table stops being
// one. What an item has is a name, a colour and a count, and this is the table
// of those.
enum class Item { Wood, Sapling, Apple, Resin, Fibre, Count };

inline constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::Count);

inline constexpr std::size_t ItemIndex(Item item) { return static_cast<std::size_t>(item); }

struct ItemDef {
    const char *name;

    // What it is drawn as while it lies on the ground waiting to be picked up.
    // One colour, because a dropped item is a few pixels across and a second
    // tone would land inside the first.
    Color colour;

    int stack;
};

inline constexpr ItemDef kItems[] = {
    {.name = "wood", .colour = {138, 92, 52, 255}, .stack = 999},
    {.name = "sapling", .colour = {96, 168, 74, 255}, .stack = 99},
    {.name = "apple", .colour = {214, 66, 58, 255}, .stack = 99},
    {.name = "resin", .colour = {228, 176, 68, 255}, .stack = 99},
    {.name = "fibre", .colour = {186, 176, 120, 255}, .stack = 999},
};

static_assert(std::size(kItems) == kItemCount, "every Item needs exactly one row in kItems");

inline constexpr const ItemDef &Def(Item item) { return kItems[ItemIndex(item)]; }

// How many of each item something gave up.
//
// The same shape as World::Yield and for the same reason: what a harvest is, is
// a count per kind, and the caller decides what the count is worth.
using Harvest = std::array<int, kItemCount>;
