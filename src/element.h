#pragma once

#include "config.h"
#include "raylib.h"
#include "terrain.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

// Materials the world is made of.
//
// Each one is a scalar field over the same lattice, drawn by the same marching
// squares routine and simulated by the same rules. Everything that used to be
// decided by naming a particular element somewhere in the code is declared here
// instead, so a new material is one row in the table below and nothing else.
//
// The order is the order the hotbar shows them in, and it reads as two runs: what
// the ground is made of near the top, where a player builds, and then the ores in
// the order they are met on the way down, which is also the order of their worth.
enum class Element {
    Rock,
    Soil,
    Sand,
    Snow,
    Torch,
    Coal,
    Copper,
    Iron,
    Gold,
    Diamond,
    Emerald,
    Water,
    Count
};

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
    Cover,   // A skin over the landscape, as thick as the climate makes it.
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

    // Reads the three heights above as depths below the surface rather than as
    // absolute world Y.
    //
    // An ore level is absolute on purpose, for the reasons written above it. A
    // cover is the opposite kind of thing: it is a skin on the land and has to
    // ride over every hill and down into every valley, which is precisely what an
    // absolute band cannot say. One flag rather than a second kind of band,
    // because everything else about the shape — the peak, the two arms, the
    // scarcity, the wobble — means exactly the same thing measured either way.
    bool relative = false;

    bool Bounded() const { return top > -kUnboundedDepth || bottom < kUnboundedDepth; }
};

// Where a material belongs, as a place in the two climate fields rather than as a
// list of biomes.
//
// A centre and a width per axis, read as a bell: densest at the centre, thinning
// to nothing about a width away. Deliberately the same shape as
// flora::SpeciesClimate, so that when there is a biome table it supplies weights
// that multiply a suitability which already exists, rather than replacing it.
// Until then this *is* the biome selection, and it is what puts sand in a desert
// and snow on the cold ground.
//
// Altitude needs no term of its own. terrain::ClimateSettings already cools the
// air by `temperatureLapse` per pixel of elevation, so a material that wants the
// tops only has to ask for cold and the peaks answer.
struct ElementClimate {
    float temperature = 0.5f;
    float humidity    = 0.5f;

    // A width of kUnboundedDepth reads as "anywhere", since the bell then never
    // falls off. That is what a material present the world over asks for, and it
    // is why the defaults here mean no climate preference at all.
    float temperatureWidth = kUnboundedDepth;
    float humidityWidth    = kUnboundedDepth;

    // Suitability at which the material lies at its full depth, and the
    // suitability below which it is not there at all.
    //
    // A pair rather than the bell used raw, and both halves of it were measured
    // rather than argued. Two bells multiplied peak only where both axes peak *in
    // the same column*, and the two climate fields are decorrelated by
    // construction — so the joint suitability of the hottest, driest place in a
    // hundred and twenty thousand pixels came out at 0.68, and sand was drawn at
    // two thirds depth at its very best and at a fifth of it everywhere else.
    // What that paints is not a desert; it is a dusting of sand over half a
    // county.
    //
    // Dividing by a ceiling fixed the depth and made the second fault worse: a
    // Gaussian's tail is long, so the fringe simply got thicker as well as
    // wider, and a fifth of the world was slightly sandy. What a desert has is an
    // edge. So the bell is spent on *where* that edge falls rather than on how
    // deep the middle is, and the run between these two numbers is how far it
    // takes to cross it — narrow on purpose, the same argument
    // flora::Settings::edgeBand makes about the border of a wood.
    float fullAt = 0.55f;
    float goneAt = 0.30f;
};

// The smooth ramp between two edges, in [0,1].
inline float ClimateRamp(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-3f), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

