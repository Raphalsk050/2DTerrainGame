#pragma once

#include "cave.h"
#include "grid.h"
#include "raylib.h"

#include <vector>

// Procedural terrain generation. This module operates purely on CPU-side data;
// it creates no textures and issues no draw calls.
//
// Sampling is a pure function of world position, so any region can be generated
// on its own and neighbouring regions agree along their shared border without
// exchanging any state.
//
// The world is not one noise field thresholded into rock. It is a stack of
// layers applied in a fixed order, each owning exactly one decision:
//
//   Height(x)      the ground surface, from one-dimensional noise alone
//   Depth(x, y)    signed distance below that surface
//   cave layers    signed bands subtracted out of the rock
//   Density        all of it, mapped into the [0,1] field the world stores
//
// The value passed between layers is a signed distance in pixels: positive
// inside rock, negative in open air, zero on a surface. Pixels rather than raw
// noise units is what makes the settings below readable, since a corridor is
// forty pixels tall because the number says forty.
namespace terrain {

// Span in world pixels over which `frequency` counts features. Fixing it here
// keeps the noise scale independent of how the world is partitioned.
inline constexpr float kFeatureSpan = 1000.0f;

// Field value the ground surface sits at, and so the value rock has to be
// thresholded against. Declared here rather than in the element table because
// it is a property of the field this module produces; the table refers to it.
inline constexpr float kSurfaceLevel = 0.45f;

// Pixels of signed distance mapped onto the full swing of the field.
//
// The field is stored in [0,1] and thresholded, but what the layers compute is a
// distance, so the two have to be related by something. Small enough that the
// contour has a steep gradient to interpolate through near the surface, wide
// enough that a material clamped against the rock (an ore vein) still has a few
// pixels of slope to be clamped over.
inline constexpr float kDensitySpan = 24.0f;

// Shape of a noise field, independent of what it is used for. Every layer of
// the terrain and every ore vein describes itself with one of these.
struct NoiseShape {
    float frequency = 4.0f;  // Number of features per kFeatureSpan pixels. Low
                             // values give few broad shapes, high values many
                             // small ones.
    int octaves = 4;         // Layers summed together. 1 is smooth; 4-6 gives a
                             // rugged outline with fine detail on top.
    float lacunarity = 2.0f; // Frequency multiplier per octave.
    float gain       = 0.5f; // Amplitude multiplier per octave.

    // Aspect of one feature. Above one the field is stretched horizontally, so
    // its shapes run sideways; below one they run up and down.
    //
    // For a cave layer this is the difference between a corridor a character
    // can walk along and a hole it can only fall down, which makes it the most
    // consequential number in the cave settings. An isotropic cave field is
    // unwalkable by construction.
    float aspect = 1.0f;

    float offsetX = 0.0f; // Sampling offset. Scrolls the field without
    float offsetY = 0.0f; // changing its shape.

    // Depth into the field, on the third axis of the noise underneath.
    //
    // The world is flat, so nothing generated in it has a third dimension and
    // this stays at zero for every layer of the terrain. What it buys is the
    // one thing the other two offsets cannot: moving along it does not scroll
    // the field, it *changes* it, the same field seen at another depth. That is
    // the difference between a cloud that arrives and a cloud that forms, and
    // it costs nothing — the noise interpolates the corners of a cube whether
    // this is zero or not.
    float offsetZ = 0.0f;

