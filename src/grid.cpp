#include "grid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

Grid::Grid(Vector2 origin, int cols, int rows, int spacing)
    : origin_(origin), cols_(cols), rows_(rows), spacing_(spacing),
      values_(static_cast<std::size_t>(cols) * rows, 0.0f) {}

Rectangle Grid::Bounds() const {
    return {origin_.x, origin_.y, static_cast<float>((cols_ - 1) * spacing_),
            static_cast<float>((rows_ - 1) * spacing_)};
}

void Grid::ToLocal(Vector2 world, int &outI, int &outJ) const {
    outI = static_cast<int>(std::lround((world.x - origin_.x) / spacing_));
    outJ = static_cast<int>(std::lround((world.y - origin_.y) / spacing_));
}

void Grid::Clear() {
    std::fill(values_.begin(), values_.end(), 0.0f);
}