// How well a climate suits a material, in [0,1], before the shaping above.
inline float ClimateBell(const ElementClimate &wants, float temperature, float humidity) {
    const float dt = (temperature - wants.temperature) / std::max(wants.temperatureWidth, 1e-3f);
    const float dh = (humidity - wants.humidity) / std::max(wants.humidityWidth, 1e-3f);

    return std::exp(-dt * dt - dh * dh);
}

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

    // How much more likely the material is against the wall of a cave than in the
    // middle of the rock, and how far from a wall that pull reaches, in pixels.
    //
    // The bias is added to the noise before it meets its cutoff, so it lowers the
    // bar rather than placing anything: the veins are still the veins, there are
    // simply more of them where the rock is open. A reach of a few tens of pixels
    // is a seam in a cave wall; hundreds would be a district, which is a different
    // idea and belongs to a field of its own.
    //
    // It exists because ore spread evenly through the rock rewards digging blind
    // exactly as much as exploring, and a cave that pays no better than the rock
    // beside it is scenery with a draught. Pulling the seams onto the walls is
    // what makes walking a passage worth more than tunnelling, and it is the same
    // move Minecraft makes with its copper and iron veins and Terraria with the
    // ore it leaves showing in a cavern wall.
    //
    // `World::CalibrateSpawn` measures the cutoff with this included, so the
    // probability above still means what it says: the material is not made more
    // common by being biased, it is moved.
    float wallBias  = 0.0f;
    float wallReach = 40.0f;

    // How thick a cover lies where its climate suits it best, in pixels, and how
    // much its own noise thickens and thins that along the way.
    //
    // The variation is not decoration. A cover of one thickness meets the rock
    // under it along a line as straight as a spirit level, which reads as a
    // painted stripe rather than as ground; a cover whose base wanders is soil
    // that has gathered where it could.
    //
    // Nothing may ask for more than kCoverCeiling — see the constant.
    float thickness     = 0.0f;
    float thicknessVary = 0.0f;

    // Where the material belongs, and how far the noise is allowed to argue with
    // that at the border.
    //
    // The jitter is added to the climate weight before it is clamped, so it does
    // nothing at all in the middle of a range — one either side of the border the
    // weight is already past both ends of the clamp — and everything at the edge,
    // where it breaks the transition into patches of one material and patches of
    // the other. A border with no jitter is a line drawn across the world, and
    // there is no such line in any desert.
    ElementClimate climate{};
    float climateJitter = 0.0f;
};

// The furthest a cover may reach below the surface, in pixels.
//
// CaveSettings::crust keeps every cave layer at least that far under the ground,
// so a cover thinner than it can never open into the roof of a gallery. Ask for
// more and the first thing you meet underground is a ceiling of hanging soil.
inline constexpr float kCoverCeiling = 48.0f;

// Distance between the two places a cover reads its own noise: once for its
// thickness, once for the jitter on its climate.
//
// Far enough apart that the two are not one field read twice, which would tie the
// raggedness of a beach's edge to the raggedness of its floor. The same trick, and
// the same reason, as flora::Settings::standStride.
inline constexpr float kCoverStride = 5300.0f;

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

// How many tones a material is authored from, and how many it is drawn from.
//
// Four to write and seven to draw, which is the arrangement the canopy arrived at
// for leaves and for the same two reasons. Four is as many as can be judged
// against each other by eye in a table; seven is as few as a surface can carry a
// lit crest, a body and a shaded belly on without the steps between them reading
// as bands.
inline constexpr std::size_t kElementTones = 4;
inline constexpr int kElementRamp          = 7;

// How a material is painted.
//
// The same separation the plants are drawn with: the tones and the form are one
// job, the texture is another, and they work at different scales. What is
// authored here is never the hour — the light multiply over the whole frame is
// what says what time of day it is, and a tone that had the sun baked into it
// would fight it.
struct ElementPaint {
    // Darkest first.
    Color tone[kElementTones];

    // The per-texel stipple, in tone steps.
    //
    // Under one on purpose, and this is the rule that decides whether the result
    // reads as drawn or as noisy: the texture's job is to break the boundary
    // between two tones the way a pixel artist breaks it by hand, not to decide
    // which tone a region is. At one it starts making that decision, and past it
    // the whole surface is static.
    float grain = 0.55f;

