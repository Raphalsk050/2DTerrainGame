#pragma once

#include "element.h"
#include "item.h"
#include "raylib.h"
#include "terrain.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
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

enum class Species { Oak, Pine, Birch, Apple, Fern, Scrub, Count };

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

// What each is called, in the enum's own order.
//
// Here rather than beside whichever screen prints them: the console matches a typed
// word against this, the head-up display shows which one F9 is holding, and a probe
// labels its columns with it. Three copies of four words is three chances for one
// of them to be wrong.
inline constexpr const char *kSeasonNames[kSeasonCount] = {"spring", "summer", "autumn", "winter"};

inline constexpr std::size_t LayerIndex(Layer layer) {
    return static_cast<std::size_t>(layer);
}
inline constexpr std::size_t SpeciesIndex(Species species) {
    return static_cast<std::size_t>(species);
}
inline constexpr std::size_t StageIndex(Stage stage) {
    return static_cast<std::size_t>(stage);
}
inline constexpr std::size_t SeasonIndex(Season season) {
    return static_cast<std::size_t>(season);
}

// What a plant is called, for every table that files something under one.
//
// The layer *and* the cell, and the layer is not decoration. The two passes count
// their cells on lattices four times apart — 110 px against 26 — so the number 5
// names a place in each of them, and an id that was the cell alone made a fern and
// an oak the same plant. That was not theoretical: canopy::Sheet keys its baked
// sprites on the id and the undergrowth is drawn first, so an oak standing in
// canopy cell 5 was handed the fern's picture; and Grove::remembered_ keys damage
// on it, so pulling up a fern would have cleared a tree three hundred pixels away.
//
// Interleaved rather than offset by a base, because a cell index is signed and a
// base only separates the halves of the number line it does not straddle. This is
// injective for every cell either side of zero, at the cost of one bit of range —
// which leaves Grove's kPlantedBase as unreachable as it was.
inline constexpr std::int64_t PlantId(Layer layer, std::int64_t cell) {
    return cell * static_cast<std::int64_t>(kLayerCount) + static_cast<std::int64_t>(LayerIndex(layer));
}

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
    int tiers   = 6;
    float reach = 1.0f;

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

    // How wide one mass of foliage is, as a share of the reach of the tier it
    // sits in.
    //
    // Per species because it trades against `tiers` and the two have to be set
    // together. A mass has to be at least as tall as the gap to the tier above it
    // or the crown opens into stripes; but it also has to be tall enough to carry
    // a light crest, a middle and a shaded belly, which is about nine texels. Few
    // tiers of large masses gives that. Many tiers of small ones — which is what
    // a conifer needs for its notched outline — gives a mass the size of the leaf
    // texture itself, and the two collide.
    //
    // The masses are set apart by whatever is left of the tier, so the canopy
    // width in the table stays the width the crown comes out.
    float mass = 0.42f;

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
    // and rain, for a tree of average vigour. Measured in the same clock the day
    // and the weather run on, so it runs fast under F7 with everything else.
    //
    // Anchored on Minecraft, which is the reference this world's block, reach,
    // stack and throw delay all came from. A sapling there has a one in seven
    // chance of advancing per random tick and needs two advances, which comes out
    // at about **sixteen minutes on average** and anywhere from five to thirty in
    // practice; every species uses the same rate, the differences between them
    // being about space rather than time. Sixteen minutes is a little under a
    // Minecraft day and a little over half of this one, and that was the fault
    // being fixed: every tree here matured in four to eight minutes, so a whole
    // wood could be planted and felled inside one afternoon.
    //
    // The spread is not in this number. Every tree rolls its own vigour against
    // it — see kVigourLeast — so the figure here is the middle of a species and
    // never the answer for any particular tree, which is what Minecraft's very
    // long tail buys and what a fixed time cannot: two saplings put in together
    // must not come up together.
    float maturityMinutes = 16.0f;

    // How much of the growth rate hangs on light and on water, in [0,1]. At zero
    // the species grows at its own pace wherever it is; at one it stops entirely
    // in the dark or in the dry.
    float lightNeed = 0.5f;
    float waterNeed = 0.5f;

    // How much tree there is to cut through, in Minecraft logs. Scaled by the
    // stage and by the specimen's own size, so a sapling goes in one and an
    // unusually large one takes longer -- see kStatureMost.
    //
    // It was "hits", which is the same number and the wrong unit: a hit is a
    // property of the swing and not of the tree, so the figure could not be
    // compared with anything or set from anything. Read as logs it can: an oak at
    // five is five logs, and Minecraft charges kLogSeconds for each of them.
    float toughness = 4.0f;
};