    int seed = 0; // Equal seeds yield identical fields.
};

// Continuous noise value in [0,1] at a world position.
float Sample(Vector2 world, const NoiseShape &shape);

// The same field before it is folded into [0,1], so its zero set is reachable.
float Signed(Vector2 world, const NoiseShape &shape);

// Cellular noise, in [0,1]: the distance from the position to the nearest of a
// scattered set of feature points, one per cell of the lattice.
//
// Zero at a feature point and rising towards the walls between them, so where fbm
// swells and sags this bulges — a field of packed rounded cells. Inverted, it is
// the other half of the standard recipe for cloud: Perlin gives the drift and the
// large shape, Worley gives the bulge, and the two mixed give a cauliflower. At a
// high frequency it is also what erodes a smooth outline into a rim of small lobes.
//
// Costs a look at nine cells, which is about what one octave of Perlin costs.
float Worley(Vector2 world, const NoiseShape &shape);

// Folded noise, in [0,1]: each octave contributes its magnitude rather than its
// value.
//
// Folding puts a crease wherever the field crosses zero and a rounded dome
// wherever it peaks, so what comes out is a mass of bulges pressed together
// instead of the smooth swells of Sample. That is the shape of a cauliflower, and
// therefore of a cumulus cloud — it is what the octaves of a real one look like,
// each puff carrying smaller puffs on its surface.
float Billow(Vector2 world, const NoiseShape &shape);

// The ground surface.
//
// A function of the horizontal position alone. That is the whole point: a
// function of one variable has exactly one value per column, so the ground
// cannot have a hole in it. Anything that needs to vary vertically is a layer
// of its own further down.
struct SurfaceSettings {
    // World Y the ground sits at where every modifier below is neutral. Y grows
    // downward, so a smaller number is higher ground.
    float level = 144.0f;

    // Where the land is broadly high and where it is broadly low. Very low
    // frequency, so one feature spans several screens and reads as a region
    // rather than as a hill.
    NoiseShape relief{};
    float reliefAmplitude = 0.0f;

    // The shape actually walked over.
    NoiseShape hills{};
    float hillAmplitude = 0.0f;

    // Texture underfoot. Cheap to sample and easy to overdo: this is the term
    // that turns a walkable slope into a staircase of one-pixel steps.
    NoiseShape detail{};
    float detailAmplitude = 0.0f;

    // How flat the land is, in [0,1], with one dead towards zero. Scales the
    // hill and detail amplitudes together, so a stretch of world comes out as
    // plain or as mountain without either being written into their amplitudes.
    NoiseShape erosion{};

    // Share of the hill and detail amplitude a fully eroded stretch keeps. Zero
    // gives dead-level plains; near one switches erosion off.
    float erosionFloor = 1.0f;

    // Snaps heights towards multiples of `terraceStep`, by this share of the
    // distance. Zero leaves the slope alone; one gives a hard staircase.
    //
    // What it buys is flat ground on a slope. A continuous incline in a
    // side-scroller is walkable but featureless; ledges give somewhere to
    // stand, and the risers between them read as cliffs.
    float terrace     = 0.0f;
    float terraceStep = 32.0f;

    // How hard the snap is, as the steepness of the riser between two ledges.
    //
    // One leaves the slope untouched however high `terrace` is set; larger
    // numbers flatten the ledges and steepen the climb between them, and in the
    // limit it is the hard staircase that rounding to the nearest ledge gives.
    //
    // It exists because rounding is a step function, and a step function has no
    // width. The surface is read one lattice column at a time by everything that
    // stands on it or lights it, and a riser crossed between two columns is a
    // vertical cliff to all of them: measured over six and a half thousand
    // columns the ground moved 1.1 px from one to the next on average and then
    // jumped **thirteen** at a ledge, with one column in twenty-two doing it. What
    // that drew was a sawtooth of black wedges along the underside of the daylight,
    // because the light is solved per column and cannot follow a surface that is
    // not there.
    //
    // A riser has to be climbed over a few columns rather than none. That is the
    // whole of what this does.
    //
    // Two, measured. Over the same six and a half thousand columns: the ground's
    // own roughness with no terrace at all peaks at 6.5 px between neighbours and
    // clears a texel in one column in five hundred, and that is the floor nothing
    // can go below. At two the terrace costs 8.3 px and one in two hundred and
    // fifty; at four it costs 10.7 px and one in fifty-five. Past about two and a
    // half the ledges stop being worth what the risers do to everything reading
    // the surface a column at a time.
    float terraceSharp = 2.0f;

    // Horizontal displacement of the position the surface is read at, taken
    // from a field that varies with depth as well as with distance. Because the
    // displacement differs from one height to the next, the surface folds, and
    // a folded surface has overhangs.
    //
    // Off by default. It is the one layer that can put a hole back in the
    // ground: pushed past roughly half the wavelength of the hills the fold
    // closes on itself and leaves rock hanging in the air.
    NoiseShape warp{};
    float warpAmplitude = 0.0f;

