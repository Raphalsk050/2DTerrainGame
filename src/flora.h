#pragma once

#include "item.h"
#include "raylib.h"
#include "terrain.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

// The plants the world grows, and where they stand.
//
// Placement is a pure function of world position, exactly as the ground under it
// is. A tree is not an object that was made and put somewhere: it is the answer
// to what grows in one cell of the horizontal axis, and its species, size,
// variant and exact position are hashed out of the index of that cell. So a
// forest costs nothing to keep, a stretch of world walked away from and returned
// to is the same wood, and two chunks generated on their own agree about the
// tree standing across the seam between them without exchanging anything.
//
// The scatter is one plant per cell, jittered inside it. The jitter is bounded
// by the cell less the plant's own width, which makes overlap a matter of one
// constant rather than of a test: at an interlock of zero a canopy cannot leave
// its cell and no two of them can touch, and raising it lets neighbours reach
// into each other by that share of a width. That is the whole difference between
// an orchard and a thicket, and it is why the scatter needs no neighbourhood
// scan, no ordering between cells and no halo — the same reason the stars and
// the raindrops are scattered this way.
//
// What is not decided here is anything that has happened. A tree that has been
// cut, damaged or planted is remembered by Grove; this module only ever answers
// what the world would grow if nobody had touched it.
namespace flora {

// Stands in for a regrowth that never comes, at a magnitude no clock reaches.
inline constexpr float kNever = 1.0e9f;

// Which pass a plant belongs to.
//
// The two are scattered separately rather than together because their sizes are
// an order apart: one pass spanning both would have to size its cell to the
// widest tree, and then most cells would hold a fern with a tree's worth of
// empty ground around it.
enum class Layer { Canopy, Undergrowth, Count };

enum class Species { Oak, Pine, Birch, Apple, Count };

// How far along a plant is. Growth moves between these; the world's untouched
// plants are all at the last one, since a wood nobody has cut is a mature wood.
enum class Stage { Sapling, Young, Mature, Old, Count };

// Ordered so that the index is also the turn of the year. Nothing supplies one
// yet — weather::Sky answers Spring until there is a calendar — but the palettes
// are authored for all four so that turning seasons on is a change to what is
// asked, not to what exists.
enum class Season { Spring, Summer, Autumn, Winter, Count };

inline constexpr std::size_t kLayerCount   = static_cast<std::size_t>(Layer::Count);
inline constexpr std::size_t kSpeciesCount = static_cast<std::size_t>(Species::Count);
inline constexpr std::size_t kStageCount   = static_cast<std::size_t>(Stage::Count);
inline constexpr std::size_t kSeasonCount  = static_cast<std::size_t>(Season::Count);

inline constexpr std::size_t LayerIndex(Layer layer) { return static_cast<std::size_t>(layer); }
inline constexpr std::size_t SpeciesIndex(Species species) { return static_cast<std::size_t>(species); }
inline constexpr std::size_t StageIndex(Stage stage) { return static_cast<std::size_t>(stage); }
inline constexpr std::size_t SeasonIndex(Season season) { return static_cast<std::size_t>(season); }

// Every plant is built from its own seed, so no two of them are the same tree.
//
// This was a fixed set of variants baked once, and it was wrong: a screenful of
// birches came out as three drawings repeated, which is exactly what a procedural
// wood must not be. A plant's shape is a pure function of the cell it grew in, so
// there is no reason to have a small number of them — the sprites are rasterised
// one per tree as it comes into view and the slot is recycled when it leaves.
//
// It also hands back the size. A sprite drawn at a fraction of the size it was
// baked at puts fractional texels on screen; one baked at its own size never
// does, so the size can be continuous again.

// How one mass of foliage is shaped.
//
// The two are not a broadleaf and a conifer by name but by construction, which
// is what lets a species be either without the rasteriser learning any species:
// a clump is a rounded blob hung off the end of a branch, a frond is a wide tier
// that droops away from the trunk and is notched along its lower edge.
enum class Crown { Clump, Frond };

// The silhouette, as the numbers needed to build one.
//
// A parametric outline rather than a grammar. At the size a tree is drawn here
// the extra freedom of an L-system lands below the pixel that would show it, and
// it can fail: a grammar produces the occasional tangle, which then has to be
// recognised and thrown away, so the generator arrives with a rejection test
// attached. A handful of numbers cannot fail.
struct SpeciesShape {
    // Share of the total height the bare trunk takes before the first foliage. A
    // conifer carries branches most of the way down; an oak holds its crown well
    // clear of the ground.
    float clearance = 0.35f;

