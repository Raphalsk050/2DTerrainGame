#pragma once

#include "grid.h"
#include "raylib.h"

namespace marching_squares {

// Draws the contour separating solid vertices from empty ones.
void DrawContour(const Grid &grid, Color color, float thickness = 2.0f);

// Debug overlay: one square per vertex, coloured by its state.
void DrawVertices(const Grid &grid, float size, Color solidColor, Color emptyColor);

} // namespace marching_squares