    // Depth over which the displacement dies away, so only the surface folds
    // and the rock well beneath it is not sheared sideways.
    float warpDepth = 96.0f;
};


// The irregularity of a cave wall.
//
// The zero set of a smooth field is a smooth curve and the band around it is a
// smooth tube, so a corridor built from one alone has the walls of a cast pipe —
// which is what the eye reads as procedural even when the route itself is good.
//
// Folding a field puts a crease wherever it crosses zero and a dome wherever it
// peaks, so adding a folded high-frequency field back into the rock gnaws the
// wall into alcoves and nubs. Cheap, and it is the one term that acts on every
// layer at once, because it is applied to the finished distance rather than to
// any one of them.
struct RoughnessSettings {
    NoiseShape shape{};

    // Pixels the wall moves either way at full strength.
    float amplitude = 0.0f;

    // Value of the folded field the wall sits at, so that as much of it is bitten
    // out as is left standing. Not derivable: the mean of a folded sum of octaves
    // is not the mean of one, and the analytic figure is well off what the field
    // does. Measured, in the manner of kFbmPeak.
    float bias = 0.36f;

    // The lobes, and they are a different shape of irregularity from the fold
    // above rather than more of it.
    //
    // A folded fbm creases where it crosses zero, which frets the wall — it makes
    // an edge that is everywhere slightly wrong. What it does not make is
    // *features*: the scallops and alcoves that a real wall is built out of, each
    // one a rounded bite of a definite size. Cellular noise is exactly that shape
    // — it is a field of packed rounded cells, and subtracted from the rock along
    // an outline it takes those cells out of it in bites.
    //
    // This is the pointwise stand-in for the cellular-automaton pass every 2D
    // cave generator reaches for. An automaton makes an outline organic by
    // repeatedly asking each cell what its neighbours are doing, which coalesces
    // the edge into rounded lumps; it also needs a grid and several passes over
    // it, and the whole generator here rests on being a pure function of one
    // position. Worley arrives at the same rounded-lump outline in one sample,
    // because the lumps are what its cells already are.
    NoiseShape lobes{};
    float lobeAmplitude = 0.0f;

    // Share of a cell the lobe reaches into. Zero puts the bite at the cell's own
    // centre and nowhere else; one has the bites meeting, and the wall is scalloped
    // edge to edge.
    float lobeBite = 0.5f;

    // Pixels either side of a surface the roughness acts over, fading to nothing.
    //
    // Bounded because the term is added to the whole field: applied everywhere it
    // would put pockets of air in the middle of solid rock and pillars of rock in
    // the middle of open air. What it is for is the outline, and the outline is
    // only ever a few pixels wide.
    float reach = 20.0f;
};


// What is taken out of the rock.
//
// The systems themselves are `cave::Settings` — see cave.h for why they are dug
// by a walking agent rather than thresholded out of a field. What is left here
// is everything about *where* they are allowed to be and what their walls look
// like once they are cut, which is this module's business rather than theirs.
struct CaveSettings {
    // Depth below the surface within which no system may lie, and the distance
    // below that over which they come in.
    //
    // This is what keeps the ground solid. Only an entrance may cross it, and it
    // crosses deliberately.
    float crust     = 110.0f;
    float crustFade = 72.0f;

    // The systems.
    cave::Settings systems{};

    // How honeycombed the underground is, region by region, and the share of it
    // that has caves at all — near the surface and far below it.
    //
    // Two figures because rarity is only ever felt at the surface, while depth is
    // where the volume belongs. One number cannot say both. This is Terraria's
    // split between the Underground and the Cavern layer, and it is a decision
    // about depth rather than about chance.
    //
    // It gates whether a *cell* holds a system, so a region that says no is
    // genuinely solid rather than holding thinner caves.
    NoiseShape region{};
    float regionCoverage        = 0.5f;
    float regionCoverageShallow = 0.5f;
    float regionDeepens         = 1600.0f;