// The bounds Grove::Stature clamps a specimen's own scale to.
//
// Published rather than left where they are used, because the felling ceiling below
// is worked out against kStatureMost: the longest any tree can take is the toughest
// species at the largest a specimen of it grows, and a bound that did not know about
// the scale would be a bound on the average and not on the worst case -- which is the
// one a player meets and remembers.
inline constexpr float kStatureLeast = 0.12f;
inline constexpr float kStatureMost  = 1.5f;

// What a species will root in, as the covers it accepts by name.
//
// Named grounds rather than another climate bell, and the difference is the whole
// of why trees were standing in the desert. A bell is a *tendency*: an oak thins
// out as the country dries, and thinning out is a share, and a share of a wood
// still leaves trees. That is right for how thick a wood is and wrong for whether
// there is one — a trunk growing out of open sand is not a tree at the edge of its
// range, it is a mistake, and no tuning of a bell can make it never happen while
// leaving the rest of the range alone.
//
// It is also the one restriction Minecraft puts on a sapling, and it is why the
// desert there is bare without anything in that world knowing what a desert is.
//
// Bare rock accepts nothing: every field here is about a cover, and a column with
// no cover on it is stone.
struct SpeciesGround {
    bool soil = true;
    bool sand = false;
    bool snow = false;
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
    Item item    = Item::Wood;
    int least    = 0;
    int most     = 0;
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
    SpeciesGround ground;

    SpeciesPalette palette[kSeasonCount];
    DropRule drops[kDropRules];

    // The seed this tree drops and the seed that grows back into it.
    //
    // Held here rather than as a species field on the item, and the direction is
    // forced: the item table knows nothing about trees — an item is a name, a
    // picture and a count — while this table is already about trees and already
    // names the items one drops. So flora depends on item and never the other way
    // round, and the pairing lives on the side that can express it.
    //
    // Nothing for a species that does not sow — the undergrowth, which grows on its
    // own and is never planted.
    //
    // An optional and not a marker item. It was left at the first row of the item
    // table on the reasoning that the first row is not a sapling, so nothing could
    // mistake it for one — which is true and hid a typo perfectly: a species naming
    // *any* item that is not plantable came out as a species that simply does not
    // sow, which is a legitimate answer, so nothing anywhere could tell the two
    // apart. Now there is a way to say "none", and saying anything else and getting
    // it wrong is a compile error.
    std::optional<Item> sapling{};

    // Stands bare in winter. A conifer does not, which is the one thing that
    // distinguishes the two through the cold half of the year.
    bool deciduous;
};

