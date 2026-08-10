#include "marching_squares.h"

namespace marching_squares {
namespace {

Vector2 Midpoint(Vector2 a, Vector2 b) {
    return {(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
}

// Every cell has four corners that are either solid or empty, giving 16
// possible configurations. The state index is built by reading the corners
// clockwise from the top left (a=8, b=4, c=2, d=1), and each case selects the
// edges the contour crosses. Corner values are binary, so the crossing always
// falls at the edge midpoint and no interpolation is involved.
void DrawCell(Vector2 a, Vector2 b, Vector2 c, Vector2 d, bool va, bool vb, bool vc, bool vd, Color color,
              float thickness) {
    const int state = (va << 3) | (vb << 2) | (vc << 1) | (vd);
    if (state == 0 || state == 15) return; // Cell entirely outside or inside.

    const Vector2 top    = Midpoint(a, b);
    const Vector2 right  = Midpoint(b, c);
    const Vector2 bottom = Midpoint(d, c);
    const Vector2 left   = Midpoint(a, d);

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

    // Ambiguous cases: the two solid corners are diagonal, so two different
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

void DrawContour(const Grid &grid, Color color, float thickness) {
    // One cell per pair of neighbouring columns and rows, hence the -1 bounds.
    for (int i = 0; i < grid.Cols() - 1; i++) {
        for (int j = 0; j < grid.Rows() - 1; j++) {
            DrawCell(grid.PointAt(i, j),         // top left
                     grid.PointAt(i + 1, j),     // top right
                     grid.PointAt(i + 1, j + 1), // bottom right
                     grid.PointAt(i, j + 1),     // bottom left
                     grid.IsSolid(i, j), grid.IsSolid(i + 1, j), grid.IsSolid(i + 1, j + 1), grid.IsSolid(i, j + 1),
                     color, thickness);
        }
    }
}

void DrawVertices(const Grid &grid, float size, Color solidColor, Color emptyColor) {
    const Vector2 origin = {size / 2.0f, size / 2.0f};

    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            const Vector2 p      = grid.PointAt(i, j);
            const Rectangle rect = {p.x, p.y, size, size};

            DrawRectanglePro(rect, origin, 0.0f, grid.IsSolid(i, j) ? solidColor : emptyColor);
        }
    }
}

} // namespace marching_squares