    // Share of the total height the trunk itself reaches. A conifer runs a
    // single leader nearly to its tip, so the crown is hung off a mast; a
    // broadleaf stops partway up and the crown carries on above it.
    //
    // Written rather than derived from the taper below. The two do move
    // together — the shapes that keep a leader are the shapes that narrow to a
    // point — but a number that can be read is worth more here than one that has
    // to be decoded.
    float trunkReach = 0.6f;

    // Width of the trunk at its base, as a share of the canopy width, and the
    // share of that left at the top. Below one the trunk tapers.
    float trunkWidth = 0.14f;
    float trunkTaper = 0.55f;

    // How far the trunk wanders off vertical over its length, in shares of its
    // own width. Zero is a mast; a broadleaf leans.
    float lean = 0.6f;

    // Masses of foliage down the crown, and how far the widest of them reaches
    // from the axis as a share of the crown's half width.
    int tiers    = 6;
    float reach  = 1.0f;

    // Share by which a tier's reach falls off towards the top. This one number
    // is the whole difference between the two shapes: near one the crown is a
    // column of equal masses and reads as a broadleaf, well below one it is a
    // stack of shrinking wedges and reads as a conifer.
    float taper = 0.55f;

    // How far a tier may wander from where the taper puts it, as a share of the
    // crown's half width. What keeps three trees of one species from being three
    // copies of one tree.
    float jitter = 0.22f;

    Crown crown = Crown::Clump;

    // How ragged the edge of the foliage is, as a share of a mass's own radius,
    // and how deep the gaps torn out of the middle of a crown go.
    //
    // The pair that stops a canopy reading as a blob. A mass drawn from its
    // falloff alone comes out a smooth ellipse whatever it is made of; the first
    // of these breaks its outline into the notches a drawn tree has, and the
    // second opens the holes that let the branches and the sky behind show
    // through it.
    float ragged = 0.30f;
    float gaps   = 0.34f;

    // Share of the way from the trunk to a mass's centre that its branch is
    // drawn along. Below one the branch stops inside the foliage, which is what
    // hides its join; at one it reaches the middle of the mass and shows.
    float branchReach = 0.78f;
};

// Where a species grows, as a place in the two climate fields rather than as a
// list of biomes.
//
// A centre and a width per axis, read as a bell: densest at the centre, thinning
// to nothing about a width away. That is enough to give a wood its composition
// today, and it is the same shape a biome table will multiply into when there is
// one — a biome row supplies weights, and a weight multiplies a suitability that
// already exists rather than replacing it.
struct SpeciesClimate {
    float temperature      = 0.5f;
    float temperatureWidth = 0.25f;
    float humidity         = 0.5f;
    float humidityWidth    = 0.25f;

    // World Y the species stops at, and the distance below it over which it
    // thins out to that stop. Y grows downward, so the smaller ceiling is the
    // species that climbs higher — which is why the pine's is above the oak's.
    float ceiling     = 60.0f;
    float ceilingFade = 90.0f;

    // Share of the eligible ground this species takes where it is most at home,
    // before the forest field thins it any further. What separates a tree that
    // fills a wood from one met occasionally in it.
    float abundance = 1.0f;
};

struct SpeciesGrowth {
    // Weather minutes from a planted seed to a mature tree under average light
    // and rain. Measured in the same clock the day and the weather run on, so it
    // runs fast under F7 with everything else.
    float maturityMinutes = 16.0f;

    // Weather minutes after felling before the stump is gone and the species
    // stands again. kNever leaves the stump where it fell.
    float regrowMinutes = 12.0f;

    // How much of the growth rate hangs on light and on water, in [0,1]. At zero
    // the species grows at its own pace wherever it is; at one it stops entirely
    // in the dark or in the dry.
    float lightNeed = 0.5f;
    float waterNeed = 0.5f;