// The plants, each one row.
//
// Sizes are in world pixels and can be read against the character, which is 26
// tall and 12 wide: a mature oak is five of it, a mature pine is six and a half.
inline constexpr SpeciesDef
    kSpecies[] =
        {
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
                        .clearance   = 0.40f,
                        .trunkReach  = 0.64f,
                        .trunkWidth  = 0.15f,
                        .trunkTaper  = 0.58f,
                        .lean        = 0.85f,
                        .tiers       = 5,
                        .reach       = 1.04f,
                        .taper       = 0.82f,
                        .jitter      = 0.15f,
                        .crown       = Crown::Clump,
                        .mass        = 0.56f,
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
                // A little over Minecraft's average, because an oak is the big
                // slow broadleaf of this table and the birch beside it is what a
                // player in a hurry plants.
                .growth = {.maturityMinutes = 22.0f,
                           .lightNeed       = 0.55f,
                           .waterNeed       = 0.5f,
                           .toughness       = 5.0f},

                // Earth, and only earth. The oak is the temperate tree and this
                // is what keeps it out of the sand and off the snowfields.
                .ground = {.soil = true},
                .palette =
                    {
                        {.barkDark  = {58, 38, 22, 255},
                         .bark      = {107, 68, 35, 255},
                         .barkLight = {140, 94, 52, 255},
                         .leaf = {{32, 94, 44, 255}, {58, 138, 58, 255}, {96, 182, 74, 255}, {150, 214, 102, 255}}},
                        {.barkDark  = {58, 38, 22, 255},
                         .bark      = {107, 68, 35, 255},
                         .barkLight = {140, 94, 52, 255},
                         .leaf      = {{26, 84, 40, 255}, {47, 124, 52, 255}, {79, 164, 66, 255}, {126, 198, 90, 255}}},
                        {.barkDark  = {56, 36, 20, 255},
                         .bark      = {102, 64, 32, 255},
                         .barkLight = {134, 88, 48, 255},
                         .leaf = {{104, 58, 22, 255}, {158, 96, 30, 255}, {202, 140, 44, 255}, {230, 182, 74, 255}}},
                        {.barkDark  = {50, 34, 22, 255},
                         .bark      = {92, 62, 36, 255},
                         .barkLight = {122, 86, 52, 255},
                         .leaf      = {{76, 52, 30, 255}, {104, 74, 42, 255}, {132, 98, 58, 255}, {158, 124, 78, 255}}},
                    },
                .drops     = {{.item = Item::Wood, .least = 4, .most = 7, .chance = 1.0f},
                              {.item = Item::OakSapling, .least = 1, .most = 2, .chance = 0.55f},
                              {.item = Item::Fibre, .least = 1, .most = 3, .chance = 0.4f}},
                .sapling   = Item::OakSapling,
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
                        .tiers      = 10,
                        .reach      = 1.05f,
                        .taper      = 0.44f,
                        .jitter     = 0.12f,
                        .crown      = Crown::Frond,
                        .mass       = 0.55f,

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

                        // The treeline, and it is now a real one: the pine is the
                        // only species in the table that climbs a mountain, and
                        // this is where it gives up. Full wood below y = -60,
                        // nothing above y = -220 — which against a snow line
                        // starting at -170 means the top of a pinewood is on the
                        // snow and the bare rock above it is the summit.
                        //
                        // It was 8, from before there were mountains, and that put
                        // the treeline a hundred and thirty pixels above the plains
                        // — halfway up an ordinary hill. Every range would have been
                        // bare from its foothills.
                        .ceiling          = -220.0f,
                        .ceilingFade      = 160.0f,
                        .abundance        = 1.0f,
                    },
                // The slowest in the table, and the tallest. Half again as long as
                // an oak takes, which at the vigour spread means an unlucky pine
                // is more than two days of this world's own clock.
                .growth = {.maturityMinutes = 30.0f,
                           .lightNeed       = 0.4f,
                           .waterNeed       = 0.35f,
                           .toughness       = 6.0f},

                // And snow as well as earth, which is not a licence but a
                // requirement: snow is a cap eleven pixels deep laid over the soil
                // and it outranks it, so the surface of every cold column in the
                // world reads as snow. A pine that would not root in it is a pine
                // that cannot grow in the country it is centred on, and the taiga
                // comes out bare.
                .ground = {.soil = true, .snow = true},
                .palette =
                    {
                        {.barkDark  = {48, 30, 18, 255},
                         .bark      = {96, 58, 32, 255},
                         .barkLight = {126, 82, 46, 255},
                         .leaf      = {{38, 62, 30, 255}, {62, 92, 42, 255}, {92, 124, 56, 255}, {130, 158, 74, 255}}},
                        {.barkDark  = {48, 30, 18, 255},
                         .bark      = {96, 58, 32, 255},
                         .barkLight = {126, 82, 46, 255},
                         .leaf      = {{32, 54, 26, 255}, {54, 82, 36, 255}, {82, 112, 48, 255}, {118, 146, 66, 255}}},
                        {.barkDark  = {46, 28, 18, 255},
                         .bark      = {92, 56, 30, 255},
                         .barkLight = {120, 78, 44, 255},
                         .leaf      = {{30, 50, 26, 255}, {50, 76, 34, 255}, {76, 104, 46, 255}, {110, 136, 62, 255}}},
                        {.barkDark  = {42, 28, 20, 255},
                         .bark      = {84, 54, 32, 255},
                         .barkLight = {110, 74, 46, 255},
                         .leaf      = {{26, 44, 24, 255}, {44, 66, 32, 255}, {66, 90, 42, 255}, {96, 118, 56, 255}}},
                    },
                .drops     = {{.item = Item::Wood, .least = 5, .most = 9, .chance = 1.0f},
                              {.item = Item::Resin, .least = 1, .most = 2, .chance = 0.45f},
                              {.item = Item::PineSapling, .least = 1, .most = 2, .chance = 0.5f}},
                .sapling   = Item::PineSapling,
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
                        .clearance   = 0.38f,
                        .trunkReach  = 0.72f,
                        .trunkWidth  = 0.11f,
                        .trunkTaper  = 0.62f,
                        .lean        = 0.45f,
                        .tiers       = 5,
                        .reach       = 1.07f,
                        .taper       = 0.78f,
                        .jitter      = 0.13f,
                        .crown       = Crown::Clump,
                        .mass        = 0.56f,
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
                // Minecraft's own average exactly. The birch is the pioneer of the
                // table — thin, quick, first onto open ground — so it is the one
                // species that keeps the reference figure unaltered.
                .growth = {.maturityMinutes = 16.0f,
                           .lightNeed       = 0.65f,
                           .waterNeed       = 0.6f,
                           .toughness       = 3.5f},

                // Earth and snow, for the pine's reason: the birch overlaps it on
                // the cold side and shares the same ground there.
                .ground = {.soil = true, .snow = true},
                .palette =
                    {
                        {.barkDark  = {112, 116, 112, 255},
                         .bark      = {196, 200, 196, 255},
                         .barkLight = {232, 236, 232, 255},
                         .leaf = {{56, 120, 56, 255}, {88, 158, 72, 255}, {126, 196, 92, 255}, {172, 224, 124, 255}}},
                        {.barkDark  = {112, 116, 112, 255},
                         .bark      = {196, 200, 196, 255},
                         .barkLight = {232, 236, 232, 255},
                         .leaf = {{44, 106, 48, 255}, {74, 142, 62, 255}, {110, 180, 80, 255}, {156, 212, 112, 255}}},
                        {.barkDark  = {110, 112, 108, 255},
                         .bark      = {190, 192, 186, 255},
                         .barkLight = {226, 228, 220, 255},
                         .leaf = {{130, 104, 28, 255}, {184, 148, 40, 255}, {224, 190, 60, 255}, {244, 220, 110, 255}}},
                        {.barkDark  = {104, 108, 108, 255},
                         .bark      = {182, 188, 190, 255},
                         .barkLight = {218, 224, 226, 255},
                         .leaf = {{96, 92, 72, 255}, {124, 120, 96, 255}, {150, 146, 120, 255}, {178, 174, 148, 255}}},
                    },
                .drops     = {{.item = Item::Wood, .least = 3, .most = 5, .chance = 1.0f},
                              {.item = Item::BirchSapling, .least = 1, .most = 2, .chance = 0.6f},
                              {.item = Item::Fibre, .least = 1, .most = 2, .chance = 0.35f}},
                .sapling   = Item::BirchSapling,
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
                        .clearance   = 0.42f,
                        .trunkReach  = 0.64f,
                        .trunkWidth  = 0.16f,
                        .trunkTaper  = 0.62f,
                        .lean        = 1.0f,
                        .tiers       = 4,
                        .reach       = 1.14f,
                        .taper       = 0.86f,
                        .jitter      = 0.16f,
                        .crown       = Crown::Clump,
                        .mass        = 0.58f,
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
                // Slower than the oak it is shaped like. It is the tree that pays
                // a crop, and a fruit tree that comes into bearing faster than the
                // timber tree beside it would make the orchard the obvious plant
                // in every situation.
                .growth = {.maturityMinutes = 26.0f,
                           .lightNeed       = 0.7f,
                           .waterNeed       = 0.65f,
                           .toughness       = 3.0f},

                .ground = {.soil = true},
                .palette =
                    {
                        // Spring is the one palette that is not foliage: the top two
                        // tones are blossom, which is what the tree is for half a season
                        // before it is worth picking.
                        {.barkDark  = {62, 42, 26, 255},
                         .bark      = {112, 76, 42, 255},
                         .barkLight = {146, 104, 60, 255},
                         .leaf = {{58, 116, 52, 255}, {96, 156, 70, 255}, {214, 178, 196, 255}, {244, 222, 232, 255}}},
                        {.barkDark  = {62, 42, 26, 255},
                         .bark      = {112, 76, 42, 255},
                         .barkLight = {146, 104, 60, 255},
                         .leaf      = {{34, 92, 42, 255}, {60, 130, 54, 255}, {92, 168, 68, 255}, {134, 200, 96, 255}}},
                        {.barkDark  = {60, 40, 24, 255},
                         .bark      = {108, 72, 40, 255},
                         .barkLight = {140, 100, 56, 255},
                         .leaf = {{112, 74, 26, 255}, {166, 112, 34, 255}, {208, 152, 48, 255}, {234, 192, 84, 255}}},
                        {.barkDark  = {54, 38, 24, 255},
                         .bark      = {98, 70, 42, 255},
                         .barkLight = {128, 96, 58, 255},
                         .leaf = {{80, 58, 34, 255}, {108, 80, 48, 255}, {134, 104, 66, 255}, {160, 130, 90, 255}}},
                    },
                .drops     = {{.item = Item::Wood, .least = 2, .most = 4, .chance = 1.0f},
                              {.item = Item::Apple, .least = 1, .most = 3, .chance = 0.7f},
                              {.item = Item::AppleSapling, .least = 1, .most = 1, .chance = 0.5f}},
                .sapling   = Item::AppleSapling,
                .deciduous = true,
            },
            {
                .name        = "fern",
                .layer       = Layer::Undergrowth,
                .height      = {8.0f, 14.0f, 22.0f, 26.0f},
                .canopyWidth = {12.0f, 20.0f, 30.0f, 34.0f},
                .shape =
                    {
                        // No trunk to speak of and fronds straight off the ground: an
                        // undergrowth plant is a crown with a stem, not a small tree.
                        .clearance   = 0.06f,
                        .trunkReach  = 0.30f,
                        .trunkWidth  = 0.08f,
                        .trunkTaper  = 0.5f,
                        .lean        = 0.4f,
                        .tiers       = 3,
                        .reach       = 1.05f,
                        .taper       = 0.55f,
                        .jitter      = 0.20f,
                        .crown       = Crown::Frond,
                        .mass        = 0.58f,
                        .ragged      = 0.24f,
                        .gaps        = 0.10f,
                        .branchReach = 0.5f,
                    },
                .climate =
                    {
                        // Shade and damp. It is the plant found *under* the wood rather
                        // than the one found where a wood is not.
                        .temperature      = 0.48f,
                        .temperatureWidth = 0.30f,
                        .humidity         = 0.70f,
                        .humidityWidth    = 0.30f,
                        .ceiling          = 60.0f,
                        .ceilingFade      = 90.0f,
                        .abundance        = 1.0f,
                    },
                // The one plant that stays quick. Undergrowth is not timber and
                // nobody waits for it; what a fern is for is the floor of a wood
                // filling back in behind a player who walked through it.
                .growth = {.maturityMinutes = 6.0f,
                           .lightNeed       = 0.2f,
                           .waterNeed       = 0.8f,
                           .toughness       = 1.0f},

                .ground = {.soil = true},
                .palette =
                    {
                        {.barkDark  = {58, 52, 34, 255},
                         .bark      = {86, 78, 50, 255},
                         .barkLight = {112, 102, 66, 255},
                         .leaf      = {{28, 74, 38, 255}, {46, 106, 48, 255}, {70, 140, 60, 255}, {104, 176, 80, 255}}},
                        {.barkDark  = {58, 52, 34, 255},
                         .bark      = {86, 78, 50, 255},
                         .barkLight = {112, 102, 66, 255},
                         .leaf      = {{24, 66, 34, 255}, {40, 96, 44, 255}, {62, 128, 54, 255}, {94, 162, 74, 255}}},
                        {.barkDark  = {56, 48, 32, 255},
                         .bark      = {84, 72, 46, 255},
                         .barkLight = {108, 96, 62, 255},
                         .leaf = {{86, 72, 30, 255}, {126, 104, 38, 255}, {166, 140, 52, 255}, {200, 176, 82, 255}}},
                        {.barkDark  = {52, 46, 34, 255},
                         .bark      = {78, 70, 50, 255},
                         .barkLight = {100, 92, 66, 255},
                         .leaf      = {{58, 60, 44, 255}, {80, 84, 60, 255}, {102, 106, 78, 255}, {126, 130, 98, 255}}},
                    },
                .drops     = {{.item = Item::Fibre, .least = 1, .most = 2, .chance = 0.8f}},
                .deciduous = false,
            },
            {
                // What grows where nothing else will.
                //
                // The desert used to be wooded, and the reason was not the climate
                // table — it was flora::Settings::supportFloor, which keeps a share
                // of a wood's thickness wherever the climate suits nobody so that
                // the ground *between* two species' ranges is a thin wood rather
                // than a bare one. That is right almost everywhere and exactly
                // wrong in a desert, which is not between two ranges: it is past
                // the end of all of them. The ground rule is what settles it, and
                // once it does the desert is genuinely empty — so it needs
                // something of its own, or a whole biome is a place with nothing
                // in it.
                //
                // Undergrowth rather than canopy, and that is the honest shape of
                // it: this is a knee-high bush of dead-looking twigs, not a small
                // tree. Nothing about the desert should read as shade.
                .name        = "scrub",
                .layer       = Layer::Undergrowth,
                .height      = {6.0f, 12.0f, 19.0f, 22.0f},
                .canopyWidth = {14.0f, 24.0f, 34.0f, 38.0f},
                .shape =
                    {
                        // Wide, low and open: a tangle sitting straight on the
                        // sand, with more gaps in it than mass. The high jitter and
                        // the ragged edge are what keep it from reading as a green
                        // fern that happens to be brown.
                        .clearance   = 0.10f,
                        .trunkReach  = 0.42f,
                        .trunkWidth  = 0.07f,
                        .trunkTaper  = 0.45f,
                        .lean        = 0.9f,
                        .tiers       = 3,
                        .reach       = 1.15f,
                        .taper       = 0.70f,
                        .jitter      = 0.34f,
                        .crown       = Crown::Clump,
                        .mass        = 0.44f,
                        .ragged      = 0.42f,
                        .gaps        = 0.52f,
                        .branchReach = 1.0f,
                    },
                .climate =
                    {
                        // Sand's own bell, near enough: hot and dry, centred where
                        // the desert is rather than at the edge of it. It does not
                        // have to be exact, because the ground rule below is what
                        // actually decides where this grows — the climate only says
                        // how thick it is inside that.
                        .temperature      = 0.84f,
                        .temperatureWidth = 0.26f,
                        .humidity         = 0.18f,
                        .humidityWidth    = 0.28f,
                        .ceiling          = 40.0f,
                        .ceilingFade      = 120.0f,
                        .abundance        = 1.0f,
                    },
                .growth = {.maturityMinutes = 9.0f,
                           .lightNeed       = 0.05f,
                           .waterNeed       = 0.05f,
                           .toughness       = 1.0f},

                // Sand alone. It is the only thing in the table that will take it,
                // and a scrub bush standing in a meadow would undo the whole point
                // of the rule.
                .ground = {.soil = false, .sand = true},
                .palette =
                    {
                        // Barely a season to it. A desert plant is the same dead
                        // straw in April as in October, which is most of what says
                        // desert — the year turning is a thing that happens to
                        // country with water in it.
                        {.barkDark  = {84, 66, 40, 255},
                         .bark      = {116, 94, 58, 255},
                         .barkLight = {146, 122, 80, 255},
                         .leaf = {{104, 92, 48, 255}, {140, 124, 66, 255}, {176, 158, 92, 255}, {208, 192, 128, 255}}},
                        {.barkDark  = {84, 64, 38, 255},
                         .bark      = {118, 92, 56, 255},
                         .barkLight = {150, 122, 78, 255},
                         .leaf = {{110, 94, 46, 255}, {148, 128, 64, 255}, {184, 162, 88, 255}, {216, 198, 124, 255}}},
                        {.barkDark  = {80, 60, 36, 255},
                         .bark      = {112, 88, 52, 255},
                         .barkLight = {144, 118, 74, 255},
                         .leaf = {{102, 84, 42, 255}, {138, 116, 58, 255}, {174, 150, 82, 255}, {206, 186, 118, 255}}},
                        {.barkDark  = {76, 60, 40, 255},
                         .bark      = {106, 86, 58, 255},
                         .barkLight = {136, 114, 80, 255},
                         .leaf = {{96, 86, 56, 255}, {128, 116, 78, 255}, {160, 146, 104, 255}, {192, 178, 136, 255}}},
                    },
                .drops     = {{.item = Item::Fibre, .least = 1, .most = 3, .chance = 0.9f}},
                .deciduous = false,
            },
};

