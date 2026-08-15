#pragma once

#include "grid.h"
#include "raylib.h"

#include <algorithm>
#include <cmath>

// Contour extraction over a scalar field. The threshold is a parameter rather
// than a property of the field, so the same block can be drawn as rock at one
// level and as a liquid surface at another, and every element in the world is
// rendered by this one routine.
namespace marching_squares {

// Draws the isoline where the field crosses `threshold`.
void DrawContour(const Grid &grid, float threshold, Color color, float thickness = 2.0f);

// Fills the region where the field is above `threshold`.
void DrawFilled(const Grid &grid, float threshold, Color color);

// The field at any world position, interpolated between the four samples around
// it. Clamped at the border, so a square asking about its neighbour just past
// the last sample is answered from the edge rather than from nothing.
float SampleAt(const Grid &grid, Vector2 world);

// What one square knows about itself before it is painted.
//
// Everything here is read off the field the square is being drawn from, so a
// painter needs to know nothing about the world, the material or the lattice —
// which is what lets the same painter serve rock, soil and the surface of a
// pool.
struct Texel {
    // The square's own top-left corner, in world units.
    Vector2 at;

    // How far below the material's nearest exposed face this square sits, in
    // world pixels.
    //
    // Taken as the field's own value divided by the length of its gradient — the
    // distance to the zero set of a function is what the function is worth
    // divided by how fast it changes. Dividing locally rather than by a constant
    // is what makes this a real distance for a field that is not one: an ore's
    // noise and a brush stroke are shapes, not distances, and their gradients
    // differ by an order.
    //
    // The same reasoning, and the same trap, as the cave band widths in
    // terrain.cpp: with an average gradient the figure balloons wherever the
    // field happens to run flat.
    float depth;

    // Which way that face points, unit length, Y growing downward. A top face
    // points up, the belly of an overhang points down.
    //
    // The field increases into the material, so this is its gradient negated.
    Vector2 normal;
};

// The top of a filled region as it is actually drawn, from where the field says
// its edge is.
//
// Not the same number, and the difference is what anything standing on the ground
// has to be placed against. A square is filled when its centre is inside, so the
// top of the ground on screen is the first such row at or below the contour — up
// to most of a texel away from the contour itself, by a different amount in every
// column. Anything planted on the contour therefore floats above the ground it
// grew in.
//
// World-anchored, like the grid it quantises onto, so it is the same answer
// wherever the view happens to be and nothing crawls as it scrolls.
inline float DrawnTop(float crossing, float pixel) {
    return std::floor((crossing + pixel * 0.5f) / pixel) * pixel;
}

// A painter that gives every square the same colour.
//
// Declaring `uniform` is what tells the walk below that a lattice cell wholly
// inside the material can be laid down as one rectangle rather than a square at
// a time. It is a statement about the painter, not a request for speed: a walk
// that took that shortcut with a painter that varies would simply be drawing the
// wrong picture.
struct Flat {
    static constexpr bool uniform = true;

    Color colour;

    Color operator()(const Texel &) const { return colour; }
};

namespace detail {

// What one square is: empty, inside, or inside with a neighbour outside.
enum class Square { Empty, Fill, Edge };

} // namespace detail

// The region above `threshold`, rasterised onto square pixels instead of drawn
// as polygons, with every square coloured by `paint`.
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
//
// A template rather than a function pointer or a std::function, because this is
// called once per square across the whole of the visible world and an indirect
// call each time would be paid for a hundred thousand times a frame to buy
// nothing.
// The cells of a block that are worth walking, as an inclusive range of indices.
// A caller that already knows where the block has anything in it says so, and a
// block that is entirely empty is not walked at all. Left at its default the
// whole block is walked, which is what a caller that does not know has to do.
struct Cells {
    int firstCol = 0;
    int lastCol  = -1;
    int firstRow = 0;
    int lastRow  = -1;

