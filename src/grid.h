#pragma once

#include "raylib.h"

#include <vector>

// A rectangular block of scalar field samples anchored at a world position.
//
// Values are continuous rather than a solid flag. Consumers decide what counts
// as filled by comparing against a threshold, which lets one block describe
// rock, water or any other element, and lets the contour cross an edge at the
// exact point where the field meets the threshold instead of always at the
// midpoint.
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

    float ValueAt(int i, int j) const { return values_[Index(i, j)]; }
    void SetValue(int i, int j, float value) { values_[Index(i, j)] = value; }

    // World region this block covers, from its first to its last vertex.
    Rectangle Bounds() const;

    // Index of the vertex nearest to a world position. The result may fall
    // outside the block, which InBounds reports.
    void ToLocal(Vector2 world, int &outI, int &outJ) const;

    // Resets every sample to zero, which every threshold reads as empty.
    void Clear();

private:
    Vector2 origin_;
    int cols_;
    int rows_;
    int spacing_;

    std::vector<float> values_;
};