static_assert(std::size(kSpecies) == kSpeciesCount, "every Species needs exactly one row in kSpecies");

// The most tree there is to cut through anywhere in the table, in logs.
//
// The toughest species at the largest a specimen of it grows, because that is the
// tree that decides how long felling can *ever* take, and it is what the ceiling has
// to be held against.
inline constexpr float Toughest() {
    float most = 0.0f;

    for (const SpeciesDef &def : kSpecies) {
        if (def.growth.toughness > most) most = def.growth.toughness;
    }

    return most * kStatureMost;
}

// The longest a tree may take to fell by hand, in seconds. **This is the knob.**
inline constexpr float kFellSeconds = 8.0f;

// Seconds a bare hand takes over one log, worked back from that ceiling.
//
// Minecraft's own answer would be 3 -- an oak log is hardness 2 and drops to a fist,
// so it costs the harvesting rate, 2 x 1.5. It is deliberately not used, and the
// reason is a difference between the two games rather than taste. Minecraft spends
// those seconds on five *separate* blocks, each breaking and dropping as it goes; a
// tree here is one object that comes down whole, so the same fifteen seconds would be
// spent in front of something that does not change until the last instant of it.
// Fifteen seconds of nothing is not the same feature as five times three seconds of
// something.
//
// Derived rather than written down, so a tougher species added to the table cannot
// quietly walk past the ceiling. The species keep their differences among themselves;
// what is fixed is the longest any of them may take.
inline constexpr float kLogSeconds = kFellSeconds / Toughest();