    // The irregularity applied to every wall once the systems are cut.
    //
    // A swept circle is smooth, and a corridor built out of them has an outline
    // made of arcs. This is what breaks that up, and it is the pointwise stand-in
    // for the cellular-automaton pass every 2D cave generator runs over its dug
    // layout: an automaton makes an outline organic by repeatedly asking each
    // cell what its neighbours are doing, which needs a grid and several passes
    // over it, and this generator is a pure function of one position.
    RoughnessSettings roughness{};

    // Cutoffs measured from the region field so the coverage figures mean what
    // they say. Filled by Calibrate; never written by hand.
    struct Calibration {
        float region        = 1.0f;
        float regionShallow = 1.0f;
    };

    Calibration calibration{};
};

// Groundwater.
//
// Water underground is not a scattering of pools, it is a *level*. A stretch of
// world has a height its groundwater stands at, and what is wet is whatever cave
// space lies below that height — which is why one cave is flooded to a flat
// surface and the next one along is bone dry, and why neither has a puddle
// hanging halfway up a wall.
//
// Built this way it is also already in equilibrium, and that is not a detail. A
// pool placed as a blob of mass is a shape the liquid automaton immediately
// tears down, so it collapses the moment it is simulated, again every time the
// chunk is rebuilt — which is the whole of why water appeared to move about when
// the player walked back into a place. Water generated below a level has nowhere
// to fall to, so it arrives settled and comes back identical.
struct AquiferSettings {
    // Where the table stands: this far below the nominal surface level, moved up
    // or down by `swing` from a field of its own.
    //
    // Absolute rather than measured from the ground overhead, because a surface
    // that followed the hills would not be level either.
    //
    // There is no second field deciding *whether* a stretch of world has an
    // aquifer, and that absence is the design. Anything that switches the water
    // off over one stretch and on over the next puts a boundary somewhere, and a
    // boundary that falls inside a cave is a vertical wall of water with nothing
    // holding it up — which the automaton pulls down the instant it runs, which is
    // the very fault this replaced. A table that is always present and merely
    // sometimes deeper than the deepest cave has no boundary to fall anywhere.
    //
    // `level.frequency` therefore has a hard requirement on it, and it is not a
    // matter of taste: the table has to be flat over the width of a cave. The
    // field moves `swing` pixels in half a feature, so the surface tilts by about
    // `2 * swing / (kFeatureSpan / frequency)` pixels per pixel travelled, and
    // over a few hundred pixels of cave that has to come to less than the lattice
    // step or the water arrives sloped and immediately flows. In practice this
    // means a feature tens of thousands of pixels across — which is what a
    // regional water table is: it varies between one part of a country and
    // another, not between one end of a cave and the other. `--caves` measures
    // the tilt and prints it.
    NoiseShape level{};
    float depth = 1400.0f;
    float swing = 1000.0f;

    // Height the table is snapped to a multiple of, in pixels.
    //
    // This is what actually makes it level, and the frequency above is what makes
    // the snapping cheap. A continuous field cannot be both varying and flat: to
    // move `swing` pixels it has to slope somewhere, and any slope at all puts the
    // whole surface out of equilibrium along its length. Snapping removes the
    // slope outright — the table is exactly constant, and changes by a whole step
    // at the few places the field underneath crosses a boundary.
    //
    // Those crossings are the one bad case, since a step falling inside a cave is
    // a ledge in the water. They are as rare as the field is slow: at a tilt of a
    // hundredth of a pixel per pixel a step of this size turns up once every few
    // thousand, so the odds of one landing in any given cave are small and the
    // fault when it does is one step and not a wall. `--caves` prints the tilt.
    float step = 48.0f;
};

// The groundwater over a stretch of world.
struct WaterTable {
    float level = 0.0f; // World Y its surface stands at. Open space below is wet.
};

// The climate of a place: the two axes a biome will be chosen on.
//
// Functions of the horizontal position alone, like the surface itself, and very
// low frequency: a climate is a region travelled into, not something that changes
// between one step and the next.
//
// Nothing about the shape of the world reads these. They are here because the sky
// does — cloud gathers over wet ground and over high ground — and because they are
// half of what a biome table will be selected on.
struct ClimateSettings {
    NoiseShape temperature{};
    NoiseShape humidity{};