    // Hits a mature tree takes before it comes down. Scaled by the stage, so a
    // sapling goes in one.
    float toughness = 4.0f;
};

struct SpeciesPalette {
    // Bark, from the shadowed side to the lit one.
    Color barkDark;
    Color bark;
    Color barkLight;

    // Foliage, darkest first. Four tones is what a canopy this size can carry: a
    // mature crown is some forty art pixels across, and a fifth tone would take
    // a band of it narrow enough that most of the crown would hold none of it.
    Color leaf[4];
};

struct DropRule {
    Item item   = Item::Wood;
    int least   = 0;
    int most    = 0;
    float chance = 0.0f;
};

inline constexpr std::size_t kDropRules = 3;

struct SpeciesDef {
    const char *name;
    Layer layer;

    // Base to crown, and the widest the crown gets, both in world pixels at
    // full size. Written per stage rather than derived from a maturity, because
    // a sapling is not a small tree — it is a different shape, and the pair of
    // numbers is where that is said.
    float height[kStageCount];
    float canopyWidth[kStageCount];

    SpeciesShape shape;
    SpeciesClimate climate;
    SpeciesGrowth growth;

    SpeciesPalette palette[kSeasonCount];
    DropRule drops[kDropRules];

    // Stands bare in winter. A conifer does not, which is the one thing that
    // distinguishes the two through the cold half of the year.
    bool deciduous;
};

// The plants, each one row.
//
// Sizes are in world pixels and can be read against the character, which is 26
// tall and 12 wide: a mature oak is five of it, a mature pine is six and a half.
inline constexpr SpeciesDef kSpecies[] = {
    {
        .name        = "oak",
        .layer       = Layer::Canopy,
        .height      = {34.0f, 78.0f, 130.0f, 158.0f},
        .canopyWidth = {24.0f, 62.0f, 105.0f, 126.0f},
        .shape =
            {
                // A crown held well clear of the ground on a leaning trunk, and
                // tiers that barely shrink towards the top: the rounded mass of
                // the broadleaf.
                .clearance  = 0.46f,
                .trunkReach = 0.66f,
                .trunkWidth = 0.15f,
                .trunkTaper = 0.58f,
                .lean       = 0.85f,
                .tiers       = 7,
                .reach       = 1.0f,
                .taper       = 0.82f,
                .jitter      = 0.26f,
                .crown       = Crown::Clump,
                .ragged      = 0.15f,
                .gaps        = 0.20f,
                .branchReach = 0.94f,
            },
        .climate =
            {
                // Temperate and wet, and the commonest thing in that country.
                .temperature      = 0.56f,
                .temperatureWidth = 0.28f,
                .humidity         = 0.62f,
                .humidityWidth    = 0.30f,
                .ceiling          = 72.0f,
                .ceilingFade      = 96.0f,
                .abundance        = 1.0f,
            },
        .growth = {.maturityMinutes = 18.0f,
                   .regrowMinutes   = 12.0f,
                   .lightNeed       = 0.55f,
                   .waterNeed       = 0.5f,
                   .toughness       = 5.0f},
        .palette =
            {
                {.barkDark = {58, 38, 22, 255},
                 .bark     = {107, 68, 35, 255},
                 .barkLight = {140, 94, 52, 255},
                 .leaf = {{32, 94, 44, 255}, {58, 138, 58, 255}, {96, 182, 74, 255}, {150, 214, 102, 255}}},
                {.barkDark = {58, 38, 22, 255},
                 .bark     = {107, 68, 35, 255},
                 .barkLight = {140, 94, 52, 255},
                 .leaf = {{26, 84, 40, 255}, {47, 124, 52, 255}, {79, 164, 66, 255}, {126, 198, 90, 255}}},
                {.barkDark = {56, 36, 20, 255},
                 .bark     = {102, 64, 32, 255},
                 .barkLight = {134, 88, 48, 255},
                 .leaf = {{104, 58, 22, 255}, {158, 96, 30, 255}, {202, 140, 44, 255}, {230, 182, 74, 255}}},
                {.barkDark = {50, 34, 22, 255},
                 .bark     = {92, 62, 36, 255},
                 .barkLight = {122, 86, 52, 255},
                 .leaf = {{76, 52, 30, 255}, {104, 74, 42, 255}, {132, 98, 58, 255}, {158, 124, 78, 255}}},
            },
        .drops = {{.item = Item::Wood, .least = 4, .most = 7, .chance = 1.0f},
                  {.item = Item::Sapling, .least = 1, .most = 2, .chance = 0.55f},
                  {.item = Item::Fibre, .least = 1, .most = 3, .chance = 0.4f}},
        .deciduous = true,
    },
    {
        .name        = "pine",
        .layer       = Layer::Canopy,
        .height      = {38.0f, 95.0f, 170.0f, 214.0f},
        .canopyWidth = {18.0f, 40.0f, 65.0f, 78.0f},
        .shape =
            {
                // Branches carried nearly to the ground on a straight mast, and
                // tiers that shrink hard towards the top: the stack of wedges
                // the conifer is.
                .clearance  = 0.32f,
                .trunkReach = 0.84f,
                .trunkWidth = 0.16f,
                .trunkTaper = 0.34f,
                .lean       = 0.12f,
                .tiers       = 10,
                .reach       = 1.28f,
                .taper       = 0.44f,
                .jitter      = 0.12f,
                .crown       = Crown::Frond,

                // Few gaps: a conifer's holes are the spaces *between* its
                // tiers, which the stack leaves on its own, not tears in the
                // middle of one.
                .ragged      = 0.16f,
                .gaps        = 0.08f,
                .branchReach = 0.60f,
            },
        .climate =
            {
                // Cold, and very nearly indifferent to how wet it is. The one
                // species that climbs to the bare ground near the tops, which is
                // what the widest humidity range of the four is for: the climate
                // lifts humidity by 0.0016 per pixel of elevation, so the high
                // ground this tree is supposed to own reads as soaking, and a
                // range narrow enough to look reasonable on paper left the peaks
                // suiting nobody and therefore bare.
                .temperature      = 0.30f,
                .temperatureWidth = 0.28f,
                .humidity         = 0.52f,
                .humidityWidth    = 0.44f,
                .ceiling          = 8.0f,
                .ceilingFade      = 110.0f,
                .abundance        = 1.0f,
            },
        .growth = {.maturityMinutes = 24.0f,
                   .regrowMinutes   = 16.0f,
                   .lightNeed       = 0.4f,
                   .waterNeed       = 0.35f,
                   .toughness       = 6.0f},
        .palette =
            {
                {.barkDark = {48, 30, 18, 255},
                 .bark     = {96, 58, 32, 255},
                 .barkLight = {126, 82, 46, 255},
                 .leaf = {{38, 62, 30, 255}, {62, 92, 42, 255}, {92, 124, 56, 255}, {130, 158, 74, 255}}},
                {.barkDark = {48, 30, 18, 255},
                 .bark     = {96, 58, 32, 255},
                 .barkLight = {126, 82, 46, 255},
                 .leaf = {{32, 54, 26, 255}, {54, 82, 36, 255}, {82, 112, 48, 255}, {118, 146, 66, 255}}},
                {.barkDark = {46, 28, 18, 255},
                 .bark     = {92, 56, 30, 255},
                 .barkLight = {120, 78, 44, 255},
                 .leaf = {{30, 50, 26, 255}, {50, 76, 34, 255}, {76, 104, 46, 255}, {110, 136, 62, 255}}},
                {.barkDark = {42, 28, 20, 255},
                 .bark     = {84, 54, 32, 255},
                 .barkLight = {110, 74, 46, 255},
                 .leaf = {{26, 44, 24, 255}, {44, 66, 32, 255}, {66, 90, 42, 255}, {96, 118, 56, 255}}},
            },
        .drops = {{.item = Item::Wood, .least = 5, .most = 9, .chance = 1.0f},
                  {.item = Item::Resin, .least = 1, .most = 2, .chance = 0.45f},
                  {.item = Item::Sapling, .least = 1, .most = 2, .chance = 0.5f}},
        .deciduous = false,
    },
    {
        .name        = "birch",
        .layer       = Layer::Canopy,
        .height      = {30.0f, 72.0f, 118.0f, 142.0f},
        .canopyWidth = {20.0f, 52.0f, 86.0f, 100.0f},
        .shape =
            {
                // Slender and upright, with a narrow crown high on a pale trunk.
                .clearance  = 0.48f,
                .trunkReach = 0.78f,
                .trunkWidth = 0.11f,
                .trunkTaper = 0.62f,
                .lean       = 0.45f,
                .tiers       = 7,
                .reach       = 1.02f,
                .taper       = 0.78f,
                .jitter      = 0.22f,
                .crown       = Crown::Clump,
                .ragged      = 0.14f,
                .gaps        = 0.18f,
                .branchReach = 0.92f,
            },
        .climate =
            {
                // Cool and wet, overlapping the oak on one side and the pine on
                // the other, so the border between those two woods is mixed
                // rather than a line.
                .temperature      = 0.40f,
                .temperatureWidth = 0.22f,
                .humidity         = 0.66f,
                .humidityWidth    = 0.26f,
                .ceiling          = 44.0f,
                .ceilingFade      = 92.0f,
                .abundance        = 0.75f,
            },
        .growth = {.maturityMinutes = 14.0f,
                   .regrowMinutes   = 9.0f,
                   .lightNeed       = 0.65f,
                   .waterNeed       = 0.6f,
                   .toughness       = 3.5f},
        .palette =
            {
                {.barkDark = {112, 116, 112, 255},
                 .bark     = {196, 200, 196, 255},
                 .barkLight = {232, 236, 232, 255},
                 .leaf = {{56, 120, 56, 255}, {88, 158, 72, 255}, {126, 196, 92, 255}, {172, 224, 124, 255}}},
                {.barkDark = {112, 116, 112, 255},
                 .bark     = {196, 200, 196, 255},
                 .barkLight = {232, 236, 232, 255},
                 .leaf = {{44, 106, 48, 255}, {74, 142, 62, 255}, {110, 180, 80, 255}, {156, 212, 112, 255}}},
                {.barkDark = {110, 112, 108, 255},
                 .bark     = {190, 192, 186, 255},
                 .barkLight = {226, 228, 220, 255},
                 .leaf = {{130, 104, 28, 255}, {184, 148, 40, 255}, {224, 190, 60, 255}, {244, 220, 110, 255}}},
                {.barkDark = {104, 108, 108, 255},
                 .bark     = {182, 188, 190, 255},
                 .barkLight = {218, 224, 226, 255},
                 .leaf = {{96, 92, 72, 255}, {124, 120, 96, 255}, {150, 146, 120, 255}, {178, 174, 148, 255}}},
            },
        .drops = {{.item = Item::Wood, .least = 3, .most = 5, .chance = 1.0f},
                  {.item = Item::Sapling, .least = 1, .most = 2, .chance = 0.6f},
                  {.item = Item::Fibre, .least = 1, .most = 2, .chance = 0.35f}},
        .deciduous = true,
    },
    {
        .name        = "apple",
        .layer       = Layer::Canopy,
        .height      = {30.0f, 68.0f, 106.0f, 120.0f},
        .canopyWidth = {22.0f, 54.0f, 84.0f, 94.0f},
        .shape =
            {
                // Short, broad and low-crowned: an orchard tree, and the one a
                // player can reach the fruit of without climbing anything.
                .clearance  = 0.48f,
                .trunkReach = 0.68f,
                .trunkWidth = 0.16f,
                .trunkTaper = 0.62f,
                .lean       = 1.0f,
                .tiers       = 6,
                .reach       = 1.0f,
                .taper       = 0.86f,
                .jitter      = 0.30f,
                .crown       = Crown::Clump,
                .ragged      = 0.15f,
                .gaps        = 0.18f,
                .branchReach = 0.88f,
            },
        .climate =
            {
                // The narrowest range of the four, so it stays something come
                // across rather than something walked through.
                .temperature      = 0.60f,
                .temperatureWidth = 0.18f,
                .humidity         = 0.58f,
                .humidityWidth    = 0.22f,
                .ceiling          = 96.0f,
                .ceilingFade      = 80.0f,
                .abundance        = 0.35f,
            },
        .growth = {.maturityMinutes = 12.0f,
                   .regrowMinutes   = 10.0f,
                   .lightNeed       = 0.7f,
                   .waterNeed       = 0.65f,
                   .toughness       = 3.0f},
        .palette =
            {
                // Spring is the one palette that is not foliage: the top two
                // tones are blossom, which is what the tree is for half a season
                // before it is worth picking.
                {.barkDark = {62, 42, 26, 255},
                 .bark     = {112, 76, 42, 255},
                 .barkLight = {146, 104, 60, 255},
                 .leaf = {{58, 116, 52, 255}, {96, 156, 70, 255}, {214, 178, 196, 255}, {244, 222, 232, 255}}},
                {.barkDark = {62, 42, 26, 255},
                 .bark     = {112, 76, 42, 255},
                 .barkLight = {146, 104, 60, 255},
                 .leaf = {{34, 92, 42, 255}, {60, 130, 54, 255}, {92, 168, 68, 255}, {134, 200, 96, 255}}},
                {.barkDark = {60, 40, 24, 255},
                 .bark     = {108, 72, 40, 255},
                 .barkLight = {140, 100, 56, 255},
                 .leaf = {{112, 74, 26, 255}, {166, 112, 34, 255}, {208, 152, 48, 255}, {234, 192, 84, 255}}},
                {.barkDark = {54, 38, 24, 255},
                 .bark     = {98, 70, 42, 255},
                 .barkLight = {128, 96, 58, 255},
                 .leaf = {{80, 58, 34, 255}, {108, 80, 48, 255}, {134, 104, 66, 255}, {160, 130, 90, 255}}},
            },
        .drops = {{.item = Item::Wood, .least = 2, .most = 4, .chance = 1.0f},
                  {.item = Item::Apple, .least = 1, .most = 3, .chance = 0.7f},
                  {.item = Item::Sapling, .least = 1, .most = 1, .chance = 0.5f}},
        .deciduous = true,
    },
};

static_assert(std::size(kSpecies) == kSpeciesCount, "every Species needs exactly one row in kSpecies");

inline constexpr const SpeciesDef &Def(Species species) { return kSpecies[SpeciesIndex(species)]; }

// How a layer is scattered.
struct LayerSettings {
    // Cell along the horizontal axis, in world pixels. One plant is grown per
    // cell, so this is what sets how thick a full wood is, and it has to be at
    // least the widest canopy in the layer or the bound below stops holding.
    float cellSpan = 110.0f;

