#pragma once

#include "world/element.h"
#include "core/grid.h"
#include "render/light.h"
#include "raylib.h"
#include "world/sod.h"
#include "world/soil.h"
#include "world/terrain.h"
#include "weather/vista.h"
#include "world/water.h"
#include "weather/weather.h"

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
namespace save {
class Writer;
class Reader;
} // namespace save

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

    // What making a world is doing right now, for the screen that is waiting on it.
    //
    // A line of English rather than a percentage alone, because the wait is long
    // enough to need explaining: the ore cutoffs are *measured* against this
    // world's own noise, one seam at a time, and a bar with no words on it makes
    // that look like the game having hung.
    struct Making {
        char what[96] = "";

        float share = 0.0f; // 0 to 1
        bool done   = true;
    };

    // Starts making a world and steps it, so that the frame keeps drawing while it
    // happens.
    //
    // Split in two rather than done in one call, and it is the difference between a
    // loading screen and a frozen window. The measurement below is seconds of work;
    // handed to the caller as one call it is seconds with nothing on screen, no way
    // to say what is being waited for, and a window the desktop paints over as not
    // responding.
    //
    // `budget` is how long a slice may take, in seconds. The work stops on the
    // first row block past it, so a slice overruns by at most one block — a few
    // milliseconds — and never by a material.
    // Together they put a different country into the same object rather than
    // building a second World and swapping it in, and that is not a convenience:
    // every system in the loop holds a reference to this one — the wood, the
    // fixtures, the editor, the light — so a swap is either a rebuild of all of
    // them or a dangling reference nobody notices until a chunk is asked for. This
    // is Reset with the seed allowed to move, and Reset is already the thing that
    // says what a world forgets.
    void BeginRebuild(const terrain::Settings &settings);
    Making StepRebuild(float budget);

    // Rasterises the ground of every chunk the view needs and has not got.
    //
    // Opens a render target of its own, so it has to run outside a frame — a
    // texture mode cannot be entered inside one. Call it before BeginDrawing;
    // DrawTerrain then draws what this prepared.
    void PaintChunks(Rectangle view);

    // Releases the textures. Called before the window closes, since they are GPU
    // resources and the context goes with it.
    void UnloadPainted();

    // Drops what the grass band remembers about every column, so the next Update
    // works the whole of it out again.
    //
    // Exists so that the remembered band can be checked against a recomputation
    // of it — see `--sodcheck`. Nothing in the game needs it: what invalidates a
    // column is a change to the ground there, and the world does that itself.
    void ForgetGrass() { sodColumns_.clear(); }

    // Rock queries in world space, answered by the vertex nearest to the
    // position. Positions in chunks that are not resident fall back to the
    // noise function at that same vertex, so collision stays correct beyond
    // the loaded area and does not change as chunks come and go.
    bool IsSolidAt(Vector2 world) const;

    // Whether anything that stops a body is actually *painted* at this point.
    //
    // The same test the draw makes, and deliberately not the same test `IsSolidAt`
    // makes. A square is painted where the field **interpolated at its centre** is
    // over the threshold; a body collides where the **nearest lattice vertex** is.
    // Those agree in the middle of a hillside and part company at every edge, and at
    // one vertex standing on its own they part company completely: the nearest texel
    // centre sits a quarter of a cell away in each direction, so it reads
    // nine sixteenths of the vertex's value, and anything between the threshold and
    // nine fifths of it is a full square of solid ground that nothing paints a pixel
    // of.
    //
    // Which is a block that is invisible and impassable at once — §13.5's fault
    // exactly, one layer down. That section fixed the hand's half of it, so the
    // player can dig such a vertex out; this is the half where they cannot see that
    // there is anything to dig.
    bool PaintedAt(Vector2 world) const;

    // Whether the vertex nearest `world` is ground that nothing draws.
    //
    // Over the threshold — so a body stops against the whole square around it — while
    // not one of the four squares that make up that square is painted. There is no
    // arrangement of the game in which that is a thing the world should contain, and
    // `ApplyStroke` clears them rather than leaving them: see the sweep at the end of
    // it, and `--stuck`, which is the check.
    bool Degenerate(Vector2 world) const;

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

    // The build grid: which cell a world position falls in, and where a cell is.
    //
    // The grid is the lattice counted in threes and is anchored at the world
    // origin, not at the player or at a chunk. That is what makes it one grid
    // rather than a grid per session: a wall built today lines up with a wall
    // built tomorrow a thousand pixels away, and two cells never overlap by a
    // vertex. See config::kBuildCell.
    static void ToCell(Vector2 world, int &outCx, int &outCy);
    static Rectangle CellBounds(int cx, int cy);

    // Fills one cell of the build grid, or empties it.
    //
    // The only way anything writes into the world by hand. There was a circular
    // brush of an arbitrary radius beside these and it is gone: a round tool over
    // a square grid can only ever leave the corners of the cells it crosses, which
    // is what a wall with bites out of it is made of. A wide stroke is a block of
    // these, which the caller walks.
    //
    // A cell is exactly config::kBuildCellArea vertices, which is exactly one
    // block, so a click spends one and digging it out returns one — see
    // kVerticesPerBlock. Nothing is charged where the cell already held the
    // material, which reports back as a `filled` of zero.
    //
    // Placing is refused where anything already occupies the cell -- see CellVacant.
    // A liquid and a wall are the two exceptions, and both for reasons of their own
    // rather than as edge cases. `keepClear` is a region no material a body cannot walk through may be
    // laid in — the character's own body, in practice. Without it a player aiming
    // at their own feet walls themselves in, and the ground they built is the one
    // ground they cannot dig back out of, because a body already inside the rock
    // is refused every move it tries to make.
    Stroke PlaceCell(Element element, int cx, int cy, Rectangle keepClear = {});
    Stroke ExcavateCell(int cx, int cy);

    // Whether a cell is free of a region nothing solid may be laid in — the
    // player's body, in practice.
    //
    // Public because the hand has to ask it a frame ahead of the click, so the
    // square can go red before it is pressed rather than the press being
    // swallowed. PlaceCell asks it again and is the one that enforces it.
    bool CellClear(int cx, int cy, Rectangle keepClear) const;

    // Whether a cell is free of anything a piece could be set into.
    //
    // Placing used to be a *replacement*: two occupying materials cannot share a
    // vertex, so setting one into a cell cleared whatever was there and handed it
    // back. That reads as generous and it is a hole through the whole game. A seam of
    // diamond is fifteen seconds of work a cell; right-clicking a cobblestone into it
    // took the diamond out instantly and gave it to the player, so the cheapest way
    // to mine anything was to build over it. Every figure in element.h was a
    // suggestion while that was true.
    //
    // So: something is already there, you dig it out first. Minecraft's rule and
    // Terraria's, and the one that makes breaking a block mean anything.
    //
    // Only what *occupies*. A liquid does not stop a block -- it is displaced by one,
    // which is what Minecraft does with water -- and a wall behind the cell is
    // exactly what a block is meant to be built in front of, or a wall could never be
    // covered up. See ElementRules::occupies and §11.2 of CLAUDE.md.
    //
    // Asked over every vertex of the cell and not at its middle. The middle is the
    // right place to ask what a cell *is*; this is asking whether anything at all is
    // in the way, and the answer has to be the same one ApplyStroke would act on, or
    // there is a sliver of ore at a contour edge that a placement still eats.
    bool CellVacant(int cx, int cy) const;

    // What one cell of the build grid is holding, counted per material over the
    // same vertices ExcavateCell would take out of it.
    //
    // The point of it is that it is the *same walk*. Anything asking "is there
    // something here to break, and what is it" from a single point — the middle of
    // the cell, say — is asking a different question than the spade answers, and the
    // two part company exactly where it matters: at a contour edge, where the middle
    // is open sky and a corner still holds ground. A remnant like that is one vertex,
    // so it draws almost nothing and reads as empty air — but a body collides with
    // the square around every filled vertex, so it is solid, and a hand that decided
    // from the middle would refuse to dig it. Invisible, impassable and immovable at
    // once, which is one bug and not three.
    //
    // Counted on the same rule ClearVertex counts by: one per material per vertex
    // standing above its threshold. The layer behind is included only where nothing
    // in front was found, which is ExcavateCell's own order — see §11.2 of CLAUDE.md.
    void CellHolds(int cx, int cy, Yield &out) const;

    // Which material a cell counts as: the occupying one holding most of its
    // vertices. Nothing where none of them occupies it.
    //
    // **This is what makes a cell a block.** The ground is a field and a cell is
    // nine samples of it, so a cell that straddles a boundary is genuinely part soil
    // and part rock — but a *block* is one thing, and every rule the player meets is
    // written about blocks. Without one answer, a cell five-four between two
    // materials was charged the two rates added together and paid out in two ledgers
    // that each fell short of a whole block, so it broke slowly and dropped nothing
    // at all. See Editor::Bank.
    //
    // Only what occupies takes part. A liquid standing in the space around a block
    // is not what the block is made of — a submerged seam of rock would otherwise
    // count as water and hand the player water — and a wall behind it is a layer of
    // its own, which ExcavateCell only ever reaches where the front is already
    // empty.
    //
    // A tie goes to the higher `precedence`, which is to say to the rarer thing: an
    // ore against the rock it sits in, soil against the stone under it. That is the
    // one the player came for, and it is a rule rather than an accident of the
    // table's order.
    static std::optional<Element> ChiefOf(const Yield &holds);

    // Whether anything stands in the layer behind a cell.
    //
    // Asked at the middle of the cell, for the reason everything else about a cell
    // is: at its edge the answer is whatever the contour rounded to, and the
    // question is about the cell as a whole.
    bool WalledAt(int cx, int cy) const;

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
    // There was an AddCover here, and the canopy shade that used it is gone. Worth
    // the note, because the idea will look obvious again:
    //
    // A canopy could not be stamped into the medium as extinction the way a cloud
    // is — extinction is fog, a material's own picture hides its cell, and a canopy
    // stands in open air where nothing is drawn, so the fog was the only thing on
    // screen and every tree wore a grey blob in the sky above it. So it was held
    // back as a *share per column* instead, applied to the sky a ray reached at the
    // end of its march.
    //
    // What that could not survive is that the share is offered by whatever the grove
    // is currently holding, and the grove's set turns over as the player walks. A
    // tree entering or leaving it added or removed a whole band of held-back sky at
    // once, and the ground under that band stepped. Nothing about it was gradual and
    // nothing about it was transport: it was a set membership changing, expressed as
    // daylight. It read as the light flickering as you walked, and it survived
    // turning the bounce off, which is what finally placed it.
    //
    // The way back is the one the cloud already took: put the leaves themselves into
    // the medium as matter, so a canopy occludes by being there. Then it is
    // transport like everything else, it moves when the tree moves, and no set
    // membership is involved. Until then a wood has no dark floor.

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

    // Puts the sky's clock where a save left it, so a world saved at midnight comes
    // back at midnight.
    void SetClock(float when) { sky_.SetTime(when); }

    // Everything this world holds that its own noise cannot produce.
    //
    // Which is exactly `edits_` and nothing else — the whole design of the generator
    // is that a chunk is a pure function of position and seed, so the journal *is* the
    // difference between the country the seed describes and the one the player has
    // been living in. Nothing about chunks, silhouettes, pictures or the light is
    // written down, because every one of those is derived and would be rebuilt
    // identically from the same journal.
    //
    // `mown_` and `sown_` are deliberately not saved. They are timers, not
    // modifications: turned earth greening over and a tuft growing back, both measured
    // on the weather clock. A world you come back to has settled, which is what a
    // world you come back to looks like.
    void Save(save::Writer &out) const;

    // Reads that journal back. Entered on the section's own record, and it consumes
    // the lines under it. The world must already have been rebuilt for this seed:
    // `Reset` clears the journal, so loading into an unrebuilt world would leave the
    // last country's edits standing in the new one.
    void Load(save::Reader &in);

    // The ranges standing behind the world.
    //
    // Owned here for the same reason the sky is: they are configured against the
    // terrain — the climate at a column decides what colour the far country is —
    // so a rebuild has to reach them, and a caller holding its own copy is a
    // second world that can disagree with this one about which one it is.
    void SetVista(const vista::Settings &settings) { vista_.Configure(settings, settings_); }

    const vista::Range &Vista() const { return vista_; }

    // Releases the texture the ranges are blitted from, beside UnloadPainted and on
    // the same terms: the world owns it, so the world is where it is given back.
    void UnloadVista() { vista_.Unload(); }

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

    // Whether cloud and canopy hold any of the sky back, for the light only.
    //
    // An instrument and not a setting. The cover is the one thing about daylight that
    // is still a field handed in from outside rather than transported, so when the
    // light moves as the player walks it is the first thing that has to be ruled in
    // or out -- and the only way to rule it out is to switch it off and walk the same
    // ground again. The clouds go on being drawn: what stops is their say over the
    // light, so the two can be told apart on screen.
    // The two things that stand between the sky and the ground, each on its own
    // switch.
    //
    // They used to share one, and that made the switch useless for the only job it
    // has: a flicker that survives turning "sky cover" off has been cleared of two
    // suspects at once and of neither individually. They are not even the same kind
    // of thing — the cloud is matter stamped into the medium and transported through,
    // the canopy is a share held back per column because its leaves are drawn rather
    // than laid into the medium — so one switch was hiding that too.
    void ToggleSkyCover() { skyCover_ = !skyCover_; }
    bool SkyCover() const { return skyCover_; }


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

    // World Y of the first solid met falling down a column, or false where the
    // column holds nothing at all within reach.
    //
    // Not SurfaceProfile, and the difference matters here. That one only ever
    // raises the surface — a column dug out below the skyline still answers with
    // the skyline, deliberately, because it is also asked about columns whose
    // chunks are not resident and the noise is the only thing that can answer
    // those. Grass has to follow the hole the player dug, so it pays for the walk.
    //
    // Public because the collision overlay reads it. That overlay's whole subject
    // is the gap between where the ground is drawn and where a body stops, and it
    // has to ask through the same walk the world uses, or it would be comparing
    // its own arithmetic against the world's rather than the world's two answers
    // against each other.
    bool SurfaceOf(float worldX, float &outTop) const;

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

    // The floor under everything that is drawn behind the ground.
    //
    // The frame used to draw the sky twice: once into itself as the source of the
    // light, and once again inside the lit layer, erased everywhere the land was
    // not standing behind it — so that a cave came out dark instead of the colour
    // of a bright afternoon. It worked and it read wrong. What was left behind a
    // dug hillside was the *sky*, dimmed: a flat wash of horizon colour that
    // matches nothing around it, so every hole in the ground had a pane of pale
    // grey in it where there should have been more ground.
    //
    // What is behind a cave is the rock the cave was cut out of, and it is now
    // drawn as that — see PaintBackdrop, which paints it into the same chunk
    // texture as the ground and so costs the frame nothing.
    //
    // This is the backstop under that: one flat rectangle per column, in the deep
    // rock's own dark, over the whole view below the generator's own skyline. It
    // is what covers the stretches no chunk has a picture of yet, and it is why a
    // chunk arriving late shows as a patch of plain rock rather than as a window
    // onto the sky.
    //
    // The land here is the *generator's* land and not the world's, on the rule §8
    // of CLAUDE.md states for the covers: what is behind a cave is a property of
    // the country, and a hole somebody dug is a hole into rock rather than a
    // window onto the sky. It is also what keeps a platform built in mid-air from
    // turning the whole sky under it into cave.
    void DrawUnderground(Rectangle view) const;

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

    // The fog lying in the low ground under a closed sky. Asked of the world for
    // the reason the rain is: the bank stops at the first solid thing under it, and
    // what is under it is the world's to know — a roof and a hillside both stand in
    // the fog, and only one of them is in the generator.
    void DrawMist(Rectangle view) const;

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

        // A silhouette the chunk has already been asked for.
        //
        // The draw paints each material as the union of itself and everything
        // that outranks it, so it asks for ten of these per chunk — each a fresh
        // grid of a thousand samples, each sample the maximum over ten fields.
        // That was the whole of it being built again every frame for a shape
        // that follows from the fields alone, and the fields change only where
        // somebody digs.
        struct Silhouette {
            int minPrecedence = 0;
            bool groundOnly   = false;

            // The cells that have anything in them at all, as a half-open range
            // of grid indices. Most of these are empty over most chunks — the ore
            // ranks describe nothing at all in a chunk of open hillside — and the
            // draw was walking every cell of every one of them to find that out.
            int firstCol = 0;
            int lastCol  = -1;
            int firstRow = 0;
            int lastRow  = -1;

            bool Empty() const { return lastCol < firstCol || lastRow < firstRow; }

            Grid field;
        };

        // Dropped whole whenever a material that occupies its vertex is written
        // into the chunk, which is the only thing any of these depend on. Liquid
        // is written every frame and is not one of those.
        mutable std::vector<Silhouette> silhouettes;
    };

    // The state a hand-edited vertex was left in.
    //
    // Not a history and not a delta — the state. A stroke leaves a vertex holding
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

        // And what was left *behind* it, on the same terms.
        //
        // A second field rather than a second record, because a wall and the block
        // in front of it are two things at one vertex and neither is the truth
        // about the other — see ElementRules::background. One record per vertex is
        // what keeps a wall from being forgotten when the block over it is dug,
        // which is exactly the case the player will hit first.
        std::optional<Element> behind;

        // Whether the record speaks for that layer at all.
        //
        // Two bits, and they carry the one distinction `std::optional` cannot: an
        // empty `element` means *dug out*, and a record that was never about the
        // front layer also has an empty one. Read as the same thing, hanging a wall
        // behind a hillside erased the hillside — the replay cleared a layer the
        // player had never touched and refilled it from a record that had nothing
        // to say about it. See World::ApplyEdits.
        bool front = false;
        bool back  = false;
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

    // The same, for the writers. Shares the remembered answer below, which is
    // what the water's write-back needs: it walks the same lattice the read did
    // and was going back to the map for every vertex of it.
    Chunk *Find(int cx, int cy) { return const_cast<Chunk *>(static_cast<const World *>(this)->Find(cx, cy)); }

    Chunk &Emplace(int cx, int cy);

    // Generating one, which is a pure function of where it is and of what has
    // been edited into it — so several can be built at once. Nothing here touches
    // the world; Settle is what puts the result into it.
    Chunk Build(int cx, int cy) const;
    Chunk &Settle(int cx, int cy, Chunk &&chunk);

    // A lattice position resolved to the chunk holding it, once.
    //
    // Every field of a chunk shares its origin and its spacing, so the local
    // index is the same for all of them: the snap, the chunk division and the
    // lookup behind them need doing once per position rather than once per
    // material. The two passes that read every material at every vertex of a
    // whole region — the light's medium and the water's read-back — were paying
    // for all three ten times over at each vertex, and between them that was more
    // than a third of the frame.
    struct Vertex {
        const Chunk *chunk = nullptr;
        Vector2 at{};      // The position, snapped to the lattice.
        int i         = 0;
        int j         = 0;
        bool resident = false; // Whether (i, j) falls inside the chunk's fields.
    };

    Vertex Resolve(Vector2 world) const;

    // One material at an already-resolved position, answered from the noise where
    // no resident chunk holds it — exactly as ValueAt does.
    float ValueAt(const Vertex &vertex, Element element) const;

    // How many times a chunk has been released.
    //
    // Find remembers the chunk it last answered with — every read of the world
    // goes through it, and the callers that matter walk a lattice, staying inside
    // one chunk for as many vertices as a chunk is wide. The map was being asked
    // the same question tens of times in a row, and a hash lookup that lands in a
    // cold bucket is most of what a lattice read costs.
    //
    // What it remembers is a pointer, which an insert cannot move —
    // unordered_map keeps its elements where they are across a rehash — but which
    // an erase can. This counts the erases, so an answer from before one is not
    // mistaken for a live chunk. It is a counter rather than a flag because the
    // memory is per thread and there is nowhere central to clear.
    long long chunkAge_ = 0;

    void ForgetRecent() { chunkAge_++; }

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
    // Files what a vertex was left holding, in one of its two layers. 
    // names the wall rather than the block — see Edit, which keeps both.
    void Remember(Vector2 vertex, std::optional<Element> element, bool behind);

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

    // The same for the layer behind, which ClearVertex deliberately leaves
    // standing — see ExcavateCell for the order the two are taken in.
    bool ClearWall(Vector2 vertex, Yield &yield);

    // The lattice vertices one stroke covers.
    //
    // Held as an index range rather than as a rectangle, because the range
    // LatticeRange derives from a cell's rectangle includes the vertex on its far
    // edge — which belongs to the cell next door. Two neighbouring cells would
    // then share a column, and clearing one would take a slice out of the other.
    struct Reach {
        // Inclusive, in whole lattice vertices from the world origin.
        int i0 = 0;
        int j0 = 0;
        int i1 = 0;
        int j1 = 0;
    };

    // Both edits a stroke can make. Placing clears the vertex first, so it is the
    // same edit as digging with a second half. `budget` bounds how many vertices
    // the placed material may newly take, and is ignored when digging.
    // `keepClear` is the region solids may not be laid in — see PlaceCell.
    Stroke ApplyStroke(const Reach &reach, std::optional<Element> place, int budget, Rectangle keepClear);

    Reach CellReach(int cx, int cy) const;

    // Takes away every vertex around `reach` that is left stopping a body without
    // drawing itself, and counts what came off into `yield`.
    //
    // Run at the end of a *dig*, and there is nowhere else it could go. A cell at the
    // contour edge holds one vertex of its nine; a player clears the cells they can see
    // around it; and the one that is left over is a full square of solid ground with
    // nothing painted anywhere in it. Measured over forty doorways dug into real
    // hillsides, better than half of them left one behind — which is the "sometimes"
    // in the report this came from.
    //
    // **Only on a dig.** Placing writes the material at the top of its range and cannot
    // leave a sliver, and the one thing it does clear is the liquid it displaces, which
    // stops no body. See `ApplyStroke`.
    //
    // A work list rather than a fixed margin, because taking a vertex away lowers what
    // its neighbours' squares sample and so can leave one of *them* drawing nothing. It
    // settles almost at once — a vertex inside solid rock has its own value and seven
    // full neighbours, so the effect damps within a step — but "almost at once" is not
    // a bound, and a fixed margin would leave the fault sitting one vertex further out.
    int Undegenerate(const Reach &reach, Yield &yield);

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
    //
    // Returns a reference into the chunk's own memory of it, which stands until
    // something writes a material into that chunk. Nothing holds one across a
    // write, and nothing should.
    const Grid &OccupancyField(const Chunk &chunk, int minPrecedence = std::numeric_limits<int>::min(),
                               bool groundOnly = false) const;

    // The same with the bounds of what is actually in it, which is what the draw
    // wants: most ranks describe nothing at all in a given chunk.
    const Chunk::Silhouette &Occupancy(const Chunk &chunk, int minPrecedence, bool groundOnly) const;

    // The lattice region the light will solve over, for a given view.
    //
    // Asked by StepLight, which solves it, and by Update, which has to have
    // generated chunks over all of it first. Two answers to that question is a
    // medium filled from the noise along its edges — see LitRegion.
    struct Lit {
        int i0   = 0;
        int j0   = 0;
        int i1   = 0;
        int j1   = 0;
        int cols = 0;
        int rows = 0;
    };

    Lit LitRegion(Rectangle view) const;

    // Cutoff each generated material's noise has to clear, measured from the
    // noise itself so that `probability` in the element table means what it says.
    // Runs the measurement to the end, which is what the constructor wants: at
    // startup there is no frame to keep drawing yet.
    void CalibrateSpawn();

    // Where the measurement has got to. Held rather than local because it is now
    // spread over frames — see BeginRebuild.
    struct Measuring {
        bool running = false;

        std::size_t material = 0; // which row of the element table is being measured
        int perAxis           = 0; // its grid, and nought where none is open
        int row               = 0; // the next row of that grid
        float feature         = 0.0f;
        float probability     = 0.0f;

        int done  = 0; // materials measured
        int total = 0; // and how many there are to measure

        std::vector<float> values; // this material's samples, gathered
        std::vector<std::vector<float>> rows; // one scratch row per worker in a block
    };

    Measuring measuring_{};

    // Opens the material at `measuring_.material`, works out its grid, and empties
    // the sample list. Skips straight past anything that is not measured.
    void OpenMaterial();

    // Takes the quantile the samples came out at and closes the material.
    void CloseMaterial();

    // And one slice of it. True when there is nothing left to measure.
    bool StepCalibration(float budget);

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

    // The whole ground at a world position, as the distance past its own
    // threshold: positive inside anything a body cannot walk through, negative in
    // open space.
    //
    // The union across materials rather than any one of them, because what buries
    // a sod is anything at all standing on it.
    float GroundValueAt(Vector2 world) const;

    // Works out the grass over a span of columns and points sodLook_/sodCover_ at
    // it. Called once a frame, before anything reads either.
    void ReadSod(Rectangle view);

    // What ReadSod worked out about one column of the plant grid, and whether it
    // has been worked out at all.
    struct SodColumn {
        float top          = 0.0f;
        float cover        = 0.0f;
        terrain::Climate climate{};
        bool known         = false;
    };

    // The band's surface, kept between frames.
    //
    // Finding it was the single most expensive thing in the frame: every column
    // walks the lattice down from the skyline asking each of the ten materials a
    // body stands on at every step, the band is wider than the view, and all of
    // it was being walked again sixty times a second.
    //
    // It is also the part of the world least likely to have moved — ground
    // changes only where somebody digs — so it is remembered per absolute column.
    // A frame walks the columns that have scrolled into the band and the ones a
    // change invalidated, and nothing else. Standing still costs nothing at all.
    std::vector<SodColumn> sodColumns_;

    // Scratch for shifting the above onto a new band, kept so the shift does not
    // allocate every frame.
    std::vector<SodColumn> sodShifted_;

    int sodColumnsFirst_ = 0;

    // Drops what is remembered about every column over a span of world x.
    void ForgetSod(float fromX, float toX);

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

    // And how it colours one standing *behind* the ground rather than in it.
    //
    // The same painter with its ramp taken down towards the dark, which is the
    // house rule for reading as behind — it is what separates the wood wall from
    // the planks it is made of, and the reasoning is written beside that material's
    // tones in element.h. Not a fade: §5.5 of CLAUDE.md needs every colour the
    // ground is drawn in to be opaque, and the backdrop goes into the very texture
    // that rule is about.
    //
    // Same grain, same bedding, same seed, so the wall of a shaft is the rock beside
    // it and not a second material that happens to be a similar colour.
    std::array<soil::Paint, kElementCount> behind_{};


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
    std::vector<sod::Look> sodLook_;
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

    // The chunks one Update found missing, and what was built for them. Kept as
    // members so that streaming does not allocate a vector of grids every frame.
    struct Coming {
        int cx = 0;
        int cy = 0;
    };

    std::vector<Coming> pending_;
    std::vector<Chunk> built_;

    // A chunk's ground, rasterised once into a texture of its own.
    //
    // The ground does not change unless somebody digs, and rasterising it was by
    // far the largest thing in the frame: every square of it sampled three times
    // over and painted with two octaves of noise, once for each material standing
    // in that chunk, sixty times a second, to draw the same picture every time.
    //
    // Held at one texel per world unit, and that is what makes the blit exactly
    // the rasterisation it replaces rather than a resampling of it: at that scale
    // a screen pixel takes the texel whose square it lands in, which is the same
    // square the rectangle would have covered it with. The five-unit squares the
    // ground is drawn in are blocks of texels inside this — see config::kPixelSize.
    struct Painted {
        RenderTexture2D texture{};

        int cx = 0;
        int cy = 0;

        bool holds    = false; // Whether it stands for a chunk at all.
        long long age = 0;     // When it was last drawn from, for eviction.
    };

    // A square of ground overruns its chunk by up to one square: the squares are
    // five units and a chunk is a hundred and ninety-two, so they do not divide,
    // and the square straddling a border is drawn by whichever chunk its middle
    // falls in. This is what gives it somewhere to land. Neighbours therefore
    // overlap here, and draw over each other without harm — the squares are
    // disjoint, so where one has something the other is clear.
    static constexpr int kPaintedMargin = 8;

    // How many chunk textures to keep. A full screen needs some eighty; the rest
    // is room for what is streaming in, so that walking does not evict a chunk
    // that is about to be wanted again.
    static constexpr int kPaintedSlots = 192;

    std::vector<Painted> painted_;
    std::unordered_map<std::int64_t, int> paintedOf_;

    // The chunks of grass one frame draws, and the fields derived for them.
    // Members so that a frame does not allocate a grid per chunk.
    mutable std::vector<const Chunk *> sodFields_;
    mutable std::vector<Grid> sodGrids_;

    long long paintedAge_ = 0;

    // How much of its own light a material keeps when it is standing behind the
    // ground rather than in it.
    //
    // A little over half, which is Minecraft's figure for the same job and is
    // settled by the same argument. Seamless is not the aim and cannot be: the
    // backdrop is lit by the same daylight as the hillside in front of it, so at
    // the same tone a pit dug at noon would be a hole nobody could see. Far
    // enough down that the eye reads depth, near enough that it is plainly the
    // same rock.
    static constexpr float kBehindShare = 0.55f;

    // How far below the generator's skyline the backdrop starts.
    //
    // The land is described one lattice column at a time and the ground is drawn
    // from a contour that crosses half a step out from the last filled vertex, so
    // the two do not agree to the pixel. A backdrop that started a pixel high
    // would show as a comb of dark teeth along every hilltop, against the sky it
    // is meant never to be seen against.
    //
    // Erring the other way is nearly free: the run this sinks past is inside the
    // ground, which is drawn over it opaquely, and the few pixels of it that a
    // fresh cut exposes are covered by the flat fill DrawUnderground lays down
    // in the same colour.
    static constexpr float kBehindSink = 4.0f;

    // Paints the country behind the ground over `covered`: the land as the
    // generator would have it with nothing dug out of it.
    //
    // The generator's land and only part of it — the rock and whatever covers lie
    // on it, and nothing else. No ore, because a seam is a thing to find by
    // digging and a backdrop full of them is a map of where to dig; no water,
    // because a level is a thing that moves and this is a picture that does not;
    // and no caves, because a cave drawn behind a cave is a hole with a hole
    // behind it. What is behind the world is the plain country, which is exactly
    // what a wall of rock is.
    //
    // Drawn into the chunk texture, first, so it is behind the walls and behind
    // the ground and costs the frame one blit that was happening anyway.
    //
    // `own` is the chunk's own span **without** the margin, and that is the whole
    // of what keeps two neighbours from fighting over the strip between them. The
    // ground gets away with painting into the margin because its squares are
    // disjoint — a square belongs to whichever chunk its middle falls in, so a
    // neighbour drawing over the same strip has nothing there to draw. A backdrop
    // fills every square it is given, so a margin painted by both is the second
    // chunk laying plain rock over the first one's ground: measured, a dark band
    // down every chunk border, the width of two margins.
    void PaintBackdrop(Rectangle own) const;

    // Rasterises one chunk into a slot.
    void PaintChunk(Painted &slot, int cx, int cy);

    // Forgets the picture of a chunk whose ground has changed.
    void DropPainted(int cx, int cy);

    // The slot holding a chunk, or nothing.
    const Painted *PaintedFor(int cx, int cy) const;

    water::Settings waterSettings_;

    // What the liquid held before the step, so that the write-back can tell which
    // vertices the step actually moved.
    //
    // The band is a screen and a half across and nearly all of it is dry rock and
    // open air. Writing every vertex of it regardless meant a lattice walk and a
    // chunk lookup for a hundred thousand vertices a step, to store the same zero
    // that was already there — and the step runs several times in a slow frame,
    // which is exactly when it can least afford to.
    std::vector<float> settled_;

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

    bool skyCover_ = true;

    light::Settings lightSettings_{};

    // The two ends of the day, and the point of it the sky is at now. The last is
    // derived every step and is the only one the solve reads.
    light::Radiance dayLight_{2.6f, 2.8f, 3.1f};
    light::Radiance nightLight_{0.05f, 0.06f, 0.09f};
    light::Radiance skyLight_{2.6f, 2.8f, 3.1f};

    light::Medium medium_;
    light::Field lightField_;

    weather::Sky sky_;
    vista::Range vista_;

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

    // Whether Skyline may add to that record.
    //
    // Cleared while the grass band is walked across the cores. The record is a
    // hash map, and one thread inserting into it while the others are reading is
    // a race — so the columns the walk will ask about are put in first, in order,
    // and the walk itself only reads.
    bool skylineWritable_ = true;

    // The scan itself, without the record. Pure, so it is safe from any thread.
    float ScanSkyline(int column) const;

    // Fills the record for a run of columns, so that a walk across the cores
    // finds every answer already there.
    void WarmSkyline(int firstColumn, int lastColumn);

    // The ground under one frame's rain. Kept between frames only so that drawing
    // does not allocate; nothing in it survives the call that fills it.
    mutable std::vector<float> surface_;
};