// The tree a sapling grows into, or nothing where the item is not a sapling.
//
// A scan of five rows, done once on a click and once a frame while a sapling is
// in hand, which is nothing at all — and it keeps the pairing stated exactly once,
// in the table, rather than as a table plus a switch somewhere that has to be
// remembered when a sixth tree arrives.
// Every sapling a species names has to be an item the hand will plant.
//
// The counterpart of fixture::KindsArePlaceable, and it catches the same class of
// mistake: a row that names the wrong item is a tree whose sapling cannot be put in
// the ground, and the only way to find that out was to fell one and try.
inline constexpr bool SaplingsArePlantable() {
    for (std::size_t e = 0; e < kSpeciesCount; e++) {
        const std::optional<Item> sapling = kSpecies[e].sapling;

        if (sapling.has_value() && Def(*sapling).placement != Placement::Plant) return false;
    }

    return true;
}

static_assert(SaplingsArePlantable(), "a species names a sapling the hand will not plant");

inline constexpr std::optional<Species> SpeciesOf(Item sapling) {
    for (std::size_t e = 0; e < kSpeciesCount; e++) {
        // The second test is what makes the default safe: a species with no
        // sapling of its own leaves the field at an item that is not planted, so
        // it can never answer for one.
        if (kSpecies[e].sapling == sapling) {
            return static_cast<Species>(e);
        }
    }

    return std::nullopt;
}

