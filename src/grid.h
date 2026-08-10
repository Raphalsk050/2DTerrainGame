#pragma once

#include "raylib.h"

#include <cstdint>
#include <vector>

// Vertex grid backing the marching squares contour: the screen position of each
// vertex and whether it lies inside the terrain.
//
// Storage is column-major, so the index of (column i, row j) is i*rows + j. All
// index arithmetic goes through Index().
class Grid {
public:
    Grid(int cols, int rows, int spacing);

    int Cols() const { return cols_; }
    int Rows() const { return rows_; }
    int Spacing() const { return spacing_; }

    int Index(int i, int j) const { return i * rows_ + j; }
    bool InBounds(int i, int j) const { return i >= 0 && i < cols_ && j >= 0 && j < rows_; }

    Vector2 PointAt(int i, int j) const { return points_[Index(i, j)]; }

    bool IsSolid(int i, int j) const { return solid_[Index(i, j)] != 0; }
    void SetSolid(int i, int j, bool solid) { solid_[Index(i, j)] = solid ? 1 : 0; }

    // Marks every vertex as empty.
    void Clear();

    // Maps a screen position to the nearest vertex in constant time. Returns
    // false when the position falls outside the grid or farther than `radius`
    // from the nearest vertex, leaving outI and outJ untouched.
    bool PickVertex(Vector2 position, float radius, int &outI, int &outJ) const;

private:
    int cols_;
    int rows_;
    int spacing_;

    std::vector<Vector2> points_;

    // std::uint8_t rather than bool: std::vector<bool> is a bit-packed
    // specialisation that does not hand out real references to its elements.
    std::vector<std::uint8_t> solid_;
};