    // Share of a canopy width by which two neighbours may reach into each other,
    // in [0,1]. At zero a canopy cannot leave its own cell and no two of them
    // can touch; at one they interlock by a full width. The single knob between
    // an orchard and a thicket.
    float interlock = 0.55f;

    // Share of the eligible ground the layer claims where its field is highest.
    // Measured, not declared: see Calibrate.
    float coverage = 0.5f;

    // Slope at which the ground stops being worth standing on, as a rise over
    // run, and the run it is measured over. The run is wide on purpose — the
    // surface is terraced into risers a quarter of a jump high, and a baseline
    // narrow enough to sit on one riser would read every hillside as a cliff.
    float slopeLimit = 0.62f;
    float slopeSpan  = 36.0f;

    // Drop across the trunk's own footing, in pixels, past which there is
    // nothing to stand on. This is the test that keeps a tree off the mouth of a
    // shaft: a column open into a cave is not a steep slope, it is a hole, and
    // the slope test above averages straight over it.
    float dropLimit = 44.0f;
};

// How much of the world is wooded, and how the woods are arranged.
struct Settings {
    // Where a wood is and where a clearing is. One feature spans a couple of
    // screens, which is the scale at which a stretch of forest reads as
    // somewhere arrived at rather than as texture on the ground.
    terrain::NoiseShape forest{.frequency = 0.5f, .octaves = 2, .seed = 9101};

