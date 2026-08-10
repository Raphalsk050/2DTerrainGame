#pragma once

#include "grid.h"
#include "raylib.h"
#include "terrain.h"

#include <cstdint>
#include <unordered_map>

// Unbounded terrain, materialised in square chunks around a moving focus.
//
// Chunks are generated when they come near the view and released when they
// leave it. Because terrain::Sample is a pure function of world position, a
// chunk regenerated later is identical to the one that was released, and no
// state has to survive in between.
//
// Chunks the player has edited are the exception: their contents no longer
// follow from the noise, so they are pinned in memory instead of released.
class World {
public:
    // Cells along one chunk side.
    static constexpr int kChunkCells = 32;

    // One vertex more than cells, so that adjacent chunks share their border
    // column and row. Without the overlap the cell between two chunks belongs
    // to neither, and the contour breaks at every chunk seam.
    static constexpr int kChunkVertices = kChunkCells + 1;

    World(const terrain::Settings &settings, int spacing);

    // Generates the chunks covering `view` plus a margin and releases those
    // that fell well outside it.
    void Update(Rectangle view);

    // Discards every chunk, including edited ones, so the world regenerates
    // from the noise alone.
    void Reset();

    // Terrain queries in world space, answered by the vertex nearest to the
    // position. Positions in chunks that are not resident fall back to the
    // noise function at that same vertex, so collision stays correct beyond
    // the loaded area and does not change as chunks come and go.
    bool IsSolidAt(Vector2 world) const;
    bool OverlapsSolid(Rectangle rect) const;

    // Sets every vertex within `radius` of a world position, across chunk
    // borders. Only resident chunks are affected.
    void Paint(Vector2 world, float radius, bool solid);

    void Draw(Rectangle view, float vertexSize, Color solidColor, Color emptyColor, Color contourColor) const;

    int Spacing() const { return spacing_; }
    int ResidentChunks() const { return static_cast<int>(chunks_.size()); }

private:
    struct Chunk {
        Grid grid;
        bool edited = false;
    };

    // World span of one chunk, in pixels.
    float ChunkSpan() const { return static_cast<float>(kChunkCells * spacing_); }

    static std::int64_t Key(int cx, int cy);

    // Chunk coordinate containing a world position. Floor division, so it stays
    // correct left of and above the origin where truncation would round the
    // wrong way.
    void ToChunk(Vector2 world, int &outCx, int &outCy) const;

    const Chunk *Find(int cx, int cy) const;
    Chunk &Emplace(int cx, int cy);

    terrain::Settings settings_;
    int spacing_;

    std::unordered_map<std::int64_t, Chunk> chunks_;
};
