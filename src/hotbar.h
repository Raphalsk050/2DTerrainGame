#pragma once

#include "element.h"
#include "raylib.h"

#include <array>

// Row of slots along the bottom of the screen, one per element, selecting what
// the mouse places.
//
// Drawn in screen space, outside the camera transform, so it stays put while
// the world scrolls underneath.
class Hotbar {
public:
    // Number keys select a slot directly; the wheel steps through them.
    void Update();

    // `collected` is how much of each material has been dug out, shown on the
    // slot it belongs to. The bar is where a player already looks for a
    // material, so it is where the amount of it belongs too.
    void Draw(const std::array<int, kElementCount> &collected) const;

    Element Selected() const { return static_cast<Element>(selected_); }

    // True when a screen position lies on the bar. Callers test this before
    // acting on a click, so selecting a slot does not also paint the world
    // behind it.
    bool Contains(Vector2 screen) const;

private:
    // The size a slot is drawn at when there is room for it, and the size below
    // which its label stops being readable and the swatch stops being a swatch.
    static constexpr float kSlotSize    = 52.0f;
    static constexpr float kMinSlotSize = 28.0f;

    static constexpr float kPadding = 6.0f;
    static constexpr float kMargin  = 12.0f;

    // What a slot is actually drawn at now.
    //
    // The bar holds one slot per material, and there are enough of them that at
    // full size it is wider than the smallest window the game may be dragged to.
    // Shrinking is the answer rather than scrolling or wrapping: a bar is read at
    // a glance, and a slot that has to be found before it can be read is not a
    // bar.
    float SlotSize() const;

    Rectangle BarBounds() const;
    Rectangle SlotBounds(int slot) const;

    int selected_ = 0;
};
