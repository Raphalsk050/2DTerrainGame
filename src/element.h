#pragma once

#include "config.h"
#include "raylib.h"
#include "terrain.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

// Materials the world is made of.
//
// Each one is a scalar field over the same lattice, drawn by the same marching
// squares routine and simulated by the same rules. Everything that used to be
// decided by naming a particular element somewhere in the code is declared here
// instead, so a new material is one row in the table below and nothing else.
// Ores are listed in the order they are met on the way down, which is also the
// order of their worth, so the hotbar reads as a progression.
enum class Element { Rock, Coal, Copper, Iron, Gold, Diamond, Emerald, Water, Torch, Count };

inline constexpr std::size_t kElementCount = static_cast<std::size_t>(Element::Count);

inline constexpr std::size_t ElementIndex(Element element) {
    return static_cast<std::size_t>(element);
}

// How the world treats a material.
struct ElementRules {
    // Stops characters. Collision tests the union of every element that sets
    // it, so two solids never have to know about each other.
    bool blocksBodies = false;

    // Stops liquids. A solid a liquid should seep through leaves this off.
    bool blocksLiquid = false;

    // Runs through the liquid automaton, and is drawn as a liquid rather than
    // as terrain.
    bool flows = false;

    // Upward push on a submerged body, relative to water. Zero for anything a
    // body does not float in.
    float buoyancy = 0.0f;

    // Owns the space it sits in. Two materials that both set this never share a
    // lattice vertex: where both would be present, the one with the higher
    // `precedence` keeps the vertex and the other is cut away around it.
    //
    // That is what makes "what is here?" a question with one answer, which is
    // the whole basis of mining: a vertex holds a material or it holds nothing,
    // and digging it out uncovers exactly one thing.
    bool occupies = false;

    // Which of two occupying materials keeps a contested vertex. Higher wins,
    // so an ore is worth more than the rock it displaces.
    //
    // Every occupying material needs its own value. Two sharing one would
    // overlap, since neither gives way to the other.
    int precedence = 0;
};

// Where a material comes from.
enum class Generator {
    None,    // Only ever placed by hand.
    Terrain, // The base landscape, shaped by terrain::Settings.
    Vein,    // Patches scattered through the ground by their own noise.
    Pool,    // Liquid standing in the open space the ground leaves behind.
};

// Which part of the world a material is allowed to generate in.
enum class SpawnSpace {
    Anywhere,
    InsideGround, // Ore: only where the base terrain already fills the space.
    OpenSpace,    // Liquid: only where the base terrain leaves the space empty.
};

// Stands in for "no limit" at a magnitude no world coordinate reaches, so an
// unrestricted band needs no special case anywhere it is used.
inline constexpr float kUnboundedDepth = 1.0e9f;

// Where in the world's depth a material is found, and how much of it there is at
// each height.
//
// Y grows downward, so `top` is the shallowest height the material reaches and
// `bottom` the deepest, with `peak` the height it is densest at. From the peak to
// either edge the amount of it falls away linearly. This is the shape Minecraft
// settled on and it reads correctly underground: an ore has a level it belongs
// to, rather than a slab it fills uniformly and then stops filling.
//
// All three are absolute world coordinates rather than depths below the surface,
// which is what makes them behave like ore levels: the same material is found
// between the same heights everywhere in the world, and a chunk does not need to
// know what the ground above it looks like to place it correctly.
struct DepthBand {
    float top    = -kUnboundedDepth;
    float bottom = kUnboundedDepth;
    float peak   = 0.0f;

    // Extra height the material's own noise has to clear outside its band, in
    // the units of that noise, which run [0,1].
    //
    // Not a boundary, which is the point. A material away from its own level
    // becomes rare rather than impossible, so a seam of something deep can still
    // turn up shallow — it just needs the noise to have peaked there. A hard
    // edge is what made the world read as a stack of painted slabs, and it is
    // also what made a find out of place worth nothing, because there were none.
    //
    // Zero leaves the material as common everywhere as at its peak. A third is a
    // marked thinning; past about a half it is effectively absent outside the
    // band.
    float scarcity = 0.35f;

    // Amplitude in pixels by which the edges wander, so a band follows the lie
    // of the ground instead of a spirit level. Taken from the material's own
    // noise along the horizontal axis, so it costs no extra field to sample.
    float jitter = 0.0f;

    bool Bounded() const { return top > -kUnboundedDepth || bottom < kUnboundedDepth; }
};

struct ElementSpawn {
    Generator generator = Generator::None;

    // Shape of the material's own noise. Its seed must differ from the
    // terrain's, or the material follows the outline of the ground and reads as
    // painted on.
    terrain::NoiseShape shape{};

