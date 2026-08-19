#include "world/marching_squares.h"

#include <algorithm>
#include <cmath>

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

namespace {

// Signed area under the shoelace formula. Raylib emits its own quads as
// top-left, bottom-left, bottom-right, top-right, which is negative in screen
// coordinates, where y grows downwards. Backface culling is enabled, so a
// polygon of the opposite sign would be discarded rather than drawn.
float SignedArea(const Vector2 *points, int count) {
    float sum = 0.0f;
    for (int k = 0; k < count; k++) {
        const Vector2 &p = points[k];
        const Vector2 &q = points[(k + 1) % count];
        sum += p.x * q.y - q.x * p.y;
    }
    return sum / 2.0f;
}

// Polygon of the filled part of one cell.
//
// The cell boundary is walked in order, emitting each corner that is above the
// threshold and each crossing on an edge that straddles it. Because every
// vertex produced lies on the boundary of the cell and they are produced in
// boundary order, the result is always convex and can be drawn as a fan. The
// two ambiguous configurations come out as hexagons whose edges coincide with
// the lines DrawCell draws, so filled and outlined shapes agree.
int FilledPolygon(Vector2 a, Vector2 b, Vector2 c, Vector2 d, float va, float vb, float vc, float vd, float threshold,
                  Vector2 *out) {
    const Vector2 corner[4] = {a, b, c, d};
    const float value[4]    = {va, vb, vc, vd};

    int count = 0;
    for (int k = 0; k < 4; k++) {
        const int next = (k + 1) % 4;

        if (value[k] > threshold) out[count++] = corner[k];

        if ((value[k] > threshold) != (value[next] > threshold)) {
            out[count++] = Crossing(corner[k], corner[next], value[k], value[next], threshold);
        }
    }

    return count;
}

} // namespace

void DrawFilled(const Grid &grid, float threshold, Color color) {
    if (color.a == 0) return;

    for (int i = 0; i < grid.Cols() - 1; i++) {
        for (int j = 0; j < grid.Rows() - 1; j++) {
            Vector2 polygon[8];

            const int count = FilledPolygon(grid.PointAt(i, j), grid.PointAt(i + 1, j), grid.PointAt(i + 1, j + 1),
                                            grid.PointAt(i, j + 1), grid.ValueAt(i, j), grid.ValueAt(i + 1, j),
                                            grid.ValueAt(i + 1, j + 1), grid.ValueAt(i, j + 1), threshold, polygon);

            if (count < 3) continue;

            if (SignedArea(polygon, count) > 0.0f) std::reverse(polygon, polygon + count);

            DrawTriangleFan(polygon, count, color);
        }
    }
}

float SampleAt(const Grid &grid, Vector2 world) {
    if (grid.Cols() < 2 || grid.Rows() < 2) return 0.0f;

    const float step = static_cast<float>(grid.Spacing());

    const float u = (world.x - grid.Origin().x) / step;
    const float v = (world.y - grid.Origin().y) / step;

    const int i = std::clamp(static_cast<int>(std::floor(u)), 0, grid.Cols() - 2);
    const int j = std::clamp(static_cast<int>(std::floor(v)), 0, grid.Rows() - 2);

    const float fx = std::clamp(u - static_cast<float>(i), 0.0f, 1.0f);
    const float fy = std::clamp(v - static_cast<float>(j), 0.0f, 1.0f);

    return grid.ValueAt(i, j) * (1.0f - fx) * (1.0f - fy) + grid.ValueAt(i + 1, j) * fx * (1.0f - fy) +
           grid.ValueAt(i, j + 1) * (1.0f - fx) * fy + grid.ValueAt(i + 1, j + 1) * fx * fy;
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