    // Which species a stretch of country favours — one field per species, taken
    // from this shape at a stride apart, so each one has its own slow swell.
    //
    // The single field that separates a forest from a scatter. Without it every
    // tree rolls on its own and a wood comes out an even mixture of whatever the
    // climate allows, which is the one thing a real wood never is.
    terrain::NoiseShape stand{.frequency = 0.32f, .octaves = 1, .seed = 9102};

    // Distance between the stands of two neighbouring species, in world pixels.
    // Far enough apart that no two of them are the same field read twice.
    float standStride = 4800.0f;

    // How sharply the favoured species wins. One leaves the stand field as a
    // gentle bias; well above one makes a stand nearly pure, with the others met
    // as the occasional tree among it.
    //
    // Six, measured rather than guessed: at three, a tree's neighbour was the
    // same species 46% of the time against the 32% an even mixture of the same
    // four would give — a bias, but not a wood. This is what carries it to the
    // two thirds that reads as walking through a pinewood and coming out into
    // oaks.
    float standSharpness = 6.0f;

    // How much of a wood's thickness is left where the climate suits nobody.
    //
    // The suitability of the best-placed species is what says how much life a
    // spot supports, and it reaches its own ceiling only exactly at that
    // species' centre — so using it outright thinned every wood in the world to
    // a third and left the map an even scatter. A floor makes the ground between
    // two species' ranges a thinner wood rather than a bare one, which is what a
    // transition between two forests actually looks like.
    float supportFloor = 0.45f;