    // How likely the material is at the height it is most abundant, as a share of
    // the eligible space, in [0,1].
    //
    // A probability, not a cutoff. The cutoff that achieves it is measured from the
    // noise itself when the world is created, so reshaping a vein does not
    // silently change how much of the material there is. Eligible means on the
    // right side of `space`, and the height is `band.peak`; everywhere else the
    // band thins it out from here.
    float probability = 0.0f;

    // How many lattice cells across one vein is, at its widest. Zero leaves the
    // size to `shape.frequency`, which is how a liquid describes itself, having no
    // veins to size.
    //
    // Separate from `probability` on purpose, because they are different questions.
    // The probability says how much of the material there is; this says whether it
    // arrives as a few broad seams or a scatter of specks. Lowering the probability
    // on its own leaves the veins exactly where they were and merely shaves them
    // down, which is why an ore can be made rare and still feel like it is
    // everywhere.
    //
    // Not a hard ceiling. A vein is the part of one noise feature that clears the
    // cutoff, and how much of it that is depends on where the feature happened to
    // peak; capping it outright would need the generator to know the extent of a
    // shape it evaluates one vertex at a time, which is exactly what it cannot do.
    // What this sets is the size the veins come out at, and the largest run to
    // roughly half again as wide.
    float veinCells = 0.0f;

    SpawnSpace space = SpawnSpace::Anywhere;

    DepthBand band{};
};

// Product of a vein's width in pixels and the frequency of the noise that made it.
//
// Measured across the whole table rather than derived. A vein is the cap of one
// noise feature standing above the cutoff, so its width is a fixed fraction of
// that feature's, and the two are therefore inversely proportional; this is the
// constant relating them, and it held to within eight per cent over ores from
// three to eight cells wide and a twenty-five-fold range of probability.
inline constexpr float kVeinFeatureSpan = 237.0f;

// The material's noise.
//
// Always taken through this rather than read off `shape` directly. For a vein the
// frequency is worked out from `veinCells`, because the size of a vein is the thing
// worth writing down and a frequency in features per thousand pixels is merely how
// it is arranged.
inline terrain::NoiseShape SpawnNoise(const ElementSpawn &spawn) {
    if (spawn.veinCells <= 0.0f) return spawn.shape;

    terrain::NoiseShape shape = spawn.shape;
    shape.frequency           = kVeinFeatureSpan / std::max(spawn.veinCells * config::kResolution, 1.0f);

    return shape;
}

// How a material takes part in lighting.
//
// Kept apart from the rules above because light does not follow from any of
// them. Water stops a body and lets light through; a torch stops nothing and is
// the brightest thing in the world. Deriving one from the other would make
// every new material an argument about which existing one it resembles.
struct ElementLight {
    // Share of the light stopped by one cell of the material, in [0,1]. One is
    // a wall. Below one the material tints and dims what passes through it,
    // which is what deep water should do and what a gas will want later.
    float opacity = 1.0f;

    // Colour of the light the material gives off, and how much of it. Strength
    // is in the same unit the sky is measured in, so a torch can be set against
    // daylight and read as a fraction of it rather than as an arbitrary number.
    Color glow     = {0, 0, 0, 0};
    float strength = 0.0f;
};

struct ElementDef {
    const char *name;

    // Field value at which the material counts as present, for drawing and for
    // every rule above alike.
    float threshold;

    Color fill; // Zero alpha leaves the material unfilled.
    Color contour;

    ElementRules rules;
    ElementSpawn spawn;
    ElementLight light;
};

