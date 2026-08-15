#pragma once

#include "element.h"
#include "grid.h"
#include "light.h"
#include "raylib.h"
#include "sod.h"
#include "soil.h"
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

    // What one stroke of the brush did.
    struct Stroke {
        // What it took out, per material.
        Yield freed{};

        // Vertices the placed material came to occupy that it did not occupy
        // before.
        //
        // Only the new ones. Placing writes every vertex in the circle, and a
        // brush swept back and forth over ground it has already laid rewrites
        // most of them every frame — so counting writes would charge a player
        // several times over for one wall. What a stroke costs is what it
        // gained.
        int filled = 0;
    };

    // Fills every vertex within `radius` with a material, clearing whatever was
    // there first, and stops once it has newly filled `budget` of them. Placing
    // is always a replacement, since two materials cannot share a vertex; a
    // liquid is the exception and simply does not enter a vertex a solid already
    // fills.
    //
    // The budget is how a supply that runs out mid-stroke stops the brush where
    // the material ran out instead of continuing for free. It is tested before
    // anything is cleared, so the brush never digs out ground it cannot afford
    // to replace.
    //
    // `keepClear` is a region no material a body cannot walk through may be laid
    // in — the character's own body, in practice. Without it a player aiming at
    // their own feet walls themselves in, and the ground they built is the one
    // ground they cannot dig back out of, because a body already inside the rock
    // is refused every move it tries to make. Liquids ignore it: standing in
    // water is swimming and not being buried.
    Stroke Place(Element element, Vector2 world, float radius, int budget, Rectangle keepClear = {});

    // Empties every vertex within `radius` and reports what came out. This is
    // the shape the mining action takes: the world performs the removal, the
    // caller decides what the yield is worth.
    Stroke Excavate(Vector2 world, float radius);

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

    // Sky held back over a run of columns, for the next solve only.
    //
    // The spark read the other way round: re-offered every frame, so nothing has
    // to be told when the thing casting it moves or is cut down.
    //
    // A *column* and not a volume, and that distinction is the whole of this. It
    // was a rectangle of extinction stamped into the medium, which is what a
    // material does — and it was wrong for the same reason it is right for a
    // material: extinction is fog, and a material's cell is also drawn, so its
    // own picture hides it. A canopy stands in open air where nothing is drawn,
    // so the fog was the only thing on screen and every tree wore a grey blob in
    // the sky above it. Softening the edge and dropping the figure to a tenth
    // made the blob fainter and no less a blob.
    //
    // This is the arrangement the clouds already use — `Medium::cover`, one
    // figure per column, applied to the sky a ray reaches at the end of its
    // march. Nothing is added to the air; what changes is how much sky arrives
    // underneath. Added to whatever the cloud is already holding back, because
    // shade under a cloud under a canopy is both.
    void AddCover(float fromX, float toX, float share);

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

    // The two ends of a day: what the open sky gives off at noon, and what is left
    // of it at midnight. Its horizon follows the terrain's own sky depth, so raising
    // the ground raises what counts as being under it.
    //
    // The pair rather than one value, because what a clock has to move between is
    // two lights and not one dimmed. A night is not a darker day: it is blue where
    // the day is near-neutral, which is what makes a torch read as warm once the sun
    // has gone.
    void SetDaylight(light::Radiance noon, light::Radiance midnight);

    // What it is giving off right now, worked out from those two and the time of
    // day. Not settable — it is derived, and setting it would only be overwritten on
    // the next step.
    light::Radiance SkyLight() const { return skyLight_; }

    // The weather over the world.
    //
    // Owned here rather than by the caller because the light solve reads it: cloud
    // shade is filled into the medium one column at a time, the same way the
    // skyline is. Keeping it outside would mean handing the world a callback, or
    // handing the light a copy of the sky, and either one lets the two disagree
    // about which frame's weather is being lit.
    void SetWeather(const weather::Settings &settings) { sky_.Configure(settings, settings_); }

    // Drifts the weather and the day, and works out what the sky is giving off by
    // the end of it. Separate from StepLight because the sky has to have finished
    // moving before the frame is lit, and callers already own that order for the
    // water.
    //
    // The daylight is turned into a radiance here rather than by the caller, because
    // doing it correctly needs the exposure the light field will read it back
    // through, and that is this object's to know.
    void StepWeather(float dt);

    const weather::Sky &Sky() const { return sky_; }

    // Runs the day on to its next quarter. For looking at a transition rather than
    // waiting for it; the weather and the clouds are not disturbed.
    void SkipToQuarter() { sky_.SkipToQuarter(); }

    // Holds the next season, and wraps back to whatever the clock says after the
    // fourth. For looking at the year before there is one.
    void CycleSeason() { sky_.ForceSeason((sky_.ForcedSeason() + 1) % 4); }

    // Straight to one, for a caller that knows which it wants rather than one
    // stepping through them.
    void SetSeason(int index) { sky_.ForceSeason(index); }

    // Steps the weather through clear, fair, overcast, storm and back to the sky's
    // own sequence. See weather::Sky::ForceMood.
    void CycleWeather() { sky_.CycleMood(); }

    // Holds one, by index, or hands the sky back its own sequence below zero.
    void ForceWeather(int mood) { sky_.ForceMood(mood); }

    // Pins the ground wind, or hands it back to the weather. Signed: the sign is
    // which way it blows.
    void SetWind(float speed) { sky_.ForceWind(speed); }
    void ReleaseWind() { sky_.ReleaseWind(); }

    // How hard it is raining over a world position, in [0,1].
    //
    // Exposed because rain is a game rule as much as a picture: crops that need it,
    // a torch that will not stay lit in it, a surface that turns slick.
    float RainAt(Vector2 world) const { return sky_.RainAt(world.x); }

    // How damp the ground is at a world position, in [0,1].
    //
    // The place's own climate, raised by rain that has fallen and lowered by the
    // daylight that has stood on it since — so it has a memory of the last shower
    // without the world having to keep one. The number a crop, a fire or a slick
    // surface should be written against.
    //
    // Costs a climate sample, which is a handful of noise. Cheap once; do not walk
    // every column with it.
    float HumidityAt(Vector2 world) const { return sky_.HumidityAt(world.x); }

    // World Y at which the ground begins in a lattice column, looking down from
    // the open sky.
    //
    // Read from the terrain function rather than from the world, and remembered
    // once found. It is a pure function of the column, so a column answered on
    // one frame has the same answer on every other, and scanning them all again
    // each frame cost more than solving the light did.
    //
    // Public because it is the surface anything *placed* has to be placed
    // against, and placement must not move. SurfaceProfile below is the surface
    // as built, which is the right answer for something falling out of the sky
    // and the wrong one for something grown out of the ground: a wood that read
    // the built surface would rearrange itself around every hole dug near it.
    float Skyline(int column) const;

    // World Y the first solid surface sits at, for a run of lattice columns
    // starting at `firstColumn`. Written into `out`, which is resized to `count`.
    //
    // What anything falling from the sky lands on, and the answer the terrain
    // function cannot give: terrain::Height describes the shape of the land, and a
    // roof is not a function of the column it stands over. This begins with that
    // shape and then lets what has been built overrule it.
    //
    // A run rather than a single column, because finding what has been built means
    // one walk of the chunk map however many columns are asked about, and asking
    // one at a time would pay for that walk each time.
    void SurfaceProfile(int firstColumn, int count, std::vector<float> &out) const;

    // Where something put down at `world` would come to rest, as the top of the
    // ground under it, or false where there is none within `reach`.
    //
    // A point and not a column, and that is the whole of it. Skyline and
    // SurfaceProfile both answer about a column, from the shape of the land, and
    // both are deliberately blind to digging so that a wood does not rearrange
    // itself around a hole. That blindness is right for growing a forest and wrong
    // for a hand: a player who has built a ramp of earth and clicks on it means
    // the ramp, and a query about the column answers with the hillside the ramp
    // was built over — several body-heights below where they pointed.
    //
    // Both directions, because a cursor can be inside the ground as easily as
    // above it. Pointing at the middle of a ramp walks up to the top of it;
    // pointing at open air falls until it lands on something.
    bool FootingUnder(Vector2 world, float reach, float &outTop) const;

    // Total liquid held by the vertices inside a region. Reported so that
    // volume can be checked from outside the simulation.
    float TotalWater(Rectangle region) const;

    // The grass over the ground now in play, for whatever stands on it.
    //
    // Filled once a frame by Update. Both the band drawn into the terrain and the
    // tufts standing on it read from this, which is what stops the two from
    // disagreeing about where the grass is or how tall it has grown.
    sod::Blades Grass() const;

    // Cuts whatever grass a blow reaches, and reports how many tufts came down.
    //
    // Called only on the frame a swing begins. Player::AttackHitbox stays live for
    // the whole strike window, so a caller reading that instead cuts the same
    // tuft nine times and hands out nine times the fibre.
    int MowGrass(Rectangle hitbox, float now);

    // How many tufts the world can no longer describe on its own.
    int MownTufts() const { return static_cast<int>(mown_.size()); }

    // Split so that liquids can be collected into their own layer and
    // composited in a single blend.
    void DrawTerrain(Rectangle view) const;
    void DrawLiquids(Rectangle view) const;

    // Rain, over the ground it lands on, and the stars, over the ground that hides
    // them.
    //
    // Drawn from here rather than from the sky directly because the sky is a
    // function of position and time and knows nothing of what has been built. Only
    // the world can say where a drop stops or where a star is behind a hill, so it
    // is the world that hands the sky the surface.
    //
    // The stars are drawn *after* the light rather than under it — see
    // weather::Sky::DrawStars — which is why they need to be told about the ground
    // at all instead of simply being covered by it.
    void DrawRain(Rectangle view) const;
    void DrawStars(Rectangle view) const;

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

    // How many vertices the world remembers having been changed by hand.
    //
    // This is the one thing that grows without bound and never shrinks, so it is
    // reported for the same reason the pinned chunks are: it is the price of the
    // edits being permanent, and it should be visible rather than suspected.
    int RememberedEdits() const;