    bool Empty() const { return lastCol < firstCol || lastRow < firstRow; }
};

// `clip` bounds the cells that are walked at all. A block is a whole chunk and
// the view cuts across it, so most of what a block describes is off screen and
// was being sampled and submitted anyway — five field reads and a rectangle per
// square, for squares nobody could see. Left empty, the whole block is drawn.
template <typename Painter>
void DrawPainted(const Grid &grid, float threshold, Painter paint, Color outline, float pixel,
                 Rectangle clip = {0.0f, 0.0f, 0.0f, 0.0f}, Cells cells = {}) {
    if (pixel <= 0.0f) return;

    // A painter that answers the same colour everywhere lets a cell the contour
    // does not pass through be laid down in one rectangle. Anything else has to
    // be asked about every square, because that is what having a texture means.
    constexpr bool uniform = requires { Painter::uniform; };

    const float step    = static_cast<float>(grid.Spacing());
    const bool outlined = outline.a > 0;

    int firstCol = std::max(0, cells.firstCol);
    int lastCol  = (cells.lastCol >= cells.firstCol) ? std::min(grid.Cols() - 2, cells.lastCol) : grid.Cols() - 2;
    int firstRow = std::max(0, cells.firstRow);
    int lastRow  = (cells.lastRow >= cells.firstRow) ? std::min(grid.Rows() - 2, cells.lastRow) : grid.Rows() - 2;

    if (clip.width > 0.0f && clip.height > 0.0f) {
        const Vector2 origin = grid.Origin();

        // A cell either side of the clip, because a square on the boundary reads
        // its neighbours to find its own normal and its outline.
        firstCol = std::max(firstCol, static_cast<int>(std::floor((clip.x - origin.x) / step)) - 1);
        lastCol  = std::min(lastCol, static_cast<int>(std::ceil((clip.x + clip.width - origin.x) / step)) + 1);
        firstRow = std::max(firstRow, static_cast<int>(std::floor((clip.y - origin.y) / step)) - 1);
        lastRow  = std::min(lastRow, static_cast<int>(std::ceil((clip.y + clip.height - origin.y) / step)) + 1);
    }

    for (int i = firstCol; i <= lastCol; i++) {
        for (int j = firstRow; j <= lastRow; j++) {
            const float a = grid.ValueAt(i, j);
            const float b = grid.ValueAt(i + 1, j);
            const float c = grid.ValueAt(i, j + 1);
            const float d = grid.ValueAt(i + 1, j + 1);

            const int corners = (a > threshold) + (b > threshold) + (c > threshold) + (d > threshold);

            if (corners == 0) continue;

            const Vector2 at = grid.PointAt(i, j);

            if constexpr (uniform) {
                if (corners == 4) {
                    const Color colour = paint(Texel{at, 0.0f, {0.0f, -1.0f}});

                    if (colour.a > 0) DrawRectangleV(at, {step, step}, colour);
                    continue;
                }
            }

            // Squares whose centre falls in this cell. Anchoring to the world
            // rather than to the cell is what stops the grid from shifting by a
            // fraction of a square at every cell border.
            const int m0 = static_cast<int>(std::floor(at.x / pixel));
            const int m1 = static_cast<int>(std::ceil((at.x + step) / pixel));
            const int n0 = static_cast<int>(std::floor(at.y / pixel));
            const int n1 = static_cast<int>(std::ceil((at.y + step) / pixel));

            for (int n = n0; n <= n1; n++) {
                const float y = (static_cast<float>(n) + 0.5f) * pixel;
                if (y < at.y || y >= at.y + step) continue;

                // Squares of a kind are drawn as one rectangle rather than one
                // each. A run costs the same as a single square to submit, and
                // a filled row of a cell is one call instead of a dozen.
                //
                // With a painter that varies, a run is broken by a change of
                // colour as well as by a change of kind — so this stays a
                // consequence of what was drawn rather than a shortcut that
                // changes it.
                detail::Square run = detail::Square::Empty;
                Color runColour    = BLANK;
                int from           = m0;

                // The neighbours to the left and to the right of a square are the
                // squares either side of it, and a square's own sample is exactly
                // the number its neighbours want. So the row is walked carrying
                // them rather than sampling the same point three times over.
                //
                // A filled square costs three field reads instead of five, and
                // this loop runs for every square of the visible world once per
                // material.
                float behind = 0.0f;
                float here   = 0.0f;

                bool haveHere   = false; // `here` already holds this square's sample.
                bool haveBehind = false; // `behind` holds the one to its left.

                for (int m = m0; m <= m1 + 1; m++) {
                    const float x = (static_cast<float>(m) + 0.5f) * pixel;

                    detail::Square square = detail::Square::Empty;
                    Color colour          = BLANK;

                    const bool covered = m <= m1 && x >= at.x && x < at.x + step;

                    if (covered) {
                        if (!haveHere) here = SampleAt(grid, {x, y}) - threshold;

                        haveHere = false;

                        if (here > 0.0f) {
                            // The four neighbours, which serve twice: they are
                            // the outline test, and they are the central
                            // difference the depth and the normal come from.
                            //
                            // The one to the right is taken at the position that
                            // square will sample itself at, so carrying it over
                            // hands it the very number it would have read.
                            const float right = SampleAt(grid, {(static_cast<float>(m) + 1.5f) * pixel, y}) - threshold;
                            const float left  = haveBehind ? behind : (SampleAt(grid, {x - pixel, y}) - threshold);
                            const float above = SampleAt(grid, {x, y - pixel}) - threshold;
                            const float below = SampleAt(grid, {x, y + pixel}) - threshold;

                            const float gx = (right - left) / (2.0f * pixel);
                            const float gy = (below - above) / (2.0f * pixel);

                            const float slope = std::sqrt(gx * gx + gy * gy);

                            Texel texel{{static_cast<float>(m) * pixel, static_cast<float>(n) * pixel}, 0.0f,
                                        {0.0f, -1.0f}};

                            // Deep inside a saturated field the gradient is
                            // nothing and there is no face to be near, which is
                            // exactly what a depth of infinity says. Left at the
                            // default upward normal, since a face that does not
                            // exist has no direction either.
                            if (slope > 1e-6f) {
                                texel.depth  = here / slope;
                                texel.normal = {-gx / slope, -gy / slope};
                            } else {
                                texel.depth = 1e9f;
                            }

                            const bool exposed =
                                outlined && (left <= 0.0f || right <= 0.0f || above <= 0.0f || below <= 0.0f);

                            square = exposed ? detail::Square::Edge : detail::Square::Fill;
                            colour = exposed ? outline : paint(texel);

                            // The sample to the right belongs to the next square.
                            behind   = here;
                            here     = right;
                            haveHere = true;
                        } else {
                            // Nothing was read to the right of an empty square,
                            // so the next one takes its own — but its neighbour
                            // to the left is this square, which is known.
                            behind = here;
                        }

                        haveBehind = true;
                    } else {
                        // Outside the cell, so the run of squares this row is
                        // carrying has been broken.
                        haveBehind = false;
                        haveHere   = false;
                    }

                    const bool same = (square == run) && (square == detail::Square::Empty || ColorIsEqual(colour, runColour));

                    if (same) continue;

                    if (run != detail::Square::Empty && runColour.a > 0) {
                        DrawRectangleV({static_cast<float>(from) * pixel, static_cast<float>(n) * pixel},
                                       {static_cast<float>(m - from) * pixel, pixel}, runColour);
                    }

                    run       = square;
                    runColour = colour;
                    from      = m;
                }
            }
        }
    }
}

// The same, in one flat colour.
inline void DrawPixelated(const Grid &grid, float threshold, Color fill, Color outline, float pixel) {
    if (fill.a == 0 && outline.a == 0) return;

    DrawPainted(grid, threshold, Flat{.colour = fill}, outline, pixel);
}

// Debug overlay: one square per sample, coloured by which side of the threshold
// it falls on.
void DrawVertices(const Grid &grid, float threshold, float size, Color filledColor, Color emptyColor);

} // namespace marching_squares
