#pragma once

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

// One family of tunnels: a band carved around the zero set of a noise field.
//
// Thresholding a field gives blobs, because the region where a field exceeds a
// value is a set of patches. The *zero set* of a field is something else
// entirely: a family of long closed curves. Carving a band around it therefore
// gives corridors, and two families with different shapes cross each other,
// which is what turns corridors into a network with loops in it.
//
// Loops are the reason to build it this way. A loop means a route back that is
// not the route in, and that is the difference between exploring a cave and
// retracing one.
struct TunnelLayer {
    NoiseShape shape{};

    // Half-width of the corridor in pixels where the layer begins, so the full
    // opening is twice this. Zero switches the layer off.
    float width = 0.0f;

    // Half-width it approaches far underground, and the depth over which it gets
    // there. Tunnels opening out as they descend is what makes the way down feel
    // like a way down.
    //
    // Approached rather than grown into: the world has no floor, so a width that
    // rises with depth without a limit ends up carving away everything below a
    // certain point. Leave it at zero to keep the width constant.
    float widthAtDepth = 0.0f;
    float growthDepth  = 1200.0f;
};

// What is subtracted from the rock.
struct CaveSettings {
    // Depth below the surface within which no cave may open at all, and the
    // distance below that over which they ramp up to full width.
    //
    // This is what keeps the ground solid. Without it the cave layers reach the
    // surface and leave it perforated, which is the failure the old generator
    // was made of.
    float crust     = 56.0f;
    float crustFade = 72.0f;

    // How honeycombed the underground is, region by region. Very low frequency,
    // so a region is somewhere the player notices arriving in.
    NoiseShape region{};

    // Share of the underground that has caves at all, in [0,1]. A probability,
    // not a cutoff: the cutoff achieving it is measured from the field by
    // Calibrate, so reshaping the region does not change how much of the world
    // is hollow.
    float regionCoverage = 0.5f;

    // Distance in noise units over which a region fades into solid rock.
    // Sharpens or softens the border between cave country and dead rock.
    float regionFade = 0.15f;

    // The rooms. Isotropic and low frequency, since a chamber has no direction;
    // what gives it a way in and out is the tunnels crossing it.
    NoiseShape chamber{};

    // Share of the eligible underground hollowed into chambers, in [0,1], on
    // the same measured basis as `regionCoverage`.
    float chamberCoverage = 0.0f;

    // Pixels of rock removed at the centre of a chamber, tapering to nothing at
    // its edge. Sets how tall a room is.
    float chamberDepth = 48.0f;

    // Long horizontal halls. Stretched sideways so they can be walked.
    TunnelLayer galleries{};

    // Share of their width the halls keep where the region says solid rock.
    //
    // Deliberately not zero. The halls are the only layer that guarantees the
    // underground is connected at all, so a route has to survive even the
    // stretches that are mostly rock; at zero, a region border becomes a wall
    // there is no way past except to dig one.
    float galleryFloor = 0.45f;

    // Narrow links between the halls. Regional, so their density varies.
    TunnelLayer crawlways{};

    // The way in from the surface: the only layer allowed through the crust.
    // Stretched vertically, so it descends rather than wanders.
    TunnelLayer shafts{};

    // Depth a shaft reaches before it pinches shut. Has to clear
    // crust + crustFade, or an entrance opens onto solid rock and leads
    // nowhere.
    float shaftReach = 320.0f;

    // Cutoffs measured from the fields themselves so the coverage figures above
    // mean what they say. Filled by Calibrate; never written by hand.
    struct Calibration {
        float region  = 1.0f;
        float chamber = 1.0f;
    };

    Calibration calibration{};
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
