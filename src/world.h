#pragma once

#include "element.h"
#include "grid.h"
#include "light.h"
#include "raylib.h"
#include "terrain.h"
#include "water.h"
#include "weather.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
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

    // How many lattice vertices of each material an edit removed.
    //
    // A count of vertices rather than a sum of field values: a vertex either
    // held the material or it did not, which is the unit a mining yield is
    // naturally expressed in.
    using Yield = std::array<int, kElementCount>;

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

    // True where any material stops liquids.
    bool BlocksLiquidAt(Vector2 world) const;

    // True where any material stops light outright. Distinct from the liquid
    // and body tests: a torch stops neither light nor anything else, and water
    // stops liquid movement in no sense but dims what passes through it.
    bool BlocksLightAt(Vector2 world) const;
    bool OverlapsSolid(Rectangle rect) const;

    // The single material filling a world position, or nothing where the space
    // is open.
    //
    // Exclusion is what makes this answerable at all: because two occupying
    // materials never share a vertex, digging one out has exactly one outcome
    // and the caller does not have to decide between overlapping claims.
    std::optional<Element> OccupantAt(Vector2 world) const;

    // Field value of an element at the vertex nearest to a world position.
    float ValueAt(Element element, Vector2 world) const;

    // How much of a rectangle lies under liquid, in [0,1], averaged over the
    // samples it covers. Bodies use it to work out how much they float.
    float SubmergedFraction(Rectangle rect) const;

    // Fills every vertex within `radius` with a material, clearing whatever was
    // there first. Placing is always a replacement, since two materials cannot
    // share a vertex; a liquid is the exception and simply does not enter a
    // vertex a solid already fills.
    void Place(Element element, Vector2 world, float radius);

    // Empties every vertex within `radius` and reports what came out. This is
    // the shape the mining action takes: the world performs the removal, the
    // caller decides what the yield is worth.
    Yield Excavate(Vector2 world, float radius);

    // Advances the liquid inside `active` by one step. The region is copied
    // into a flat buffer, simulated, and written back, which keeps the
    // automaton free of any chunk bookkeeping.
    void StepWater(Rectangle active);

    void SetWaterSettings(const water::Settings &settings) { waterSettings_ = settings; }
    const water::Settings &WaterSettings() const { return waterSettings_; }

    // Solves the light over `region`, from the materials standing in it and
    // from whatever was handed to AddLight since the last call.
    //
    // Light is state that follows entirely from the world, so it is rebuilt
    // rather than stored: nothing about it has to survive a chunk being
    // released, and a torch placed this frame is alight the same frame.
    void StepLight(Rectangle region);

    // A light that exists for the next solve only.
    //
    // Anything that moves is lit this way rather than by standing a material in
    // the world: a creature, a thrown lantern, the player. The caller re-adds
    // it each frame, which means nothing has to be told when a light moves or
    // goes out.
    void AddLight(Vector2 world, light::Radiance radiance, float radius);

    // Light reaching a world position, and the same as a single level in [0,1].
    //
    // The level is the number game rules should be written against: crops that
    // need light, creatures that will not stand in it, a torch bright enough to
    // hold them off. Positions outside the region last solved read as dark.
    light::Radiance LightAt(Vector2 world) const { return lightField_.At(world); }
    float LightLevelAt(Vector2 world) const { return lightField_.LevelAt(world); }

    const light::Field &Light() const { return lightField_; }

    void SetLightSettings(const light::Settings &settings) { lightSettings_ = settings; }
    const light::Settings &LightSettings() const { return lightSettings_; }

    // Light the open sky gives off. Its horizon follows the terrain's own sky
    // depth, so raising the ground raises what counts as being under it.
    void SetSkyLight(light::Radiance radiance) { skyLight_ = radiance; }
    light::Radiance SkyLight() const { return skyLight_; }

    // The weather over the world.
    //
    // Owned here rather than by the caller because the light solve reads it: cloud
    // shade is filled into the medium one column at a time, the same way the
    // skyline is. Keeping it outside would mean handing the world a callback, or
    // handing the light a copy of the sky, and either one lets the two disagree
    // about which frame's weather is being lit.
    void SetWeather(const weather::Settings &settings) { sky_.Configure(settings, settings_); }

    // Drifts the weather. Separate from StepLight because the sky has to have
    // finished moving before the frame is lit, and callers already own that order
    // for the water.
    void StepWeather(float dt) { sky_.Advance(dt); }

    const weather::Sky &Sky() const { return sky_; }

    // How hard it is raining over a world position, in [0,1].
    //
    // Exposed because rain is a game rule as much as a picture: crops that need it,
    // a torch that will not stay lit in it, a surface that turns slick.
    float RainAt(Vector2 world) const { return sky_.RainAt(world.x); }

    // Total liquid held by the vertices inside a region. Reported so that
    // volume can be checked from outside the simulation.
    float TotalWater(Rectangle region) const;

    // Split so that liquids can be collected into their own layer and
    // composited in a single blend.
    void DrawTerrain(Rectangle view) const;
    void DrawLiquids(Rectangle view) const;

    // Field a liquid is drawn from, derived from its mass and clamped against
    // the solids around it. Exposed so the clamp can be checked directly.
    Grid LiquidRenderField(int cx, int cy, Element element) const;
    void DrawVertexOverlay(Rectangle view, float vertexSize, Color filledColor, Color emptyColor) const;

    // Threshold of the base terrain. Liquids and veins are bounded against it,
    // so it stays named even though every other decision now comes from the
    // element table.
    static constexpr float RockThreshold() { return Def(Element::Rock).threshold; }

    // Procedural value of an element at a world position, exclusion included,
    // or zero for one that is only ever placed by hand.
    float SpawnValue(Element element, Vector2 world) const;

    // One resident chunk, as the debug view sees it. Chunk bookkeeping stays
    // private; this is a snapshot of it, not a handle into it.
    struct ChunkView {
        int cx = 0;
        int cy = 0;
        Rectangle bounds{};
        bool edited      = false;
        bool holdsLiquid = false;
    };

    std::vector<ChunkView> ChunksIn(Rectangle view) const;

    const terrain::Settings &Settings() const { return settings_; }

    int Spacing() const { return spacing_; }
    float ChunkSpan() const { return static_cast<float>(kChunkCells * spacing_); }
    int ResidentChunks() const { return static_cast<int>(chunks_.size()); }

    // How many chunks are held past their usefulness because they carry state
    // the noise cannot reproduce. Reported so the cost of editing and flooding
    // the world is visible rather than merely suspected.
    int PinnedChunks() const;