inline constexpr const SpeciesDef &Def(Species species) {
    return kSpecies[SpeciesIndex(species)];
}

// Whether a species will take root in a ground.
//
// `cover` is what the surface is made of, or nothing where it is bare rock — the
// shape SurfaceCoverAt answers in, and the shape a world lookup answers in too,
// so the scatter and the hand ask the same question of the same rule. Anything
// that is not a cover is refused: a tree cannot be planted in a torch.
inline constexpr bool RootsIn(const SpeciesDef &def, std::optional<Element> cover) {
    if (!cover.has_value()) return false;

    switch (*cover) {
    case Element::Soil: return def.ground.soil;
    case Element::Sand: return def.ground.sand;
    case Element::Snow: return def.ground.snow;
    default: break;
    }

    return false;
}

// How far either side of its species' nominal time one plant's own pace may fall.
//
// The spread Minecraft gets from rolling a one-in-seven chance against a random
// tick, which is what makes two saplings planted together come up minutes apart
// — and the thing a fixed maturity cannot express at all. There it is exponential
// and unbounded; here it is a flat roll between these two, which gives the same
// thing that actually matters (no two trees on one clock) without the tail that
// would leave one sapling in a row still ankle-high an hour later.
//
// At these figures a birch is anywhere from seven to thirty-five minutes and a
// pine from thirteen to sixty-six, against a day of twenty-four — so a wood is
// visibly of mixed age while it comes up, and nothing in it matures inside the
// afternoon it was planted.
inline constexpr float kVigourLeast = 0.45f;
inline constexpr float kVigourMost  = 2.2f;

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
    // nothing to stand on. This is the test that keeps a tree off the *edge* of a
    // shaft: a column open into a cave is not a steep slope, it is a hole, and
    // the slope test above averages straight over it.
    float dropLimit = 44.0f;

    // How far below the land's own surface a plant may take root, in pixels.
    //
    // The test above catches the edge of a hole and not its floor. Where an
    // entrance is wide, the ground the sky finds is a ledge well down inside the
    // shaft — level, with level ground either side of it, so every footing test
    // passes and a tree grows halfway down a cliff with its roots in the air over
    // a cave. Nothing about the local shape says anything is wrong; the only way
    // to know is to compare against where the land's surface actually is.
    //
    // Generous rather than tight, because the ground the scan reports is up to a
    // lattice step off and the surface is terraced into risers besides. What it
    // has to exclude is a cave mouth, and a cave mouth is far deeper than this.
    float rootLimit = 40.0f;
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
        {.cellSpan = 110.0f, .interlock = 0.55f, .coverage = 0.9f},
        {.cellSpan   = 26.0f,
         .interlock  = 0.8f,
         .coverage   = 0.5f,
         .slopeLimit = 0.85f,
         .slopeSpan  = 18.0f,
         .dropLimit  = 26.0f},
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

    // How far each column's top lies below the land's own surface, in pixels.
    //
    // Zero over ordinary ground, and a long way from it down the inside of a cave
    // mouth — which is the one thing `top` alone cannot say, since a ledge in a
    // shaft is indistinguishable from a ledge on a hill by any local measurement.
    // See LayerRules::rootLimit.
    //
    // Optional: a null pointer reads as zero everywhere, which is the right
    // answer for a caller that has no caves to worry about.
    const float *sunk = nullptr;

    int count     = 0;
    float originX = 0.0f;
    float spacing = 1.0f;
};

// Surface at a world position, from the nearest column prepared.
float GroundAt(const Ground &ground, float worldX);

// How far the ground at a position lies below the land's own surface. See
// Ground::sunk.
float SunkAt(const Ground &ground, float worldX);

// One plant the world grows, before anything that has happened to it.
struct Plant {
    // Which layer and which cell it grew in, through PlantId — the whole of its
    // identity: the same number on every frame and in every session, and therefore
    // what a record of damage is filed under.
    std::int64_t id = 0;

    Species species = Species::Oak;

    // Where the trunk meets the ground.
    Vector2 base{};

    // Size at maturity, against the sizes in the table. Carries both the plant's
    // own variation and how well the place suits it, so a species at the edge of
    // its range stands there stunted rather than absent.
    float scale = 1.0f;
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
