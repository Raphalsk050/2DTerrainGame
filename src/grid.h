#pragma once

#include "raylib.h"

#include <cstdint>
#include <vector>

// A rectangular block of terrain vertices anchored at a world position.
//
// Storage is column-major, so the index of (column i, row j) is i*rows + j. All
// index arithmetic goes through Index().
//
// Vertex positions are derived from the origin and the spacing rather than
// stored, since a world made of many blocks would otherwise hold a redundant
// copy of a regular lattice.
class Grid {
public:
    Grid(Vector2 origin, int cols, int rows, int spacing);

    Vector2 Origin() const { return origin_; }
    int Cols() const { return cols_; }
    int Rows() const { return rows_; }
    int Spacing() const { return spacing_; }

    int Index(int i, int j) const { return i * rows_ + j; }
    bool InBounds(int i, int j) const { return i >= 0 && i < cols_ && j >= 0 && j < rows_; }

    Vector2 PointAt(int i, int j) const {
        return {origin_.x + static_cast<float>(i * spacing_), origin_.y + static_cast<float>(j * spacing_)};
    }

    bool IsSolid(int i, int j) const { return solid_[Index(i, j)] != 0; }
    void SetSolid(int i, int j, bool solid) { solid_[Index(i, j)] = solid ? 1 : 0; }

    // World region this block covers, from its first to its last vertex.
    Rectangle Bounds() const;

    // Index of the vertex nearest to a world position. The result may fall
    // outside the block, which InBounds reports.
    void ToLocal(Vector2 world, int &outI, int &outJ) const;

    void Clear();

private:
    Vector2 origin_;
    int cols_;
    int rows_;
    int spacing_;

    // std::uint8_t rather than bool: std::vector<bool> is a bit-packed
    // specialisation that does not hand out real references to its elements.
    std::vector<std::uint8_t> solid_;
};