    // Half-width of the ramp across the edge of a wood, in units of the forest
    // field. A hard cutoff would put a fence around every wood.
    //
    // Much narrower than it looks like it should be, because the field it is
    // measured in is narrow: folded Perlin crowds hard around the middle, and
    // moving the coverage from 0.46 to 0.58 moved the measured cutoff by 0.02.
    // At 0.06 the ramp was three times the whole span the coverage control has,
    // so nowhere was fully wooded and nowhere was fully clear, and raising the
    // coverage by twelve points bought four. This is a border a wood thins out
    // over rather than one it fades across the county in.
    float edgeBand = 0.015f;

    LayerSettings layer[kLayerCount] = {
        // Coverage is the share of the world the forest field claims, which is
        // not the share that ends up with a tree on it: the ramp across a wood's
        // border and the support term both thin it, and what came out of walking
        // two hundred thousand pixels was a little over two thirds of it. Set
        // from the figure wanted on the ground rather than from the one the field
        // is measured against.
        {.cellSpan = 110.0f, .interlock = 0.55f, .coverage = 0.58f},
        {.cellSpan = 26.0f, .interlock = 0.8f, .coverage = 0.5f, .slopeLimit = 0.85f, .slopeSpan = 18.0f,
         .dropLimit = 26.0f},
    };

