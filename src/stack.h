#pragma once

#include "config.h"
#include "element.h"
#include "item.h"
#include "picture.h"

#include <cstdint>

// What one slot holds, and what a block is worth.
//
// The world keeps two tables of things and neither folds into the other — the
// head of item.h sets out why, and nothing here disturbs that. What changes is
// that a slot has to be able to hold either one, since a player carries the
// stone they dug and the wood they felled in the same nine places. So what a
// slot holds is written down as: which table, which row of it, and how many.

enum class Holds : std::uint8_t {
    Nothing,

    // A row of kElements, carried as blocks of the material.
    Material,

    // A row of kItems.
    Item,
};

// One block, in world pixels.
//
// Sixteen, which is Minecraft's, and which the world was already written
// against without ever saying so: the character is twelve wide and twenty-six
// tall, a terrace riser is twenty-four, and a mature tree is five characters.
inline constexpr float kBlockSide = 16.0f;

// What one block costs, in lattice vertices.
//
// The ground here is a field sampled every config::kResolution pixels and not a
// stack of cubes, so digging returns a count of vertices cleared and there is no
// block anywhere in the world to count instead. This is the rate between the
// two, and it is simply an area: a block is as much material as fills the square
// a block would have stood in.
//
// It comes out a little over seven, and the fraction is the point. A brush
// stroke crosses part of a block far more often than a whole one, so the
// remainder has to be carried from one stroke to the next rather than rounded
// away — see Editor, which keeps it.
inline constexpr float kVerticesPerBlock = (kBlockSide / config::kResolution) * (kBlockSide / config::kResolution);

struct Stack {
    Holds holds = Holds::Nothing;

    // The row, in whichever table `holds` names.
    //
    // One byte, because both tables are an order of magnitude smaller than that
    // and a slot is a thing there are thirty-six of before anything is in them.
    std::uint8_t what = 0;

    int count = 0;

    bool Empty() const { return holds == Holds::Nothing || count <= 0; }

    Element AsElement() const { return static_cast<Element>(what); }
    Item AsItem() const { return static_cast<Item>(what); }

    // Whether two stacks are the same thing, without regard to how many.
    //
    // What decides whether one may be poured into the other, so an empty slot is
    // like nothing at all — including like another empty slot, which would
    // otherwise merge two holes into one.
    bool Alike(const Stack &other) const {
        if (Empty() || other.Empty()) return false;

        return holds == other.holds && what == other.what;
    }

    const char *Name() const {
        switch (holds) {
        case Holds::Material: return kElements[what].name;
        case Holds::Item: return kItems[what].name;
        case Holds::Nothing: break;
        }

        return "";
    }

    // How many of it one slot will hold.
    int Limit() const {
        switch (holds) {
        case Holds::Material: return kElements[what].stack;
        case Holds::Item: return kItems[what].stack;
        case Holds::Nothing: break;
        }

        return 0;
    }

    // How many more of it this slot could take.
    int Room() const { return Empty() ? 0 : Limit() - count; }
};

inline constexpr Stack BlocksOf(Element element, int count) {
    return {.holds = Holds::Material, .what = static_cast<std::uint8_t>(ElementIndex(element)), .count = count};
}

inline constexpr Stack ItemsOf(Item item, int count) {
    return {.holds = Holds::Item, .what = static_cast<std::uint8_t>(ItemIndex(item)), .count = count};
}

// The picture a stack is drawn from.
//
// A free function rather than a member so that it reads the same as the one
// beside it in element.h, and so that the two tables keep answering for their
// own rows.
inline Picture PictureOf(const Stack &stack) {
    switch (stack.holds) {
    case Holds::Material: return PictureOf(kElements[stack.what]);
    case Holds::Item: return kItems[stack.what].picture;
    case Holds::Nothing: break;
    }

    return {};
}
