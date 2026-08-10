#include "world.h"

#include "marching_squares.h"

#include <cmath>

namespace {

// Chunks are kept this many chunks beyond the view before being released, so
// that walking back and forth across a border does not regenerate them every
// frame.
constexpr int kKeepMargin = 2;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

} // namespace

World::World(const terrain::Settings &settings, int spacing) : settings_(settings), spacing_(spacing) {}

std::int64_t World::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

void World::ToChunk(Vector2 world, int &outCx, int &outCy) const {
    const int span = kChunkCells * spacing_;

    outCx = FloorDiv(static_cast<int>(std::floor(world.x)), span);
    outCy = FloorDiv(static_cast<int>(std::floor(world.y)), span);
}

const World::Chunk *World::Find(int cx, int cy) const {
    const auto it = chunks_.find(Key(cx, cy));
    return (it != chunks_.end()) ? &it->second : nullptr;
}

World::Chunk &World::Emplace(int cx, int cy) {
    const Vector2 origin = {cx * ChunkSpan(), cy * ChunkSpan()};

    Chunk chunk{Grid(origin, kChunkVertices, kChunkVertices, spacing_), false};
    terrain::Fill(chunk.grid, settings_);

    return chunks_.emplace(Key(cx, cy), std::move(chunk)).first->second;
}

void World::Update(Rectangle view) {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            if (Find(cx, cy) == nullptr) Emplace(cx, cy);
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (it->second.edited) {
            ++it;
            continue;
        }

        int cx = 0;
        int cy = 0;
        ToChunk({it->second.grid.Origin().x, it->second.grid.Origin().y}, cx, cy);

        const bool far = cx < minCx - kKeepMargin || cx > maxCx + kKeepMargin || cy < minCy - kKeepMargin ||
                         cy > maxCy + kKeepMargin;

        it = far ? chunks_.erase(it) : std::next(it);
    }
}

void World::Reset() {
    chunks_.clear();
}

bool World::IsSolidAt(Vector2 world) const {
    // Snapped to the vertex lattice before anything else. Resident chunks can
    // only answer at their vertices, so sampling the noise at the exact
    // position would make terrain shift by up to half a cell the moment a
    // chunk is released.
    const float step     = static_cast<float>(spacing_);
    const Vector2 vertex = {std::round(world.x / step) * step, std::round(world.y / step) * step};

    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    const Chunk *chunk = Find(cx, cy);
    if (chunk == nullptr) return terrain::IsSolid(vertex, settings_);

    int i = 0;
    int j = 0;
    chunk->grid.ToLocal(vertex, i, j);

    if (!chunk->grid.InBounds(i, j)) return terrain::IsSolid(vertex, settings_);

    return chunk->grid.ValueAt(i, j) > settings_.threshold;
}

bool World::OverlapsSolid(Rectangle rect) const {
    const float step = static_cast<float>(spacing_);

    // Vertices occupy a square of one spacing centred on themselves, so the
    // rectangle can only be met by vertices within half a spacing of its edges.
    const float left   = rect.x;
    const float right  = rect.x + rect.width;
    const float top    = rect.y;
    const float bottom = rect.y + rect.height;

    for (float x = std::floor(left / step) * step; x <= right + step; x += step) {
        for (float y = std::floor(top / step) * step; y <= bottom + step; y += step) {
            // Only vertices whose square actually meets the rectangle count.
            if (x + step / 2.0f < left || x - step / 2.0f > right) continue;
            if (y + step / 2.0f < top || y - step / 2.0f > bottom) continue;

            if (IsSolidAt({x, y})) return true;
        }
    }

    return false;
}

void World::Paint(Vector2 world, float radius, bool solid) {
    const Rectangle brush = {world.x - radius, world.y - radius, radius * 2.0f, radius * 2.0f};

    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({brush.x, brush.y}, minCx, minCy);
    ToChunk({brush.x + brush.width, brush.y + brush.height}, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const auto it = chunks_.find(Key(cx, cy));
            if (it == chunks_.end()) continue;

            Chunk &chunk = it->second;

            for (int i = 0; i < chunk.grid.Cols(); i++) {
                for (int j = 0; j < chunk.grid.Rows(); j++) {
                    if (!CheckCollisionPointCircle(chunk.grid.PointAt(i, j), world, radius)) continue;

                    // Painted samples are pinned to the extremes of the range
                    // rather than nudged, so a brush stroke reads as a definite
                    // edit and not as a faint gradient.
                    chunk.grid.SetValue(i, j, solid ? 1.0f : 0.0f);
                    chunk.edited = true;
                }
            }
        }
    }
}

void World::Draw(Rectangle view, float vertexSize, Color solidColor, Color emptyColor, Color contourColor) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const Chunk *chunk = Find(cx, cy);
            if (chunk == nullptr) continue;

            marching_squares::DrawVertices(chunk->grid, settings_.threshold, vertexSize, solidColor, emptyColor);
            marching_squares::DrawContour(chunk->grid, settings_.threshold, contourColor);
        }
    }
}