    // Added to every field's own seed, the way terrain::Settings::seed is, so one
    // number moves every wood in the world and the fields stay decorrelated.
    int seed = 0;

    // Cutoffs measured from the fields themselves so the coverage figures above
    // mean what they say. Filled by Calibrate; never written by hand.
    struct Calibration {
        float forest[kLayerCount] = {1.0f, 1.0f};
    };

    Calibration calibration{};
};

// Measures the cutoffs the coverage figures ask for. Call once after authoring
// the settings and before scattering anything; the results are stored in
// `settings` so that scattering stays a pure function of it.
void Calibrate(Settings &settings);

// The surface the plants stand on, as the scatter wants to be handed it.
//
// A view rather than a copy, and rather than a way back into the world: the
// scatter asks about a handful of columns, the world already remembers them, and
// what this carries is the answers, filled by the caller before it asks. The
// same arrangement weather::Ground has, for the same reason.
//
// It must be the *skyline* — the surface the noise describes — and not the
// surface as built. Placement that moved with what has been dug would let a tree
// blink out of existence because somebody put a hole near it.
struct Ground {
    const float *top = nullptr;
    int count        = 0;
    float originX    = 0.0f;
    float spacing    = 1.0f;
};

// Surface at a world position, from the nearest column prepared.
float GroundAt(const Ground &ground, float worldX);

// One plant the world grows, before anything that has happened to it.
struct Plant {
    // The cell it grew in, which is the whole of its identity: the same number
    // on every frame and in every session, and therefore what a record of damage
    // is filed under.
    std::int64_t id = 0;

