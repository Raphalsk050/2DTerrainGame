#pragma once

#include "grid.h"
#include "raylib.h"

// Contour extraction over a scalar field. The threshold is a parameter rather
// than a property of the field, so the same block can be drawn as rock at one
// level and as a liquid surface at another, and every element in the world is
// rendered by this one routine.
namespace marching_squares {

// Draws the isoline where the field crosses `threshold`.
void DrawContour(const Grid &grid, float threshold, Color color, float thickness = 2.0f);

// Debug overlay: one square per sample, coloured by which side of the threshold
// it falls on.
void DrawVertices(const Grid &grid, float threshold, float size, Color filledColor, Color emptyColor);

} // namespace marching_squares