inline constexpr ElementDef kElements[] = {
    {
        .name = "rock",

        // Taken from the generator rather than written down again. The terrain
        // field is a signed distance mapped into [0,1], and this is the value it
        // crosses where that distance is zero; a second copy of the number here
        // would let the rock's outline drift away from the ground the generator
        // describes.
        .threshold = terrain::kSurfaceLevel,

        .fill    = {105, 115, 130, 255},
        .contour = {0, 82, 172, 255},
        .rules   = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 0},
        .spawn   = {.generator = Generator::Terrain},
        .light   = {.opacity = 0.8f},
    },
    // The ores below follow Minecraft's set and its ordering, on a scale of one
    // block to sixteen pixels: its sea level at Y 64 is this world's y 144 and
    // its floor at Y -64 is y 2192. What is not carried across is its absolute
    // heights, since it has a floor to arrange them against and this world does
    // not, so each peak sits where the ore is actually worth digging for here.
    //
    // Each ore is written as three numbers and a level: how likely it is where it
    // is densest, how many lattice cells across one vein of it is, how quickly it
    // thins out away from that level, and the level itself.
    {
        .name      = "coal",
        .threshold = 0.45f,
        .fill      = {58, 58, 66, 255},
        .contour   = {26, 26, 32, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 1},
        .spawn =
            {
                // The commonest ore and the largest veins, as in Minecraft, where
                // it is seventeen blocks to iron's four.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8103},
                .probability = 0.034f,
                .veinCells   = 8.0f,
                .space       = SpawnSpace::InsideGround,

                // Shallow: the first thing dug up, thinning out well before the
                // depths the later ores belong to.
                .band = {.top = 200.0f, .bottom = 1600.0f, .peak = 480.0f, .scarcity = 0.17f, .jitter = 48.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "copper",
        .threshold = 0.45f,
        .fill      = {196, 110, 74, 255},
        .contour   = {132, 66, 40, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 2},
        .spawn =
            {
                // Shares coal's range almost exactly in Minecraft, and does here
                // too, so the shallow depths have two things worth mining rather
                // than one.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8105},
                .probability = 0.0154f,
                .veinCells   = 6.5f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 220.0f, .bottom = 1800.0f, .peak = 560.0f, .scarcity = 0.18f, .jitter = 44.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "iron",
        .threshold = 0.45f,
        .fill      = {150, 143, 134, 255},
        .contour   = {92, 86, 79, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 3},
        .spawn =
            {
                // The broadest band of any ore, and small veins. Found nearly
                // anywhere underground, which is what makes it the material
                // everything ordinary is built out of.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8101},
                .probability = 0.0062f,
                .veinCells   = 6.0f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 400.0f, .bottom = 3200.0f, .peak = 1500.0f, .scarcity = 0.16f, .jitter = 56.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "gold",
        .threshold = 0.45f,
        .fill      = {222, 183, 64, 255},
        .contour   = {148, 114, 20, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 4},
        .spawn =
            {
                // A fifth of iron's share, and deep. The lower frequency is what
                // keeps that share as a few seams worth finding rather than
                // dust scattered evenly through the rock.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8104},
                .probability = 0.00065f,
                .veinCells   = 3.5f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 700.0f, .bottom = 3400.0f, .peak = 1800.0f, .scarcity = 0.20f, .jitter = 64.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "diamond",
        .threshold = 0.45f,
        .fill      = {104, 224, 226, 255},
        .contour   = {40, 146, 154, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 5},
        .spawn =
            {
                // The deepest and by a distance the rarest: a quarter of one per
                // cent of the rock even where it is densest. Minecraft puts its
                // peak five blocks off the floor of the world, and the shape of
                // that is what is copied here — almost all of it in the last
                // stretch before there is nothing below.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8106},
                .probability = 0.00028f,
                .veinCells   = 3.5f,
                .space       = SpawnSpace::InsideGround,
                .band =
                    {.top = 1100.0f, .bottom = kUnboundedDepth, .peak = 2400.0f, .scarcity = 0.22f, .jitter = 72.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "emerald",
        .threshold = 0.45f,
        .fill      = {72, 206, 118, 255},
        .contour   = {26, 124, 62, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 6},
        .spawn =
            {
                // The odd one out: in Minecraft it is found only in mountains and
                // gets denser the higher it goes, which makes it the one ore that
                // is a reward for climbing rather than for digging. Until there
                // are biomes to hang that on, it is here as the shallowest and
                // rarest of the set, which keeps the shape of the idea if not the
                // rule behind it.
                .generator   = Generator::Vein,
                .shape       = {.octaves = 2, .seed = 8107},
                .probability = 0.00060f,
                .veinCells   = 3.0f,
                .space       = SpawnSpace::InsideGround,
                .band        = {.top = 170.0f, .bottom = 1100.0f, .peak = 320.0f, .scarcity = 0.24f, .jitter = 40.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name = "water",

        // The threshold is low on purpose. The field holds mass, not height,
        // and a lattice cell is the smallest thing that can be drawn: a stream
        // carrying a third of a unit is physically a third of a cell across,
        // which the grid cannot express. Only two outcomes are available,
        // nothing or a whole cell, and a high threshold picks nothing, so
        // running water breaks into disconnected pieces or vanishes.
        .threshold = 0.15f,

        .fill    = {102, 191, 255, 255},
        .contour = {56, 152, 236, 255},
        .rules   = {.flows = true, .buoyancy = 1.0f},
        .spawn =
            {
                // Lower than the ores by an order of magnitude: a flooded
                // cavern is a place, not a pocket, and at the ores' frequency
                // the water table would break into puddles the size of a vein.
                .generator = Generator::Pool,
                .shape     = {.frequency = 2.2f, .octaves = 2, .seed = 8102},

                // Only part of the underground is wet. At a probability of one, every
                // cavity in the band floods, which leaves nowhere to walk.
                .probability = 0.30f,
                .space       = SpawnSpace::OpenSpace,

                // Below the surface: an underground water table, not rain.
                // Standing water on open ground would be a separate band with
                // its own top at the surface.
                //
                // A liquid is the one thing here that keeps a hard edge, so the
                // peak sits midway and the scarcity is zero: a water table that
                // thinned out with depth instead of ending would be a mist rather
                // than a surface.
                .band = {.top = 340.0f, .bottom = 2600.0f, .peak = 1470.0f, .scarcity = 0.0f, .jitter = 40.0f},
            },

        // Dims light rather than stopping it, so a pool reads as deep by
        // getting darker towards the bottom instead of turning into a wall the
        // moment it is thick enough to swim in.
        .light = {.opacity = 0.10f},
    },
    {
        .name      = "torch",
        .threshold = 0.45f,
        .fill      = {255, 216, 150, 255},
        .contour   = {196, 128, 44, 255},

        // Claims its vertex, so it replaces what it is put on and can be mined
        // back out, but stops nothing: a body walks through it and so does
        // water. A torch that cast its own shadow would sit in a dark spot of
        // its own making.
        //
        // Outranks every ore, so a torch driven into a seam replaces it rather
        // than being swallowed by it.
        .rules = {.occupies = true, .precedence = 7},
        .spawn = {.generator = Generator::None},

        // Brighter than the sky, because it has to carry a room on its own
        // while daylight arrives from every direction at once. Not by much,
        // though: this is the light given off by every cell the brush covers,
        // and a wide brush lays down a great many of them.
        .light = {.opacity = 0.0f, .glow = {255, 198, 130, 255}, .strength = 2.5f},
    },
};

// An enumerator without its row would otherwise read past the table, and the
// first symptom is a hotbar slot drawing from whatever follows it in memory.
static_assert(std::size(kElements) == kElementCount, "every Element needs exactly one row in kElements");

// Two occupying materials sharing a precedence would overlap, since neither one
// gives way to the other, and the vertex they share would have no single
// answer to what is in it.
consteval bool PrecedencesAreDistinct() {
    for (std::size_t a = 0; a < kElementCount; a++) {
        if (!kElements[a].rules.occupies) continue;

        for (std::size_t b = a + 1; b < kElementCount; b++) {
            if (!kElements[b].rules.occupies) continue;
            if (kElements[a].rules.precedence == kElements[b].rules.precedence) return false;
        }
    }

    return true;
}

static_assert(PrecedencesAreDistinct(), "every occupying element needs its own precedence");

inline constexpr const ElementDef &Def(Element element) {
    return kElements[ElementIndex(element)];
}

// Vertical displacement of a band's two edges at a horizontal position.
//
// Sampled along one axis only, so the edge undulates like ground instead of
// running dead level, and taken from the material's own noise so that a band
// and the veins inside it belong to the same shape.
inline float BandWobble(const ElementSpawn &spawn, float worldX) {
    if (spawn.band.jitter <= 0.0f) return 0.0f;

    return (terrain::Sample({worldX, 0.0f}, SpawnNoise(spawn)) - 0.5f) * 2.0f * spawn.band.jitter;
}

// Distance into a band at a world position, in pixels: positive inside,
// negative outside, zero exactly on an edge.
//
// Expressed as a distance rather than a yes-or-no test so that the field can
// be faded across the boundary and the contour still has a gradient to
// interpolate through.
inline float BandDepth(const ElementSpawn &spawn, Vector2 world) {
    const float wobble = BandWobble(spawn, world.x);

    return std::min(world.y - (spawn.band.top + wobble), (spawn.band.bottom + wobble) - world.y);
}

// Share of its peak abundance a material has at a world position, in [0,1]: one
// at the peak of its band, falling away linearly to zero at either edge.
//
// The two arms are independent, so an ore can reach a long way down from its peak
// and only a little way up, which is the shape most of them have. An arm running
// to kUnboundedDepth simply never falls off on that side.
inline float BandAbundance(const ElementSpawn &spawn, Vector2 world) {
    const DepthBand &band = spawn.band;
    if (!band.Bounded()) return 1.0f;

    const float y    = world.y - BandWobble(spawn, world.x);
    const float peak = std::clamp(band.peak, band.top, band.bottom);

    // Above the peak the shallow arm decides, below it the deep one. An arm of no
    // length cannot be interpolated across, so it reads as a hard edge, which is
    // the only sensible answer for a peak sitting on its own boundary.
    const float arm      = (y <= peak) ? (peak - band.top) : (band.bottom - peak);
    const float distance = (y <= peak) ? (y - band.top) : (band.bottom - y);

    if (arm <= 0.0f) return (distance >= 0.0f) ? 1.0f : 0.0f;

    return std::clamp(distance / arm, 0.0f, 1.0f);
}

// Extra height a material's noise has to clear at a world position, which is how
// it thins out away from its own level instead of stopping at the edge of it.
inline float BandPenalty(const ElementSpawn &spawn, Vector2 world) {
    return spawn.band.scarcity * (1.0f - BandAbundance(spawn, world));
}

inline constexpr const ElementDef &StyleOf(Element element) {
    return Def(element);
}
