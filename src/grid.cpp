#include "grid.h"

#include <algorithm>
#include <cstddef>

Grid::Grid(int cols, int rows, int spacing)
    : cols_(cols), rows_(rows), spacing_(spacing), points_(static_cast<std::size_t>(cols) * rows),
      solid_(static_cast<std::size_t>(cols) * rows, 0) {
    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            points_[Index(i, j)] = {static_cast<float>(i * spacing_), static_cast<float>(j * spacing_)};
        }
    }
}

void Grid::Clear() {
    std::fill(solid_.begin(), solid_.end(), 0);
}

bool Grid::PickVertex(Vector2 position, float radius, int &outI, int &outJ) const {
    // Adding half the spacing before truncating rounds to the nearest vertex
    // instead of the one above and to the left.
    const int i = static_cast<int>((position.x + spacing_ / 2.0f) / spacing_);
    const int j = static_cast<int>((position.y + spacing_ / 2.0f) / spacing_);

    if (!InBounds(i, j)) return false;
    if (!CheckCollisionPointCircle(position, PointAt(i, j), radius)) return false;

    outI = i;
    outJ = j;
    return true;
}
