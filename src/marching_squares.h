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

// Fills the region where the field is above `threshold`.
void DrawFilled(const Grid &grid, float threshold, Color color);

// The same region, rasterised onto square pixels instead of drawn as polygons.
//
// The shape is still the field's: a surface halfway through a cell is still
// halfway through it, because the field is interpolated between its samples
// exactly as the contour does. Only the drawing is quantised, so the edge comes
// out as a staircase of squares rather than as a smooth line.
//
// `pixel` is the side of one square in world units, and is independent of the
// lattice. Finer than the lattice, the staircase still follows the contour
// faithfully; coarser, it starts to coarsen the shape itself.
//
// The pixel grid is anchored to the world rather than to the block, so that
// neighbouring blocks agree along their shared border and the squares do not
// shift as the world scrolls.
//
// `outline` is given to the squares that have a neighbour outside, which is
// what the contour line becomes once there is no line to draw. Passing a
// transparent colour leaves the fill unbroken.
void DrawPixelated(const Grid &grid, float threshold, Color fill, Color outline, float pixel);

// Debug overlay: one square per sample, coloured by which side of the threshold
// it falls on.
void DrawVertices(const Grid &grid, float threshold, float size, Color filledColor, Color emptyColor);

} // namespace marching_squares
