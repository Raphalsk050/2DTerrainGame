#pragma once

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
enum class Element { Rock, Coal, Iron, Gold, Water, Torch, Count };

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

// Range of world height a material generates in.
//
// Y grows downward, so `top` is the shallowest height the material reaches and
// `bottom` the deepest. Both are absolute world coordinates rather than depths
// below the surface, which is what makes them behave like ore levels: the same
// material is found between the same two heights everywhere in the world, and
// a chunk does not need to know what the ground above it looks like to place
// it correctly.
struct DepthBand {
    float top    = -kUnboundedDepth;
    float bottom = kUnboundedDepth;

    // Distance in pixels over which the material thins out as it approaches
    // either edge. It sets how abruptly a vein is cut off; at zero the band
    // would end on one exact height and the cut would read as a machined edge.
    float fade = 48.0f;

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

    // Share of the eligible space this material should occupy, in [0,1].
    //
    // A probability, not a cutoff. The cutoff that achieves it is measured from
    // the noise itself when the world is created, so reshaping the veins does
    // not silently change how much ore there is. Eligible means inside the
    // band and on the right side of `space`, so narrowing either one keeps the
    // density the number asks for rather than diluting it.
    float coverage = 0.0f;

    SpawnSpace space = SpawnSpace::Anywhere;

    DepthBand band{};
};

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
        .name      = "rock",
        .threshold = 0.45f,
        .fill      = {105, 115, 130, 255},
        .contour   = {0, 82, 172, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 0},
        .spawn     = {.generator = Generator::Terrain},
        .light     = {.opacity = 1.0f},
    },
    {
        .name      = "coal",
        .threshold = 0.45f,
        .fill      = {58, 58, 66, 255},
        .contour   = {26, 26, 32, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 1},
        .spawn =
            {
                // Frequency counts features per kFeatureSpan pixels, so this is
                // what sets the size of a vein: around a dozen across that span
                // puts a pocket at roughly the scale of the brush that digs it
                // out. Low values are the trap here. At one feature per span a
                // vein is wider than the screen, and the coverage arrives as a
                // handful of continents instead of something to go looking for.
                .generator = Generator::Vein,
                .shape     = {.frequency = 13.0f, .octaves = 2, .seed = 8103},
                .coverage  = 0.10f,
                .space     = SpawnSpace::InsideGround,

                // Shallow and wide: the first thing dug up, and still there
                // well past the depth the other ores start at.
                .band = {.top = 400.0f, .bottom = 1500.0f, .fade = 64.0f, .jitter = 48.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "iron",
        .threshold = 0.45f,
        .fill      = {150, 143, 134, 255},
        .contour   = {92, 86, 79, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 2},
        .spawn =
            {
                .generator = Generator::Vein,
                .shape     = {.frequency = 17.0f, .octaves = 2, .seed = 8101},
                .coverage  = 0.06f,
                .space     = SpawnSpace::InsideGround,
                .band      = {.top = 520.0f, .bottom = 2400.0f, .fade = 80.0f, .jitter = 56.0f},
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "gold",
        .threshold = 0.45f,
        .fill      = {222, 183, 64, 255},
        .contour   = {148, 114, 20, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 3},
        .spawn =
            {
                .generator = Generator::Vein,

                // Higher frequency than iron, so its smaller share arrives as
                // many little pockets rather than a few rich seams.
                .shape    = {.frequency = 26.0f, .octaves = 2, .seed = 8104},
                .coverage = 0.03f,
                .space    = SpawnSpace::InsideGround,
                .band     = {.top = 1600.0f, .bottom = kUnboundedDepth, .fade = 120.0f, .jitter = 64.0f},
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

                // Only part of the underground is wet. At full coverage every
                // cavity in the band floods, which leaves nowhere to walk.
                .coverage = 0.30f,
                .space    = SpawnSpace::OpenSpace,

                // Below the surface: an underground water table, not rain.
                // Standing water on open ground would be a separate band with
                // its own top at the surface.
                .band = {.top = 340.0f, .bottom = 2600.0f, .fade = 32.0f, .jitter = 40.0f},
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
        .rules = {.occupies = true, .precedence = 4},
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

    return (terrain::Sample({worldX, 0.0f}, spawn.shape) - 0.5f) * 2.0f * spawn.band.jitter;
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

inline constexpr const ElementDef &StyleOf(Element element) {
    return Def(element);
}