    // Humidity gained per pixel of elevation above `SurfaceSettings::level`.
    //
    // Air pushed up over a rise cools, and cool air holds less water, so it
    // condenses out: high ground is wetter than the plains beside it and stands in
    // cloud on a day the plains are clear. One term, and it ties the weather to
    // the shape of the land without either having to know about the other.
    float humidityLift = 0.0f;

    // Temperature lost per pixel of that same elevation. The other half of the
    // same physics, and what a snow line will eventually be read off.
    float temperatureLapse = 0.0f;
};

struct Climate {
    float temperature = 0.5f; // 0 frozen, 1 scorching.
    float humidity    = 0.5f; // 0 arid, 1 lush.
};

// Tunable parameters of the whole generator.
struct Settings {
    SurfaceSettings surface{};
    CaveSettings caves{};
    AquiferSettings aquifer{};
    ClimateSettings climate{};

    // Added to every layer's own seed, so one number reseeds the entire world
    // while the layers stay decorrelated from each other. Sharing one seed
    // outright would make the caves follow the outline of the hills.
    int seed = 0;
};

// Measures the cutoffs the coverage figures ask for. Call once after authoring
// the settings and before generating anything; the results are stored in
// `settings` so that sampling stays a pure function of it.
void Calibrate(Settings &settings);

// World Y of the ground surface in a column, before any fold is applied.
float Height(float worldX, const Settings &s);

// Climate of a column, elevation included.
Climate ClimateAt(float worldX, const Settings &s);

// The groundwater over the span a position falls in.
//
// A function of the horizontal position alone, like the surface itself, and for
// the same kind of reason: a level has one value per place, and anything that
// varied with height would not be one.
WaterTable TableAt(float worldX, const Settings &s);

// Signed distance below the surface in pixels, fold included: positive
// underground, negative in the open air.
float Depth(Vector2 world, const Settings &s);

// Signed distance into rock in pixels, caves subtracted: positive inside rock,
// negative in open air, zero on whichever surface is nearest.
float Solidity(Vector2 world, const Settings &s);

// Rock density in [0,1]. This is the value stored in the field, so the contour
// can interpolate through it, and it crosses kSurfaceLevel exactly where
// Solidity crosses zero.
float Density(Vector2 world, const Settings &s);

// Both answers about the ground at a position, taken together.
//
// The density is what the field stores; the depth is what a material whose band
// is measured from the surface has to be placed against. They are asked for at
// once because the depth is already worked out on the way to the density, and
// the surface it comes from is much the most expensive part of either — eight
// octaves for a number that would otherwise be computed twice at every vertex of
// every chunk.
struct Ground {
    float density; // Rock density in [0,1], exactly as Density reports it.
    float depth;   // Signed distance below the surface in pixels, fold included.

    // Signed distance into the rock in pixels, caves subtracted: what the density
    // is made from, before it is mapped into [0,1] and clamped there.
    //
    // Carried because the clamp destroys it. The field only has to resolve the
    // few pixels either side of a contour, so kDensitySpan is small and the
    // density saturates about thirteen pixels inside the rock — which is fine for
    // drawing an edge and useless for any question about how deep in the rock
    // something is. Anything that wants that distance has to read it here.
    //
    // Free: SolidityBelow computed it on the way to the density.
    float solid;
};

Ground SampleGround(Vector2 world, const Settings &s);

// Density thresholded into ground or air. The threshold is passed in rather
// than stored here: it belongs to the element the field represents, and keeping
// a second copy alongside the noise settings is what let drawing and collision
// disagree about where the ground was.
bool IsSolid(Vector2 world, const Settings &s, float threshold);

// Fills every sample of a block from its own world position.
void Fill(Grid &grid, const Settings &s);

// Continuous field over a world region, for inspection and debug rendering.
struct Field {
    int cols = 0;
    int rows = 0;
    std::vector<float> value;

    float At(int i, int j) const { return value[i * rows + j]; }
};

Field Generate(const Settings &s, Vector2 origin, int cols, int rows, int spacing);

// Renders the field as a greyscale image. The caller owns the result and must
// release it with UnloadImage.
Image ToImage(const Field &field);

} // namespace terrain
