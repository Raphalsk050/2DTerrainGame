#pragma once

#include "element.h"
#include "hotbar.h"
#include "raylib.h"
#include "world.h"

#include <optional>

// The mouse brush, and the switches that decide what it does.
//
// The brush has one button and a mode, rather than one button per action. A
// button that places and another that removes reads well in a sentence and
// badly in the hand: the two are used in long alternating stretches, not one
// click at a time, and swapping hands mid-stroke to erase costs more than the
// keystroke that would have changed the mode. The mode is also visible, which
// a button is not, so what the next click will do can be read off the screen
// instead of remembered.
class Editor {
public:
    enum class Mode {
        Place, // Fills the brush with the selected material.
        Dig,   // Empties the brush of whatever is in it.
    };

    // Reads the keyboard and the mouse and applies the brush. Returns nothing:
    // what was dug out is accumulated instead, since a caller that wanted the
    // yield of one frame would have to add it up anyway.
    void Update(World &world, const Hotbar &hotbar, const Camera2D &camera);

    // Outline of the area the next click affects, drawn in world space so it
    // sits over the material it is about to change.
    void DrawCursor(const Hotbar &hotbar, const Camera2D &camera) const;

    Mode CurrentMode() const { return mode_; }
    const char *ModeName() const { return (mode_ == Mode::Place) ? "place" : "dig"; }
    float Radius() const { return radius_; }

    // Everything the brush has dug out, counted in lattice vertices per
    // material. The world reports each edit; keeping the running total here is
    // what a pickaxe eventually fills an inventory with.
    const World::Yield &Collected() const { return collected_; }

    // What the cursor is currently over, or nothing where the space is open.
    // Held from the last update so the head-up display and the cursor agree on
    // one answer rather than each asking the world separately.
    std::optional<Element> Under() const { return under_; }

private:
    // Brush sizes in pixels. Bounded at the small end by the lattice, since a
    // brush narrower than the spacing between vertices covers none of them and
    // silently does nothing.
    static constexpr float kMinRadius  = 8.0f;
    static constexpr float kMaxRadius  = 64.0f;
    static constexpr float kRadiusStep = 4.0f;

    Mode mode_     = Mode::Place;
    float radius_  = 16.0f;
    World::Yield collected_{};

    std::optional<Element> under_;
};