private:
    struct Chunk {
        std::vector<Grid> fields; // One per Element.
        bool edited      = false; // Hand-painted, so no longer derivable.
        bool holdsLiquid = false; // Flooded, so no longer derivable either.
    };

    // The state a hand-edited vertex was left in.
    //
    // Not a history and not a delta — the state. A brush leaves a vertex holding
    // exactly one material at full or holding nothing at all, so the last stroke
    // over a vertex is the whole truth about it and editing the same one a
    // hundred times stores one of these.
    struct Edit {
        // Global lattice indices, not chunk-relative: a vertex on a border
        // belongs to four chunks, and none of them has to be resident for the
        // edit to be remembered.
        int i = 0;
        int j = 0;

        // What was left there, or nothing where it was dug out.
        std::optional<Element> element;
    };

    // Two indices packed into one, for the maps keyed by a pair of them. Named
    // for the chunk grid it was written for, and used unchanged for the lattice
    // one that sown_ is kept on: a pair of ints is a pair of ints.
    static std::int64_t Key(int cx, int cy);
    static void FromKey(std::int64_t key, int &outI, int &outJ);

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
    // it is no longer regenerated from the noise while it is resident.
    void MarkEdited(Vector2 vertex);

    // Records what a brush left at a lattice position, so that the chunk holding
    // it can be thrown away and rebuilt exactly.
    //
    // Pinning the chunk was the whole of what kept an edit alive, and a pin is
    // not the same thing as a memory: a chunk far enough behind the player is
    // dropped whatever it holds, or the world grows with the distance walked
    // rather than with the size of the view. So walking far enough from
    // something built and coming back found it gone.
    void Remember(Vector2 vertex, std::optional<Element> element);

    // Notes that a lattice position was turned over by hand just now, so the
    // grass over it has to be earned again rather than being there the moment the
    // block lands.
    //
    // Every disturbed vertex and not one per column, because which of them is the
    // one the grass would grow on is not knowable when the brush passes: filling a
    // hole makes the highest vertex the new surface and digging a pit makes the
    // lowest one, and the same column can be both within a stroke. ReadSod asks
    // the question the other way round — it already knows where the surface is,
    // and only has to ask whether the vertex sitting there was disturbed.
    void Disturb(Vector2 vertex);

    // Replays every remembered edit that lands in a freshly generated chunk.
    //
    // Runs after the exclusion pass, which is where a live edit happens too: a
    // brush writes over the finished field, not into the contest that produced
    // it.
    void ApplyEdits(Chunk &chunk, int cx, int cy) const;

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
    float GeneratedValue(Element element, Vector2 world, const terrain::Ground &ground,
                         const terrain::Climate &climate) const;

    // Ceiling a material's field is held under by everything that outranks it,
    // expressed so that it equals the material's own threshold exactly where
    // the higher-ranked one reaches its threshold.
    //
    // That single identity is what aligns the two contours: both cross their
    // thresholds on the same line, so the ore ends precisely where the rock
    // around it begins and neither leaves a seam.
    float ExclusionHeadroom(Element element, Vector2 world, const terrain::Ground &ground,
                            const terrain::Climate &climate) const;

    // True where a material's generator is allowed to place it at all: inside
    // its band, and on the side of the ground its `space` names.
    bool SpawnEligible(const ElementSpawn &spawn, Vector2 world, float ground) const;

    // Empties a lattice position of everything that fills it, solid and liquid
    // alike, and adds what was there to `yield`.
    //
    // Reports whether it removed anything a body could not walk through, which
    // is the same question as whether it counted as an edit: a brush swept
    // through open sky changes nothing and must not be remembered as having.
    bool ClearVertex(Vector2 vertex, Yield &yield);

    // Both edits a brush can make. Placing clears the vertex first, so it is
    // the same edit as digging with a second half. `budget` bounds how many
    // vertices the placed material may newly take, and is ignored when digging.
    // `keepClear` is the region solids may not be laid in — see Place.
    Stroke ApplyBrush(Vector2 world, float radius, std::optional<Element> place, int budget, Rectangle keepClear);

    // Whether the square a vertex owns meets a rectangle.
    //
    // The same square OverlapsSolid tests a body against, and deliberately the
    // same test: a vertex it would report as overlapping the character is exactly
    // a vertex nothing solid may be written to, and two different answers to one
    // question is how a body ends up inside a block that was never meant to
    // reach it.
    bool VertexMeets(Vector2 vertex, Rectangle rect) const;

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

    // The surface under a view, widened by `margin`, as the sky wants to be handed
    // it. Fills `surface_` and returns a view over it, so the caller holds nothing.
    weather::Ground GroundUnder(Rectangle view, float margin) const;

    // Whether one of a chunk's own vertices is filled by something a body cannot
    // pass through. The test IsSolidAt makes, read straight off the chunk's grids:
    // no snapping, no map lookup and no falling back to the noise, which is what
    // makes it cheap enough to walk a whole chunk with.
    bool SolidVertex(const Chunk &chunk, int i, int j) const;

    // How deep the cover over the rock goes in a column, in world pixels, from
    // the surface down to the first thing that is not one.
    //
    // Handed to the light so that daylight reaches the bottom of the soil and
    // begins to fail in the rock underneath — a depth the generator decides, and
    // so one the light has to be told rather than one it could choose.
    float CoverDepth(float worldX, float surfaceY) const;

    // World Y of the first solid met falling down a column, or false where the
    // column holds nothing at all within reach.
    //
    // Not SurfaceProfile, and the difference matters here. That one only ever
    // raises the surface — a column dug out below the skyline still answers with
    // the skyline, deliberately, because it is also asked about columns whose
    // chunks are not resident and the noise is the only thing that can answer
    // those. Grass has to follow the hole the player dug, so it pays for the walk.
    bool SurfaceOf(float worldX, float &outTop) const;

    // The whole ground at a world position, as the distance past its own
    // threshold: positive inside anything a body cannot walk through, negative in
    // open space.
    //
    // The union across materials rather than any one of them, because what buries
    // a sod is anything at all standing on it.
    float GroundValueAt(Vector2 world) const;

    // Works out the grass over a span of columns and points sodRamp_/sodCover_ at
    // it. Called once a frame, before anything reads either.
    void ReadSod(Rectangle view);

    // Takes the grass back off the columns whose earth was turned over lately, and
    // lets it back on at the speed the front crosses them.
    //
    // Split out of ReadSod because it is a different question asked of the same
    // band: that one works out where grass could grow, and this one works out
    // where it has had the time to. Runs after it, and only ever subtracts.
    //
    // Run again by a brush that turned any earth over, which is what keeps a
    // stroke from drawing one frame of grass on ground it has just dug: the band
    // was worked out at the top of the frame and the stroke happens well after
    // it. Idempotent, so running it twice over a frame costs a second pass and
    // changes nothing else.
    void ReadSown();

    // The field the grass is drawn from, derived from a chunk's own soil and from
    // the silhouette of the ground as a whole.
    //
    // Positive where there is grass, and by how much: inside the soil, within
    // sod::kSodDepth of a face, on the side of that face that looks at the sky,
    // and as much of it as has grown back. A field rather than a test, so that the
    // same rasteriser draws it and it therefore lands on exactly the contour the
    // ground did — no alignment to get right, because there is no second answer to
    // align with.
    Grid SodField(const Chunk &chunk) const;

    terrain::Settings settings_;
    int spacing_;

    std::array<float, kElementCount> spawnCutoff_{};

    // How each material colours one of its own texels.
    //
    // Built once, here, rather than per chunk per frame: what a painter carries is
    // the seven-step ramp its four authored tones expand into, and interpolating
    // that a hundred times a frame to get the same seven colours would be work
    // done to arrive back where it started.
    std::array<soil::Paint, kElementCount> paint_{};

    // The grass over the ground now in play, one entry per lattice column.
    //
    // Kept rather than asked for per texel because what decides the colour — the
    // climate, the turn of the year, how much rain the ground has had — varies
    // over thousands of pixels and the texels being drawn are five apart. Asking
    // per texel would be sampling the surface's eight octaves a few hundred times
    // a frame to be told the same answer.
    //
    // Filled by ReadSod once a frame. Both the band the terrain draws and the
    // tufts standing on it read from this, which is what keeps the two from
    // disagreeing about where the grass is.
    std::vector<soil::Ramp> sodRamp_;
    std::vector<float> sodCover_;
    std::vector<float> sodTop_;
    std::vector<float> sodPush_;
    int sodFirstColumn_ = 0;

    // What is left of each tuft after a blow, on the tufts' own grid.
    std::vector<float> sodStanding_;
    std::int64_t sodFirstCell_ = 0;

    // The tufts the player has cut, and when.
    //
    // Sparse, and it gives entries back: a tuft nobody has swung at has no record
    // at all, and one that has grown again has nothing left to say and is
    // dropped. So unlike edits_, which only ever grows, this tracks what is
    // outstanding rather than everything that has ever happened.
    std::unordered_map<std::int64_t, float> mown_;

    // The lattice positions a brush turned over, and when — bare earth, until
    // grass reaches it.
    //
    // Kept on the same terms as mown_ above and for the same reason: an entry has
    // something to say only until the ground there is as established as the ground
    // around it, and then the world can describe it again on its own. That is what
    // bounds this against edits_, which has to keep everything for ever.
    std::unordered_map<std::int64_t, float> sown_;

    // How fresh the turned earth is under each column of the band, on the plant
    // grid ReadSod works in, and how far each column is from the nearest
    // established grass.
    //
    // Held here rather than built per frame so the two vectors are allocated once.
    // The second is the whole of the spreading: grass arrives at a column a delay
    // after it arrived at the one beside it, so a filled hole greens from its rim
    // inwards and the middle of a laid platform is the last of it to turn — which
    // is what the ground does in Minecraft, and it is the reason a dirt block
    // there is a thing you notice rather than a thing you never see.
    std::vector<float> sodSown_;
    std::vector<float> sodSpread_;

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

    // Sky held back over a span of columns, lasting one solve.
    struct Cover {
        float fromX = 0.0f;
        float toX   = 0.0f;
        float share = 0.0f;
    };

    std::vector<Cover> covers_;

    light::Settings lightSettings_{};

    // The two ends of the day, and the point of it the sky is at now. The last is
    // derived every step and is the only one the solve reads.
    light::Radiance dayLight_{2.6f, 2.8f, 3.1f};
    light::Radiance nightLight_{0.05f, 0.06f, 0.09f};
    light::Radiance skyLight_{2.6f, 2.8f, 3.1f};

    light::Medium medium_;
    light::Field lightField_;

    weather::Sky sky_;

    // Every vertex the player has changed, grouped by the chunk it is filed
    // under. The whole of what this world cannot derive from its own noise.
    //
    // Grouped so that generating a chunk is four lookups rather than a walk of
    // every edit ever made — four, because a vertex on a border belongs to four
    // chunks and is filed under one of them, which is the same asymmetry
    // WriteVertex has, read the other way round.
    //
    // It grows with what has been built and never shrinks, which is what makes
    // the edits permanent. The cost is per *vertex touched*, not per chunk: a
    // chunk somebody set one block down in used to hold nine full grids, some
    // forty kilobytes, for the sake of one changed number.
    std::unordered_map<std::int64_t, std::vector<Edit>> edits_;

    // One entry per lattice column, filled the first time that column is asked
    // about. Cleared with the world, since regenerating changes the ground.
    mutable std::unordered_map<int, float> skyline_;

    // The ground under one frame's rain. Kept between frames only so that drawing
    // does not allocate; nothing in it survives the call that fills it.
    mutable std::vector<float> surface_;
};
