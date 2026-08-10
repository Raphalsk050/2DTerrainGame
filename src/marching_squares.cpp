#include "marching_squares.h"

#include <algorithm>

namespace marching_squares {
namespace {

// Point along the edge from a to b where the field reaches `threshold`.
//
// This is what separates a contour that follows the data from one made of
// 45-degree facets: a surface sitting halfway through a cell is drawn halfway
// through it, not snapped to the cell centre.
Vector2 Crossing(Vector2 a, Vector2 b, float va, float vb, float threshold) {
    // Equal endpoints lie on the same side of the threshold, so this edge is
    // not crossed and the result is never used; the midpoint keeps the value
    // finite rather than dividing by zero.
    const float span = vb - va;
    const float t    = (span != 0.0f) ? std::clamp((threshold - va) / span, 0.0f, 1.0f) : 0.5f;

    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// Every cell has four corners that are either above or below the threshold,
// giving 16 possible configurations. The state index is built by reading the
// corners clockwise from the top left (a=8, b=4, c=2, d=1), and each case
// selects the edges the contour crosses.
void DrawCell(Vector2 a, Vector2 b, Vector2 c, Vector2 d, float va, float vb, float vc, float vd, float threshold,
              Color color, float thickness) {
    const int state = ((va > threshold) << 3) | ((vb > threshold) << 2) | ((vc > threshold) << 1) | (vd > threshold);
    if (state == 0 || state == 15) return; // Cell entirely outside or inside.

    const Vector2 top    = Crossing(a, b, va, vb, threshold);
    const Vector2 right  = Crossing(b, c, vb, vc, threshold);
    const Vector2 bottom = Crossing(d, c, vd, vc, threshold);
    const Vector2 left   = Crossing(a, d, va, vd, threshold);

    switch (state) {
    case 1:
    case 14: DrawLineEx(left, bottom, thickness, color); break;
    case 2:
    case 13: DrawLineEx(bottom, right, thickness, color); break;
    case 3:
    case 12: DrawLineEx(left, right, thickness, color); break;
    case 4:
    case 11: DrawLineEx(top, right, thickness, color); break;
    case 6:
    case 9: DrawLineEx(top, bottom, thickness, color); break;
    case 7:
    case 8: DrawLineEx(left, top, thickness, color); break;

    // Ambiguous cases: the two filled corners are diagonal, so two different
    // connections are possible. The convention here is to always separate the
    // diagonals.
    case 5:
        DrawLineEx(left, top, thickness, color);
        DrawLineEx(bottom, right, thickness, color);
        break;
    case 10:
        DrawLineEx(left, bottom, thickness, color);
        DrawLineEx(top, right, thickness, color);
        break;
    }
}

} // namespace

void DrawContour(const Grid &grid, float threshold, Color color, float thickness) {
    // One cell per pair of neighbouring columns and rows, hence the -1 bounds.
    for (int i = 0; i < grid.Cols() - 1; i++) {
        for (int j = 0; j < grid.Rows() - 1; j++) {
            DrawCell(grid.PointAt(i, j),         // top left
                     grid.PointAt(i + 1, j),     // top right
                     grid.PointAt(i + 1, j + 1), // bottom right
                     grid.PointAt(i, j + 1),     // bottom left
                     grid.ValueAt(i, j), grid.ValueAt(i + 1, j), grid.ValueAt(i + 1, j + 1), grid.ValueAt(i, j + 1),
                     threshold, color, thickness);
        }
    }
}

void DrawVertices(const Grid &grid, float threshold, float size, Color filledColor, Color emptyColor) {
    const Vector2 origin = {size / 2.0f, size / 2.0f};

    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            const Vector2 p      = grid.PointAt(i, j);
            const Rectangle rect = {p.x, p.y, size, size};

            DrawRectanglePro(rect, origin, 0.0f, (grid.ValueAt(i, j) > threshold) ? filledColor : emptyColor);
        }
    }
}

} // namespace marching_squares