    // The slow drift, in tone steps, over a few hundred pixels.
    //
    // What stops a wall of one material being one colour. Free to be more than a
    // step, because unlike the grain it varies at the scale of the shape rather
    // than of the texel, so what it produces is a lighter patch of rock and not
    // a speckle.
    float patch = 0.9f;

    // Bedding, in tone steps: the same drift again, stretched flat.
    //
    // Rock is laid down in layers and reads wrongly without them — the patch term
    // alone gives clouds inside the stone. Zero for anything with no bedding to
    // show, which is most things that are not stone.
    float strata = 0.0f;
};

struct ElementDef {
    const char *name;

    // Field value at which the material counts as present, for drawing and for
    // every rule above alike.
    float threshold;

    ElementPaint paint;
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

        .paint   = {.tone   = {{62, 70, 84, 255}, {84, 93, 108, 255}, {105, 115, 130, 255}, {132, 143, 158, 255}},

                    .grain  = 0.55f,

                    .patch  = 1.00f,

                    .strata = 1.15f},
        .contour = {0, 82, 172, 255},
        .rules   = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 0},
        .spawn   = {.generator = Generator::Terrain},
        .light   = {.opacity = 0.8f},
    },
    // The three covers. Each one is a skin over the rock as thick as its climate
    // allows, and they stack by precedence alone: sand outranks soil and so
    // replaces it where a desert wants it, snow outranks both but asks for so
    // little depth that what it takes is a cap, leaving the soil underneath.
    // Nothing had to be taught either arrangement — it falls out of the exclusion
    // pass, which is the whole point of ranking materials rather than choosing
    // between them.
    {
        .name = "soil",

        // The same line the rock crosses, for the same reason: a cover's field is
        // its distance to its own edges mapped through kDensitySpan, exactly as
        // the terrain's is, so the two meet on one contour instead of leaving a
        // seam a fraction of a cell wide between them.
        .threshold = terrain::kSurfaceLevel,

        .paint   = {.tone   = {{68, 48, 30, 255}, {95, 68, 42, 255}, {122, 88, 56, 255}, {154, 115, 76, 255}},

                    .grain  = 0.72f,

                    .patch  = 0.95f,

                    .strata = 0.40f},
        .contour = {74, 52, 30, 255},
        .rules   = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 7},
        .spawn =
            {
                .generator = Generator::Cover,

                // One feature every four hundred pixels or so: the scale at which
                // soil reads as having gathered in places rather than as having
                // been spread with a trowel.
                .shape = {.frequency = 2.6f, .octaves = 2, .seed = 8201},
                .space = SpawnSpace::InsideGround,

                // Two Minecraft blocks and a bit, which is what that game puts
                // between the grass and the stone. Deep enough that an ordinary
                // hole stays in the soil and reaching rock is a decision.
                //
                // No climate: soil is what the ground is made of wherever nothing
                // else has claimed it, so its bell is the default one and reads
                // as one everywhere.
                .thickness     = 36.0f,
                .thicknessVary = 10.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "sand",
        .threshold = terrain::kSurfaceLevel,
        .paint   = {.tone   = {{148, 128, 84, 255}, {181, 160, 108, 255}, {214, 192, 134, 255}, {240, 224, 178, 255}},
                    .grain  = 0.50f,
                    .patch  = 0.70f,
                    .strata = 0.60f},
        .contour   = {166, 144, 92, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 8},
        .spawn =
            {
                .generator = Generator::Cover,
                .shape     = {.frequency = 2.2f, .octaves = 2, .seed = 8203},
                .space     = SpawnSpace::InsideGround,

                // Nearly as deep as the soil it displaces, so a desert is sand to
                // dig through and not a dusting over brown ground.
                .thickness     = 32.0f,
                .thicknessVary = 9.0f,

                // Hot and dry, and well past where any tree in the table grows —
                // the hottest of those is the apple at 0.60 — so a desert comes
                // out bare rather than wooded, without anything having to say so.
                .climate       = {.temperature      = 0.88f,
                                  .humidity         = 0.14f,
                                  .temperatureWidth = 0.22f,
                                  .humidityWidth    = 0.26f,
                                  .fullAt           = 0.45f,
                                  .goneAt           = 0.26f},
                .climateJitter = 0.16f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "snow",
        .threshold = terrain::kSurfaceLevel,
        .paint   = {.tone   = {{168, 182, 205, 255}, {201, 213, 231, 255}, {234, 239, 248, 255}, {252, 254, 255, 255}},
                    .grain  = 0.34f,
                    .patch  = 0.55f,
                    .strata = 0.00f},
        .contour   = {182, 196, 216, 255},
        .rules     = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 9},
        .spawn =
            {
                .generator = Generator::Cover,

                // The finest of the three, because a snow line is drawn by the
                // shape of the hill it lies on and a coarse field would drape it
                // across two valleys at once.
                .shape = {.frequency = 3.4f, .octaves = 2, .seed = 8205},
                .space = SpawnSpace::InsideGround,

                // A cap and not a layer. Thin enough that the soil it sits on is
                // still there to be dug, which is what keeps a snowfield ground
                // rather than a different world.
                .thickness     = 11.0f,
                .thicknessVary = 5.0f,

                // Colder than anything else asks for — the pine, the coldest tree
                // in the table, centres on 0.30. Narrow with it, so snow is the
                // tops and the far north and not a general chill: the climate
                // loses 0.0011 of temperature per pixel of elevation, so the peaks
                // reach this on their own without altitude being named here.
                //
                // Indifferent to how wet it is, since what falls as snow is
                // decided by the cold alone.
                .climate       = {.temperature      = 0.10f,
                                  .humidity         = 0.5f,
                                  .temperatureWidth = 0.18f,
                                  .humidityWidth    = kUnboundedDepth,
                                  .fullAt           = 0.50f,
                                  .goneAt           = 0.32f},
                .climateJitter = 0.14f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "torch",
        .threshold = 0.45f,
        .paint   = {.tone   = {{196, 128, 44, 255}, {226, 170, 92, 255}, {255, 216, 150, 255}, {255, 242, 210, 255}},
                    .grain  = 0.30f,
                    .patch  = 0.30f,
                    .strata = 0.00f},
        .contour   = {196, 128, 44, 255},

        // Claims its vertex, so it replaces what it is put on and can be mined
        // back out, but stops nothing: a body walks through it and so does
        // water. A torch that cast its own shadow would sit in a dark spot of
        // its own making.
        //
        // Outranks every ore and every cover, so a torch driven into a seam or
        // into a snowbank replaces it rather than being swallowed by it.
        .rules = {.occupies = true, .precedence = 10},
        .spawn = {.generator = Generator::None},

        // Brighter than the sky, because it has to carry a room on its own
        // while daylight arrives from every direction at once. Not by much,
        // though: this is the light given off by every cell the brush covers,
        // and a wide brush lays down a great many of them.
        .light = {.opacity = 0.0f, .glow = {255, 198, 130, 255}, .strength = 2.5f},
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
        .paint   = {.tone   = {{24, 24, 30, 255}, {40, 40, 48, 255}, {58, 58, 66, 255}, {82, 82, 92, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                // Drawn onto the cave walls. See ElementSpawn::wallBias: the ore
                // is moved rather than added, since the cutoff is measured with
                // this in it, so what the bias buys is a reason to walk a passage
                // instead of tunnelling past it.
                .wallBias    = 0.200f,
                .wallReach = 48.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "copper",
        .threshold = 0.45f,
        .paint   = {.tone   = {{116, 56, 34, 255}, {155, 82, 53, 255}, {196, 110, 74, 255}, {228, 150, 112, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                .wallBias    = 0.170f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "iron",
        .threshold = 0.45f,
        .paint   = {.tone   = {{86, 80, 74, 255}, {117, 111, 103, 255}, {150, 143, 134, 255}, {186, 180, 172, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                .wallBias    = 0.100f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "gold",
        .threshold = 0.45f,
        .paint   = {.tone   = {{136, 104, 24, 255}, {179, 143, 43, 255}, {222, 183, 64, 255}, {248, 218, 122, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                .wallBias    = 0.130f,
                .wallReach   = 48.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "diamond",
        .threshold = 0.45f,
        .paint   = {.tone   = {{38, 144, 152, 255}, {70, 184, 189, 255}, {104, 224, 226, 255}, {170, 245, 246, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                .wallBias    = 0.180f,
                .wallReach = 48.0f,
            },
        .light = {.opacity = 1.0f},
    },
    {
        .name      = "emerald",
        .threshold = 0.45f,
        .paint   = {.tone   = {{24, 122, 60, 255}, {47, 163, 88, 255}, {72, 206, 118, 255}, {134, 233, 166, 255}},
                    .grain  = 0.62f,
                    .patch  = 0.75f,
                    .strata = 0.45f},
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

                .wallBias    = 0.035f,
                .wallReach   = 48.0f,
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

        .paint   = {.tone   = {{48, 122, 190, 255}, {74, 156, 223, 255}, {102, 191, 255, 255}, {160, 218, 255, 255}},

                    .grain  = 0.35f,

                    .patch  = 0.70f,

                    .strata = 0.00f},
        .contour = {56, 152, 236, 255},
        .rules   = {.flows = true, .buoyancy = 1.0f},
        // The only row whose extent this table does not describe.
        //
        // Everything else here is a field thresholded against a depth band, and
        // that is the wrong shape for a liquid: a share of the cavities scattered
        // through a band is not water, it is a mist of it, hanging at whatever
        // height the noise happened to clear its cutoff. Water stands at a level.
        // So `Generator::Pool` reads terrain::TableAt instead, and neither the
        // shape, the probability nor the band below is consulted — `space` is the
        // whole of what it takes from here, and it says the obvious thing, that
        // water goes where the rock is not.
        //
        // See terrain::AquiferSettings for the level itself and for why placing a
        // liquid anywhere but at its own resting height cannot be made to work.
        .spawn =
            {
                .generator = Generator::Pool,
                .space     = SpawnSpace::OpenSpace,
            },

        // Dims light rather than stopping it, so a pool reads as deep by
        // getting darker towards the bottom instead of turning into a wall the
        // moment it is thick enough to swim in.
        .light = {.opacity = 0.10f},
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

// How much a vein's field is lifted for being near the wall of a cave, in the
// units the field itself is measured in.
//
// terrain::Ground carries the answer already, in `solid`: the signed distance
// into the rock in pixels, worked out on the way to the density and kept because
// the density itself cannot answer this. So the term costs no samples at all.
//
// Read from `solid` and not from the density, and that is not interchangeable —
// the density is clamped into [0,1] over kDensitySpan pixels, so it saturates
// about thirteen pixels inside the rock and every seam in the world would think
// it was against a wall.
//
// Smoothstepped rather than linear, so a seam thins away from the wall instead
// of ending on a line parallel to it.
inline float WallLift(const ElementSpawn &spawn, float solid) {
    if (spawn.wallBias <= 0.0f) return 0.0f;

    const float reach = std::max(spawn.wallReach, 1e-3f);
    const float t     = std::clamp(solid / reach, 0.0f, 1.0f);

    return spawn.wallBias * (1.0f - t * t * (3.0f - 2.0f * t));
}

// A cover reaching past the crust would meet the cave layers, and the first thing
// it produced would be soil hanging from the roof of a gallery. The thickest a
// cover ever gets is its nominal depth plus the whole swing of its own noise, so
// that is what has to clear the ceiling.
consteval bool CoversFitUnderTheCrust() {
    for (const ElementDef &def : kElements) {
        if (def.spawn.generator != Generator::Cover) continue;
        if (def.spawn.thickness + def.spawn.thicknessVary > kCoverCeiling) return false;
    }

    return true;
}

static_assert(CoversFitUnderTheCrust(), "a cover may not reach deeper than kCoverCeiling");

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

// The coordinate a band is measured along at a world position: the depth below
// the surface for a relative band, the world Y for an absolute one.
//
// One function rather than a branch at every use, so that a band cannot be read
// one way in one place and the other way somewhere else. `depth` is
// terrain::Ground::depth, which the generator has already worked out.
inline float BandCoord(const ElementSpawn &spawn, Vector2 world, float depth) {
    return spawn.band.relative ? depth : world.y;
}

// Distance into a band at a world position, in pixels: positive inside,
// negative outside, zero exactly on an edge.
//
// Expressed as a distance rather than a yes-or-no test so that the field can
// be faded across the boundary and the contour still has a gradient to
// interpolate through.
inline float BandDepth(const ElementSpawn &spawn, Vector2 world, float depth) {
    const float wobble = BandWobble(spawn, world.x);
    const float along  = BandCoord(spawn, world, depth);

    return std::min(along - (spawn.band.top + wobble), (spawn.band.bottom + wobble) - along);
}

// Share of its peak abundance a material has at a world position, in [0,1]: one
// at the peak of its band, falling away linearly to zero at either edge.
//
// The two arms are independent, so an ore can reach a long way down from its peak
// and only a little way up, which is the shape most of them have. An arm running
// to kUnboundedDepth simply never falls off on that side.
inline float BandAbundance(const ElementSpawn &spawn, Vector2 world, float depth) {
    const DepthBand &band = spawn.band;
    if (!band.Bounded()) return 1.0f;

    const float y    = BandCoord(spawn, world, depth) - BandWobble(spawn, world.x);
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
inline float BandPenalty(const ElementSpawn &spawn, Vector2 world, float depth) {
    return spawn.band.scarcity * (1.0f - BandAbundance(spawn, world, depth));
}

// How thick a cover lies in a column, in pixels, climate and noise included.
//
// Zero where the climate does not suit it at all, which is what makes a material
// belong somewhere rather than everywhere. Both noises are read along the
// horizontal axis alone, because a cover is a property of a column: sampling them
// in two dimensions would make the thickness change on the way down through the
// slab, which is not a thickness at all.
inline float CoverThickness(const ElementSpawn &spawn, float worldX, float temperature, float humidity) {
    const terrain::NoiseShape shape = SpawnNoise(spawn);

    const float jitter =
        (terrain::Sample({worldX + kCoverStride, 0.0f}, shape) - 0.5f) * 2.0f * spawn.climateJitter;

    const float suits = ClimateBell(spawn.climate, temperature, humidity) + jitter;

    // Full depth where the place suits it, nothing where it does not, and a short
    // run between the two. See ElementClimate::fullAt for what the alternatives
    // drew instead.
    const float weight = ClimateRamp(spawn.climate.goneAt, spawn.climate.fullAt, suits);
    if (weight <= 0.0f) return 0.0f;

    const float vary = (terrain::Sample({worldX, 0.0f}, shape) - 0.5f) * 2.0f * spawn.thicknessVary;

    return std::max(weight * (spawn.thickness + vary), 0.0f);
}

inline constexpr const ElementDef &StyleOf(Element element) {
    return Def(element);
}

// The one colour to stand for a material where only one will fit.
//
// The body tone of its ramp, opaque. Wanted by the polygon path, which fills a
// region rather than walking texels and so has nowhere to put a texture, and by
// anything that has to name the material outside the world — a swatch on the
// bar, a marker on an overlay.
inline constexpr Color Body(const ElementDef &def) {
    const Color tone = def.paint.tone[kElementTones / 2];

    return {tone.r, tone.g, tone.b, 255};
}