private:
    struct Chunk {
        std::vector<Grid> fields; // One per Element.
        bool edited      = false; // Hand-painted, so no longer derivable.
        bool holdsLiquid = false; // Flooded, so no longer derivable either.
    };

    static std::int64_t Key(int cx, int cy);

    // Chunk coordinate containing a world position. Floor division, so it stays
    // correct left of and above the origin where truncation would round the
    // wrong way.
    void ToChunk(Vector2 world, int &outCx, int &outCy) const;

    // Chunk range a world region covers.
    void ChunkRange(Rectangle region, int &outMinCx, int &outMinCy, int &outMaxCx, int &outMaxCy) const;

    // Nearest lattice position to a world point. Every query and every write
    // snaps through this, so a value cannot depend on which side of a vertex
    // the caller happened to ask from.
    Vector2 SnapToLattice(Vector2 world) const;

    const Chunk *Find(int cx, int cy) const;
    Chunk &Emplace(int cx, int cy);

    // Writes a lattice position in every chunk that holds a copy of it, which
    // is up to four at a chunk corner.
    void WriteVertex(Element element, Vector2 vertex, float value);

    // Marks every chunk holding a copy of a lattice position as hand-edited, so
    // it is no longer regenerated from the noise.
    void MarkEdited(Vector2 vertex);

    // Global lattice index range covering a region.
    void LatticeRange(Rectangle region, int &outI0, int &outJ0, int &outI1, int &outJ1) const;

    // Value the material's own generator asks for, before any other material
    // has had a say. Exclusion is applied on top of this, and is expressed in
    // terms of it rather than of the finished value, so a material never
    // depends on the outcome of the contest it is itself part of.
    //
    // `ground` is the base terrain's density at the same position. Passed in
    // rather than sampled here because several materials are bounded against it
    // and it is the most expensive field the generator produces: asking for it
    // once per material per vertex was five times the work of asking once.
    float GeneratedValue(Element element, Vector2 world, float ground) const;

    // Ceiling a material's field is held under by everything that outranks it,
    // expressed so that it equals the material's own threshold exactly where
    // the higher-ranked one reaches its threshold.
    //
    // That single identity is what aligns the two contours: both cross their
    // thresholds on the same line, so the ore ends precisely where the rock
    // around it begins and neither leaves a seam.
    float ExclusionHeadroom(Element element, Vector2 world, float ground) const;

    // True where a material's generator is allowed to place it at all: inside
    // its band, and on the side of the ground its `space` names.
    bool SpawnEligible(const ElementSpawn &spawn, Vector2 world, float ground) const;

    // Empties a lattice position of everything that fills it, solid and liquid
    // alike, and adds what was there to `yield`.
    void ClearVertex(Vector2 vertex, Yield &yield);

    // Both edits a brush can make. Placing clears the vertex first, so it is
    // the same edit as digging with a second half.
    void ApplyBrush(Vector2 world, float radius, std::optional<Element> place, Yield &yield);

    // Signed margin by which the solids of at least `minPrecedence` fill each
    // vertex of a chunk: positive inside one of them, negative outside, and
    // exactly zero on the contour of whichever one claims the vertex.
    //
    // Materials do not share a threshold, so this is the one field about a
    // group of them that can be compared against a single number. The default
    // takes them all, which is what the liquid clamp and the vertex overlay
    // need; drawing asks for one rank and above.
    // `groundOnly` keeps to the materials a body cannot walk through, which is
    // what the terrain's own silhouette is made of. A torch occupies its vertex
    // and can be mined back out, but it is a fixture standing in the ground
    // rather than a part of it, and drawing it into the rock's outline gives it
    // a rock-coloured halo.
    Grid OccupancyField(const Chunk &chunk, int minPrecedence = std::numeric_limits<int>::min(),
                        bool groundOnly = false) const;

    // Cutoff each generated material's noise has to clear, measured from the
    // noise itself so that `probability` in the element table means what it says.
    void CalibrateSpawn();

    // World Y at which the ground begins in a lattice column, looking down from
    // the open sky.
    //
    // Read from the terrain function rather than from the world, and remembered
    // once found. It is a pure function of the column, so a column answered on
    // one frame has the same answer on every other, and scanning them all again
    // each frame cost more than solving the light did.
    float Skyline(int column) const;

    terrain::Settings settings_;
    int spacing_;

    std::array<float, kElementCount> spawnCutoff_{};

    // Occupying materials in ascending order of precedence. A chunk is carved
    // in this order so that when a material is clamped, everything that
    // outranks it still holds the value its own generator produced.
    std::vector<Element> exclusionOrder_;

    std::unordered_map<std::int64_t, Chunk> chunks_;

    water::Settings waterSettings_;

    // Kept between steps so that a 9000-cell region is not reallocated sixty
    // times a second.
    water::Buffer scratch_;

    // A light that lasts one solve. Cleared by StepLight, so a caller that
    // stops adding one has stopped emitting it.
    struct Spark {
        Vector2 at{};
        light::Radiance radiance{};
        float radius = 0.0f;
    };

    std::vector<Spark> sparks_;

    light::Settings lightSettings_{};
    light::Radiance skyLight_{2.6f, 2.8f, 3.1f};

    light::Medium medium_;
    light::Field lightField_;

    weather::Sky sky_;

    // One entry per lattice column, filled the first time that column is asked
    // about. Cleared with the world, since regenerating changes the ground.
    mutable std::unordered_map<int, float> skyline_;
};
