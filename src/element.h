#pragma once

#include "raylib.h"

#include <cstddef>

// Materials the world is made of.
//
// Each one is a scalar field over the same lattice and is drawn by the same
// marching squares routine, so a new material means a new field and its rules,
// never a new renderer.
enum class Element { Rock, Water, Count };

inline constexpr std::size_t kElementCount = static_cast<std::size_t>(Element::Count);

inline constexpr std::size_t ElementIndex(Element element) {
    return static_cast<std::size_t>(element);
}

struct ElementStyle {
    const char *name;
    float threshold; // Field value at which the surface sits.
    Color fill;      // Interior colour. Zero alpha leaves the element unfilled.
    Color contour;
};

inline const ElementStyle kElementStyles[kElementCount] = {
    // Rock is filled and outlined. An outline on its own leaves the ground and
    // the sky the same colour, so nothing reads as solid.
    {"rock", 0.45f, {105, 115, 130, 255}, {0, 82, 172, 255}},

    // Water is drawn by the same marching squares routine as everything else.
    // Its field is clamped against the rock before drawing, so the two contours
    // meet exactly instead of each interpolating to its own answer.
    //
    // The threshold is low on purpose. The field holds mass, not height, and a
    // lattice cell is the smallest thing that can be drawn: a stream carrying a
    // third of a unit is physically a third of a cell across, which the grid
    // cannot express. Only two outcomes are available, nothing or a whole cell.
    //
    // A high threshold picks nothing, and running water breaks into disconnected
    // pieces or vanishes. This picks the whole cell, so thin liquid reads one
    // cell thick and a pool's partly filled top row reads as full. Continuity is
    // worth more here than a fraction of a cell of depth.
    {"water", 0.15f, {102, 191, 255, 255}, {56, 152, 236, 255}},
};

inline const ElementStyle &StyleOf(Element element) {
    return kElementStyles[ElementIndex(element)];
}
