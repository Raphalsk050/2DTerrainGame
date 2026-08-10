#pragma once

#include "element.h"
#include "grid.h"
#include "raylib.h"
#include "terrain.h"
#include "water.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Unbounded terrain, materialised in square chunks around a moving focus.
//
// Chunks are generated when they come near the view and released when they
// leave it. Because terrain::Density is a pure function of world position, a
// chunk regenerated later is identical to the one that was released, and no
// state has to survive in between.
//
// Two kinds of chunk break that property and are pinned in memory instead:
// those the player edited, and those holding liquid. Neither follows from the
// noise any more.
class World {
public:
    // Cells along one chunk side.
    static constexpr int kChunkCells = 32;

    // One vertex more than cells, so that adjacent chunks share their border
    // column and row. Without the overlap the cell between two chunks belongs
    // to neither, and the contour breaks at every chunk seam.
    //
    // The duplication is invisible to the simulation: reads resolve to a single
    // owner and writes update every copy, so a shared vertex never holds two
    // different amounts of liquid.
    static constexpr int kChunkVertices = kChunkCells + 1;

    World(const terrain::Settings &settings, int spacing);

    // Generates the chunks covering `view` plus a margin and releases those
    // that fell well outside it.
    void Update(Rectangle view);

    // Discards every chunk, including edited and flooded ones, so the world
    // regenerates from the noise alone.
    void Reset();

    // Rock queries in world space, answered by the vertex nearest to the
    // position. Positions in chunks that are not resident fall back to the
    // noise function at that same vertex, so collision stays correct beyond
    // the loaded area and does not change as chunks come and go.
    bool IsSolidAt(Vector2 world) const;
    bool OverlapsSolid(Rectangle rect) const;

    // Field value of an element at the vertex nearest to a world position.
    float ValueAt(Element element, Vector2 world) const;

    // How much of a rectangle lies under liquid, in [0,1], averaged over the
    // samples it covers. Bodies use it to work out how much they float.
    float SubmergedFraction(Rectangle rect) const;

    // Sets every vertex within `radius` of a world position, across chunk
    // borders. Only resident chunks are affected.
    void Paint(Element element, Vector2 world, float radius, bool add);

    // Advances the liquid inside `active` by one step. The region is copied
    // into a flat buffer, simulated, and written back, which keeps the
    // automaton free of any chunk bookkeeping.
    void StepWater(Rectangle active);

    void SetWaterSettings(const water::Settings &settings) { waterSettings_ = settings; }
    const water::Settings &WaterSettings() const { return waterSettings_; }

    // Total liquid held by the vertices inside a region. Reported so that
    // volume can be checked from outside the simulation.
    float TotalWater(Rectangle region) const;

    // Split so that liquids can be collected into their own layer and
    // composited in a single blend.
    void DrawTerrain(Rectangle view) const;
    void DrawLiquids(Rectangle view) const;

    // Field a liquid is drawn from, derived from its mass and clamped against
    // the rock. Exposed so the clamp can be checked directly.
    Grid LiquidRenderField(int cx, int cy, Element element) const;
    void DrawVertexOverlay(Rectangle view, float vertexSize, Color filledColor, Color emptyColor) const;

    // The one threshold that decides where rock is, shared by collision, the
    // contour, the fill and the debug overlay.
    static float RockThreshold() { return StyleOf(Element::Rock).threshold; }

    int Spacing() const { return spacing_; }
    int ResidentChunks() const { return static_cast<int>(chunks_.size()); }

private:
    struct Chunk {
        std::vector<Grid> fields; // One per Element.
        bool edited     = false;  // Hand-painted, so no longer derivable.
        bool holdsWater = false;  // Flooded, so no longer derivable either.
    };

    // World span of one chunk, in pixels.
    float ChunkSpan() const { return static_cast<float>(kChunkCells * spacing_); }

    static std::int64_t Key(int cx, int cy);

    // Chunk coordinate containing a world position. Floor division, so it stays
    // correct left of and above the origin where truncation would round the
    // wrong way.
    void ToChunk(Vector2 world, int &outCx, int &outCy) const;

    // Nearest lattice position to a world point. Every query and every write
    // snaps through this, so a value cannot depend on which side of a vertex
    // the caller happened to ask from.
    Vector2 SnapToLattice(Vector2 world) const;

    const Chunk *Find(int cx, int cy) const;
    Chunk &Emplace(int cx, int cy);

    // Writes a lattice position in every chunk that holds a copy of it, which
    // is up to four at a chunk corner.
    void WriteVertex(Element element, Vector2 vertex, float value);

    // Global lattice index range covering a region.
    void LatticeRange(Rectangle region, int &outI0, int &outJ0, int &outI1, int &outJ1) const;

    terrain::Settings settings_;
    int spacing_;

    std::unordered_map<std::int64_t, Chunk> chunks_;

    // Kept between steps so that a 9000-cell region is not reallocated sixty
    // times a second.
    water::Settings waterSettings_;

    water::Buffer scratch_;
};