    Species species = Species::Oak;

    // Where the trunk meets the ground.
    Vector2 base{};

    // Size at maturity, against the sizes in the table. Carries both the plant's
    // own variation and how well the place suits it, so a species at the edge of
    // its range stands there stunted rather than absent.
    float scale = 1.0f;

    // Drawn mirrored. Free variety on top of the shape, and safe to do because
    // the form shading is symmetric — a baked sun would come out on the wrong
    // side of half the wood.
    bool mirrored = false;
};

// The cell a world position falls in, for a layer.
std::int64_t CellAt(Layer layer, const Settings &settings, float worldX);

// The plant one cell grows, or nothing where the cell is bare.
//
// The whole of the placement. Scatter is this in a loop; it is exposed on its
// own because a cell can be asked about without asking about its neighbours,
// which is what the undergrowth pass needs in order to keep out of a trunk and
// what a record of damage needs in order to find the tree it belongs to.
bool Grow(Layer layer, std::int64_t cell, const Settings &settings, const terrain::Settings &terrain,
          const Ground &ground, Plant &out);

// Every plant of a layer whose cell overlaps the span. Cleared first.
void Scatter(Layer layer, float fromX, float toX, const Settings &settings, const terrain::Settings &terrain,
             const Ground &ground, std::vector<Plant> &out);

// How wide a margin either side of a span must be prepared before scattering it,
// so that a plant rooted off the edge still has its cell considered.
float Margin(Layer layer, const Settings &settings);

// One mass of foliage on a crown.
struct Lobe {
    Vector2 at   = {};
    float radius = 0.0f;

    // Squashed vertically above one. A frond is a tier lying across the trunk
    // rather than a ball stuck to it, and this is the whole difference.
    float flatten = 1.0f;

    // Where down the crown it sits, in [0,1] from the top. What a shader would
    // call an ambient occlusion term: the tones darken with it, so a crown is
    // lit from above without anything having to know where the sun is.
    float depth = 0.0f;

    // Which side of the trunk it hangs off, and its own number, so the noise
    // that ravels its edge is different from its neighbour's.
    int salt = 0;
};

// A limb, from where it leaves the trunk to where it disappears into a mass of
// foliage.
//
// Drawn before the leaves and mostly covered by them. What it is for is the part
// that is not covered: every reference tree of this kind shows its branches
// through the gaps in its crown, and a canopy with no structure behind the holes
// reads as a cloud sitting on a stick.
struct Branch {
    Vector2 from = {};
    Vector2 to   = {};

    // Where the limb would end if it were drawn the whole way to the middle of
    // the mass it carries. Bare of leaves, that is where it is drawn to, because
    // in winter the limbs are the tree.
    Vector2 tip = {};

    float width = 0.0f;
};

inline constexpr int kTrunkNodes  = 6;
inline constexpr int kMaxLobes    = 32;
inline constexpr int kMaxBranches = kMaxLobes;

// A plant's shape, in its own space: the origin is where the trunk meets the
// ground and Y grows upward, so a node's Y is its height off the floor.
//
// Built rather than stored. It is a pure function of the species, the stage, the
// variant and the size, so the baker builds one to rasterise and the silhouette
// draw builds the same one to outline, and the two cannot disagree.
struct Skeleton {
    Vector2 trunk[kTrunkNodes]{};
    float trunkWidth[kTrunkNodes]{};

    Lobe lobes[kMaxLobes]{};
    int lobeCount = 0;

    Branch branches[kMaxBranches]{};
    int branchCount = 0;

    float height = 0.0f;
    float width  = 0.0f;

    // The box the whole plant occupies in its own frame, foliage included, so a
    // rasteriser can size a canvas without walking the masses to find out.
    float left  = 0.0f;
    float right = 0.0f;
};

// `seed` is the plant's own — its cell index — so every tree in the world is a
// different tree and the same tree every time it is asked for.
Skeleton Build(Species species, Stage stage, std::int64_t seed, float scale);

} // namespace flora
