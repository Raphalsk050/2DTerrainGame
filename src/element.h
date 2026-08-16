#pragma once

#include "config.h"
#include "picture.h"
#include "raylib.h"
#include "terrain.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>

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
    WoodPlank,
    Cobblestone,
    WoodWall,
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

    // Sits behind the world rather than in it: drawn before the ground, walked
    // through, and never in contest for a vertex.
    //
    // This is Terraria's wall, and the whole of what makes it a *second layer*
    // rather than a fifteenth material is that it does not occupy. Two occupying
    // materials can never share a vertex — that is what makes "what is here?" a
    // question with one answer — and a wall has to share, because the point of it
    // is to stand behind a block and still be there when the block is dug out.
    //
    // So it keeps its own field, is cut back by nothing, cuts back nothing, and is
    // painted in its own pass. What it costs is that the digging order has to be
    // written down somewhere, since "what is here" now has two answers: the spade
    // takes the block first and the wall only once the block is gone.
    bool background = false;
};

// How the right hand puts a material into the world.
//
// A row on the table rather than a test in the editor, for the reason item.h
// gives about Placement: what "this goes down a cell at a time" means has to be
// one fact in one place, or the hand that lays it, the hand that decides whether
// it may be laid, and the ghost that shows where it would go end up asking
// different questions and answering them differently.
enum class Laying {
    // By the fistful, under a circular brush of the player's own chosen radius.
    // What the ground itself is made of: rock, soil, sand, snow. A landscape is
    // shaped in armfuls and would be absurd to lay out square by square.
    Brush,

    // One build cell per click, snapped to the grid. What is built rather than
    // shaped: planks, cobble, walls.
    //
    // The distinction is not tidiness. A brush spends material by the area it
    // happens to sweep, which is exactly what nobody wants of a wall — a wall is
    // meant to be the same thickness along its whole length, and to end where it
    // was aimed to end.
    Cell,
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
    // This is the *regional* half of that: `shape` is the material's own field,
    // and at the frequency a cover asks for it that means one swell every few
    // hundred pixels. What it says is that this stretch of country has deep soil
    // and that one has thin soil, which is a thing worth knowing and is not what
    // the eye is looking at.
    //
    // Nothing may ask for more than kCoverCeiling — see the constant.
    float thickness     = 0.0f;
    float thicknessVary = 0.0f;

    // The grain of the cover's floor: a second, much finer field, and how many
    // pixels it moves the floor either way.
    //
    // Separate from the pair above because the two are answering different
    // questions and one field cannot answer both. A cover with regional variation
    // alone still meets the rock along a smooth curve — it is a stripe of slowly
    // changing width, and a stripe is what it reads as. What makes a layer look
    // like ground is that its floor is *rough at the scale of the ground it is
    // part of*: it dips into hollows, thins over swells, and does so over tens of
    // pixels rather than hundreds.
    //
    // Measured rather than judged, and the measurement is the point. `--covers`
    // reports how far the floor moves between neighbouring columns. Before this
    // existed the soil moved 0.18 px per lattice column while the ground above it
    // moved 1.1 — the layer was six times flatter than the terrain it was lying
    // on, which is exactly the "painted stripe" complaint stated as a number. The
    // target is the ground's own figure: a floor that wanders like the surface
    // does reads as ground, and one that wanders much more than it reads as
    // static.
    //
    // It is bounded from below by the lattice, in the way every field in this
    // generator is. The world is described one column every six pixels, so a
    // feature finer than about thirty pixels has too few samples across it to be
    // a shape and arrives as noise on the contour.
    //
    // Not the same field as `shape` read somewhere else, and deliberately: the
    // regional swell and the local grain have to be able to disagree, or a
    // stretch of deep soil is also a stretch of rough soil and the two are locked
    // together forever.
    terrain::NoiseShape grain{};
    float grainVary = 0.0f;

    // How high the ground has to stand before the cover lies on it, as the world Y
    // it reaches full depth at and the drop below that over which it thins to
    // nothing. Y grows downward, so the *smaller* number is the higher ground.
    //
    // The snow line, and it had to become a term of its own. Altitude was supposed
    // to need none: terrain::ClimateSettings cools the air by `temperatureLapse`
    // per pixel of elevation, so a material that wanted the tops only had to ask
    // for cold and the peaks would answer. That reasoning is sound and it does not
    // survive contact with a world that also has cold *regions* in it — the
    // temperature field runs the whole range at sea level, so the coldest lowland
    // is as cold as a peak and snow lay in flat country a long way from any
    // mountain. Nothing about a bell can separate the two, because to a bell they
    // are the same reading.
    //
    // A height can. And the two together are the real thing rather than a patch:
    // the bell decides whether this stretch of country has a snow line at all and
    // how far down the lapse carries it, and this decides that it is a line on a
    // mountain. Warm ranges come out bare, cold ones white from the treeline up.
    //
    // Defaults to a height no ground reaches, which reads as no requirement.
    float crest     = kUnboundedDepth;
    float crestFade = 1.0f;

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
//
// A hundred and thirty-two against a crust of a hundred and fifty-two, and the
// twenty between them is a real margin rather than a tight one. `CrustAllowance`
// is a SmoothStep from the crust to the crust plus its fade, so it is *exactly*
// zero above the crust — no system is dug there and no wall roughness is applied
// — and it is still under a hundredth for some way below it. The first depth a
// cave is meaningfully carved at is nearer a hundred and eighty.
//
// It was forty-eight, and raising it bought two different things. The first is
// swing: a cover's mean and its variation come out of one budget, so at
// forty-eight soil at its Minecraft-ish thirty-six had ten pixels left to vary
// within and could not be made to look like anything but a stripe. The second is
// the mean itself, which had to roughly double before the layer read as a
// *stratum* — a band of ground with rock under it — rather than as a line drawn
// along the underside of the grass.
//
// Both were needed and neither would have done on its own. A thin layer that
// varies a lot is a ragged stripe, and a thick layer that does not vary is a
// thicker stripe.
inline constexpr float kCoverCeiling = 132.0f;

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

    // Side of one drawn square, in world units, for this material.
    //
    // Every row uses config::kPixelSize today and **they all have to**, which is
    // worth writing down because the field looks like an invitation to vary it and
    // varying it is a bug.
    //
    // The draw paints each material as the union of itself and everything that
    // outranks it — see World::Occupancy — and that union is what stops a gap
    // opening between two materials whose squares fall differently. So a coarse
    // material paints the *ground of the fine one above it* in its own big squares,
    // which overhang the block by up to a texel; the fine material then covers only
    // its own exact area and the overhang is left standing. What that looks like is
    // a pale rind down the side of every placed block, brightest where the material
    // underneath is snow — and it happens even where there is no snow within a mile,
    // because the union is positive over the block whatever the snow field says.
    //
    // Removing the union instead trades the fringe for a two-pixel gap of open sky
    // round every block, which is worse. Measured both ways.
    //
    // The way to make this varyable is to draw each silhouette at the finest texel
    // of everything it gathers, and that costs the whole chunk. It was worth it
    // while blocks were going to carry authored pixel art at eighteen texels
    // across; it is not worth it for procedural paint.
    float texel = config::kPixelSize;

};

struct ElementDef {
    const char *name;

    // Field value at which the material counts as present, for drawing and for
    // every rule above alike.
    float threshold;

    ElementPaint paint;
    Color contour;

    // The material as it appears held rather than as it appears in the ground:
    // one block of it, in a slot.
    //
    // Only the art is written here. The four tones are the paint tones above,
    // read backwards — see PictureOf. Painting a wall and drawing a block are
    // the same material lit the same way, and a second set of colours here would
    // let the two drift apart with nothing to notice that they had.
    //
    // Most of these are a face: a lit crest, a body, a shaded belly, and a
    // stipple across each boundary. Which is to say the form is the block and
    // the texture is the material, at the two different scales that keeps them
    // legible. What varies between them is the mark that says *which* material
    // at the size of a slot — bedding for rock, ripples for sand and water,
    // facets for the gems, a rolled sheen for the metals.
    const char *icon[kPictureSide];

    // How many blocks of it a slot holds.
    //
    // Sixty-four throughout, as the items are. It is here rather than as one
    // constant because the first material that should not stack like the rest is
    // already in the table: water is carried by the bucket everywhere it has
    // ever been carried, and when it is, this is the row that says so.
    int stack;

    // Brush unless the row says otherwise, so that adding this changed nothing
    // about any material that was already here.
    Laying laying = Laying::Brush;

    // What comes up in the hand when this is dug, where that is not itself.
    //
    // Stone breaks into cobble, which is Minecraft's rule and the reason the
    // player has anything to build with before there is any crafting: what is dug
    // out of a hillside is not the same thing as the hillside. Everything else
    // comes up as what it was.
    //
    // `Count` stands for "itself", because a row of a constexpr table cannot name
    // itself in its own initialiser. Read it through YieldOf and never directly.
    Element yields = Element::Count;

    ElementRules rules;
    ElementSpawn spawn;
    ElementLight light;
};

// The material drawn as a block.
//
// The paint table runs darkest first and a picture runs lit face first, so the
// ramp is read backwards, which also lands the darkest tone on the accent — and
// the accent is where a picture wants its darkest anyway.
inline constexpr Picture PictureOf(const ElementDef &def) {
    return {
        .tone = {def.paint.tone[3], def.paint.tone[2], def.paint.tone[1], def.paint.tone[0]},
        .art  = {def.icon[0], def.icon[1], def.icon[2], def.icon[3], def.icon[4], def.icon[5]},
    };
}

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

        // Bedded, which is the one thing that separates stone from every other
        // grey at this size: the seam across the middle and the darker course
        // under it are the same layering the paint's strata term draws in the
        // wall.
        .icon =
            {
                "aaabaa",
                "abbabb",
                "bbbbbb",
                "bcbbcb",
                "cccccc",
                "cdccdc",
            },
        .stack = 64,

        // Broken stone, not stone. What a pick takes out of a hillside is rubble,
        // and rubble is what there is to build with — which is also the whole of
        // why the world starts the player with nothing and a hillside.
        //
        // It means the untouched rock of the generator is the only rock there is:
        // a player can never put it back, only cobble in its place. That is
        // Minecraft's arrangement and it is worth keeping for the same reason —
        // ground that was never dug looks different from ground that was.
        .yields = Element::Cobblestone,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 0},
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

        // Loose rather than laid down: the tones break across each other in
        // clods instead of in courses, and the darkest marks are scattered as
        // the stones in it are.
        .icon =
            {
                "aabaab",
                "babbba",
                "bbcbbb",
                "bccbcb",
                "ccdccc",
                "cdcddc",
            },
        .stack = 64,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 7},
        .spawn =
            {
                .generator = Generator::Cover,

                // One feature every four hundred pixels or so: the scale at which
                // soil reads as having gathered in places rather than as having
                // been spread with a trowel.
                .shape = {.frequency = 2.6f, .octaves = 2, .seed = 8201},
                .space = SpawnSpace::InsideGround,

                // Around six squares on average, with nearly the whole budget spent
                // on the swing rather than the mean. The old pair was 36 ± 10 and
                // it drew the stripe this whole arrangement exists to get rid of:
                // a standard deviation of 3.2 px on a mean of 36, which is a layer
                // of one thickness with a rounding error on it.
                //
                // Thirty-two with thirty either way is very nearly the same soil on
                // average — an ordinary hole still stays in it and reaching rock is
                // still a decision — but the two noises together carry it from a
                // scrape over bare rock to a pocket seventeen squares deep. That
                // range is the point and the mean is not: what makes ground worth
                // digging is that the next hole is not the last hole.
                //
                // No climate: soil is what the ground is made of wherever nothing
                // else has claimed it, so its bell is the default one and reads
                // as one everywhere.
                .thickness     = 50.0f,
                .thicknessVary = 44.0f,

                // A feature every hundred and eighty pixels, with two octaves under
                // it reaching down to forty-five — about seven lattice columns,
                // which is near the limit of what the world can describe. Soil is
                // the roughest floor of the three: it holds an edge, so it keeps
                // whatever shape it was left in.
                //
                // The frequency came down as the amplitude went up, and the two are
                // not independent. What the eye reads as ground rather than as
                // static is how far the floor moves *between neighbouring columns*,
                // which is amplitude over wavelength — so doubling the swing at a
                // fixed frequency does not make a rougher floor, it makes a noisy
                // one. `--covers` reports that figure and the target is the ground's
                // own 1.1 px per lattice column.
                .grain     = {.frequency = 5.5f, .octaves = 3, .seed = 8202},
                .grainVary = 38.0f,
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
        .contour = {166, 144, 92, 255},

        // The evenest face in the table, and deliberately. Sand is the one
        // material with no structure of its own, so what it gets is a ripple —
        // a single texel offset between one course and the next.
        .icon =
            {
                "aaaaaa",
                "aabaab",
                "bbbbbb",
                "bbcbbc",
                "cccccc",
                "ccdccd",
            },
        .stack = 64,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 8},
        .spawn =
            {
                .generator = Generator::Cover,
                .shape     = {.frequency = 2.2f, .octaves = 2, .seed = 8203},
                .space     = SpawnSpace::InsideGround,

                // Nearly as deep as the soil it displaces, so a desert is sand to
                // dig through and not a dusting over brown ground, and swinging as
                // widely for the same reason soil does — a shade wider, even, since
                // a desert is the one place where what the layer is doing *is* the
                // landscape.
                .thickness     = 48.0f,
                .thicknessVary = 46.0f,

                // Nearly as rough a floor as soil's, and the first attempt at this
                // had it much smoother on the grounds that sand slumps and cannot
                // hold a scarp. That is true of sand and it is an argument about
                // the wrong surface. What comes to rest along a curve is the
                // *top* of a sand bed — and the top of a cover here is the
                // terrain's own surface, which this cannot move. The floor is the
                // rock the sand is lying in, and buried rock is as ragged as rock
                // anywhere: a desert is sand filling the hollows of a bedrock
                // surface, which is the same relationship soil has with it.
                //
                // Measured, and it is why the reasoning got corrected rather than
                // kept: at one feature every two hundred and fifty pixels the
                // desert floor moved 0.48 px per lattice column against the 1.1 the
                // ground above it moves, so the desert still had the flat painted
                // underside this whole change exists to remove. Slightly coarser
                // than soil — a desert's bedrock is the more buried of the two —
                // and otherwise the same treatment.
                .grain     = {.frequency = 4.5f, .octaves = 3, .seed = 8204},
                .grainVary = 36.0f,

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
        .contour = {182, 196, 216, 255},

        // Weighted to the lit face, since snow is the one material whose whole
        // character is that it is bright. The shading is kept to the last two
        // courses; taking it further up turns fresh snow into grey slush, and
        // its four tones are close enough together that there is nowhere to
        // recover the brightness from once it has gone.
        .icon =
            {
                "aaaaaa",
                "aaaaaa",
                "aabaab",
                "bbbbbb",
                "bbcbbc",
                "ccdccd",
            },
        .stack = 64,

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
                //
                // Proportionally the widest swing of the three, and that is what
                // snow actually does: it is the one cover that is *placed by the
                // weather* rather than weathered out of the rock, so it gathers
                // in every hollow and is scoured off every rib. Sixteen either way
                // on a mean of sixteen means a mountainside is drifts and bare
                // patches rather than a coat of paint over the crest — and the
                // patches cost nothing extra, since a column whose snow came out
                // at zero simply shows the ground underneath.
                //
                // It does not follow the other two up into the new headroom, and
                // that is deliberate. A cover this thin is a cap by definition, and
                // the whole of what keeps a snowfield ground rather than a separate
                // world is that the soil beneath it is still there to be dug. A
                // drift may be deep; a snowfield may not be a stratum.
                .thickness     = 16.0f,
                .thicknessVary = 10.0f,

                // The finest grain in the table, matching the field above it. A
                // drift is a small thing — it forms behind whatever broke the wind
                // — so this runs at one feature every seventy pixels, near the
                // limit of what the lattice can hold. Snow is also the one cover
                // where the grain carries more than the regional swell: where it
                // lies at all it lies everywhere, and what varies is how deep it
                // has piled from one dip to the next.
                .grain     = {.frequency = 11.0f, .octaves = 3, .seed = 8206},
                .grainVary = 14.0f,

                // The snow line, and it is what makes snow a mountain and not a
                // latitude.
                //
                // Measured, not reasoned: `--covers` reports the surface's own range
                // over a stretch of world, and with the ranges switched off it runs
                // -14 to 310. So ordinary high ground never reaches y = -14 and a
                // line at -60 clears it with room to spare, while the ranges climb
                // past -360. Full depth from y = -190 up, nothing below y = -60, and
                // the hundred and thirty between is the belt where a mountainside is
                // patchy — which is what a snow line looks like from a distance and
                // is why this is a fade rather than an edge.
                //
                // It replaces nothing: the climate bell below still decides whether
                // a range is cold enough to hold snow at all, and the lapse rate
                // still carries a northern peak past it sooner than a southern one.
                // What this adds is the one thing a bell cannot say, which is
                // "high". Before it, the coldest lowland in the world read exactly
                // like a summit and lay under snow a day's walk from any mountain.
                .crest     = -190.0f,
                .crestFade = 130.0f,

                // Colder than anything else asks for — the pine, the coldest tree
                // in the table, centres on 0.30. Widened now that the crest above
                // does the work of keeping snow off the plains: what this is for is
                // no longer "only the far north" but "not the tropics", so a
                // temperate range wears a cap and only a range standing in a desert
                // comes out bare.
                //
                // Indifferent to how wet it is, since what falls as snow is
                // decided by the cold alone.
                .climate       = {.temperature      = 0.20f,
                                  .humidity         = 0.5f,
                                  .temperatureWidth = 0.40f,
                                  .humidityWidth    = kUnboundedDepth,
                                  .fullAt           = 0.50f,
                                  .goneAt           = 0.30f},
                .climateJitter = 0.14f,
            },
        .light = {.opacity = 1.0f},
    },
    // The two built materials. Neither is anywhere in the world until somebody
    // puts it there — see Generator::None — and both go down a cell at a time
    // rather than under the brush, which is what separates building from shaping.
    //
    // Both sit at the same threshold as everything else in this table, and that
    // is load-bearing twice over.
    //
    // A block has to meet the ground it is laid against without a seam. The
    // contour crosses where the field meets the threshold, so two materials
    // crossing at different values part company along their shared edge — a plank
    // set into a hillside would either sink into the rock or stand a fraction of a
    // cell off it. Matching the table is what makes a built wall continuous with
    // the world it is built into.
    //
    // And it keeps a new material from reaching into the generator. Every occupying
    // material is held under everything that outranks it by a term in the *other's*
    // threshold — see World::ExclusionHeadroom — so a row added at a new threshold
    // changes the headroom over every older one beneath it, and these two outrank
    // the whole table. The clamp does not bite at the depths an ore actually sits
    // at, so nothing was found to move here; matching the table is what means
    // nothing has to be checked again if it ever does.
    //
    // The cost is six tenths of a pixel. At one half the contour would cross at the
    // midpoint between a filled vertex and its empty neighbour and a cell would
    // measure exactly its eighteen; at 0.45 it crosses a little further out and the
    // block is that much proud of its square. Nothing can see it, and it is the
    // cheaper of the two mistakes.
    {
        .name      = "wood plank",
        .threshold = 0.45f,

        // Sawn rather than barked: lighter than the wood item it will be made
        // from, and far more even. What says plank at this size is that the
        // colour barely moves — a sawn face has grain and no patching at all.
        .paint   = {.tone   = {{104, 72, 40, 255}, {138, 98, 56, 255}, {170, 128, 78, 255}, {198, 160, 106, 255}},
                    .grain  = 0.30f,
                    .patch  = 0.08f,

                    // Boards are courses, like stone is, and at a lower contrast:
                    // enough for a wall to read as laid up out of lengths.
                    .strata = 0.85f,

                    // Eighteen texels across a block, which is where authored
                    // pixel art goes. See ElementPaint::texel.
                    },
        .contour = {104, 72, 40, 255},

        // The joint is the whole of it. Two boards with a dark seam between them
        // and the grain running along each — take the seam away and this is a
        // brown square that could be anything.
        .icon =
            {
                "aaaaaa",
                "abbbab",
                "bbbbbb",
                "dddddd",
                "bcbccb",
                "cccccc",
            },
        .stack  = 64,
        .laying = Laying::Cell,

        // Outranks every ore and every cover, so a plank laid into a seam or into
        // a snowbank replaces it rather than being swallowed by it. What it
        // displaces comes back to the player — see World::ApplyBrush, which pays
        // out whatever it cleared.
        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 10},
        .spawn = {.generator = Generator::None},

        // A shade denser than stone. A plank floor is meant to be the thing that
        // makes a room a room, and a roof daylight leaked through would leave
        // nothing for a torch to do.
        .light = {.opacity = 0.85f},
    },
    {
        .name      = "cobblestone",
        .threshold = 0.45f,

        // Rock's own greys, cooled very slightly and spread wider apart. Broken
        // stone catches the light on more faces than bedded stone does, so the
        // range is what says it has been through a pick.
        .paint   = {.tone   = {{74, 74, 80, 255}, {102, 102, 108, 255}, {130, 130, 136, 255}, {162, 162, 168, 255}},

                    .grain  = 0.85f,

                    // High patching and no bedding at all, which is exactly the
                    // opposite of the rock it came out of: cobble is rubble, and
                    // rubble has no layers left.
                    .patch  = 1.10f,
                    .strata = 0.00f},
        .contour = {74, 74, 80, 255},

        // Stones of no particular size with the joints between them, against
        // rock's clean horizontal courses. The two greys have to be told apart in
        // a slot, and this is the only mark that does it.
        .icon =
            {
                "aabaab",
                "badbba",
                "bbbdbb",
                "bdcbcd",
                "ccdccc",
                "cdcccd",
            },
        .stack  = 64,
        .laying = Laying::Cell,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 11},
        .spawn = {.generator = Generator::None},

        // The stone it came from, unchanged. Breaking rock up does not make it
        // let light through.
        .light = {.opacity = 0.8f},
    },
    {
        .name      = "wood wall",
        .threshold = 0.45f,

        // The plank's own tones taken down towards the dark, and that is the whole
        // of how a wall reads as *behind*. Not by being faded — see the cache rule
        // in CLAUDE.md §5.5, which needs every colour the ground is drawn in to be
        // opaque — but by being the colour a plank is when no light reaches it.
        //
        // Far enough down that a wall is never mistaken for the floor in front of
        // it. A player builds a room out of both at once, and the two have to be
        // told apart at a glance in the dark.
        .paint   = {.tone   = {{40, 28, 16, 255}, {56, 39, 22, 255}, {72, 52, 31, 255}, {88, 68, 44, 255}},
                    .grain  = 0.28f,
                    .patch  = 0.08f,
                    .strata = 0.85f},
        .contour = {40, 28, 16, 255},

        // The plank's face, read in the dark. The same joint in the same place, so
        // a wall behind a wall of planks lines up with it.
        .icon =
            {
                "aaaaaa",
                "abbbab",
                "bbbbbb",
                "dddddd",
                "bcbccb",
                "cccccc",
            },
        .stack  = 64,
        .laying = Laying::Cell,

        // Behind everything and in the way of nothing. No precedence, because it
        // never enters the contest — see ElementRules::background.
        .rules = {.background = true},
        .spawn = {.generator = Generator::None},

        // Casts nothing. A wall that dimmed the light would darken the room it is
        // the back of, and the torch standing in front of it would be solving a
        // problem the wall had just invented. Deliberate, and the first thing to
        // revisit if a built room ever wants to be dark on its own.
        .light = {.opacity = 0.0f},
    },
    // The ores below follow Minecraft's set and its ordering, on a scale of one
    // block to sixteen pixels: its sea level at Y 64 is this world's y 144 and
    // its floor at Y -64 is y 2192. That was the scale when a block here was
    // sixteen pixels too, and the bands were left alone when it became eighteen
    // — see kBlockSide. They are absolute heights in pixels and were settled by
    // digging for the ore rather than by the conversion, so re-deriving them
    // against the new figure would move every seam in the world to fix a sum
    // nothing reads. The arithmetic above is history; the depths are the world.
    //
    // What is not carried across is Minecraft's absolute heights, since it has a
    // floor to arrange them against and this world does not, so each peak sits
    // where the ore is actually worth digging for here.
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
        .contour = {26, 26, 32, 255},

        // Lumpy and matt. Coal is the one ore that does not catch the light, so
        // it gets no sheen and no facet: the tones interleave without ever
        // settling into a course, which is what reads as a broken black surface
        // rather than as a polished one.
        .icon =
            {
                "abbbab",
                "bbbcbb",
                "bcbbcb",
                "cbccbc",
                "ccdcdc",
                "cddddc",
            },
        .stack = 64,

        .rules = {.blocksBodies = true, .blocksLiquid = true, .occupies = true, .precedence = 1},
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
        .contour = {132, 66, 40, 255},

        // The three metals are told apart by how each one catches the light,
        // since their tones alone are close enough to read as one another in a
        // slot. Copper takes a diagonal sheen, which is the sharpest of the
        // three and the one that says beaten rather than cast.
        .icon =
            {
                "aaabbb",
                "aabbbc",
                "abbbcc",
                "bbbccd",
                "bbcccd",
                "bcccdd",
            },
        .stack = 64,

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
        .contour = {92, 86, 79, 255},

        // Iron is rolled: courses running flat across, stepping straight down,
        // where copper's sheen runs diagonally. Banding against a diagonal is
        // what separates two greys with a warm cast at six texels.
        //
        // The courses are broken by a texel each rather than laid clean. A run
        // of six identical texels is a painted stripe and reads as one; the
        // stipple has to cross every boundary or the whole face goes flat.
        .icon =
            {
                "aaaaba",
                "abbbbb",
                "bbbcbb",
                "bcccbc",
                "ccccdc",
                "cdcddd",
            },
        .stack = 64,

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
        .contour = {148, 114, 20, 255},

        // Gold is soft, so its sheen is the broadest and the least stepped of
        // the three: the same diagonal as copper with the boundaries stippled
        // open, which is the difference between a struck edge and a polished
        // one.
        .icon =
            {
                "aaabba",
                "aabbbc",
                "abbbcc",
                "bbabcd",
                "bccbcd",
                "cccddd",
            },
        .stack = 64,

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
        .contour = {40, 146, 154, 255},

        // The two gems are cut rather than surfaced: the corners are taken off
        // and the light collects in the middle instead of along the top, which
        // is the only way six texels can say faceted. Diamond is cut across —
        // a wide table with the crown falling away below it.
        .icon =
            {
                "ccaacc",
                "cabbac",
                "abbbba",
                "abccba",
                "bcddcb",
                "cddddc",
            },
        .stack = 64,

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
        .contour = {26, 124, 62, 255},

        // Emerald takes a step cut instead of diamond's brilliant: the corners
        // are off it in the same way, but the light gathers to one side and
        // falls away across the stone rather than sitting symmetrically on a
        // table. Two gems that took the same cut would be one green square and
        // one cyan square, and the point of a picture is that it is not that.
        //
        // Its first cut ran a dark course down the middle of the lit face,
        // meaning to read as a facet edge. At this size an interior mark
        // surrounded by lighter tone has nothing to be an edge *of* and reads as
        // a smudge on the stone. A facet has to be a boundary between two
        // regions, which means it has to reach the outline.
        .icon =
            {
                "cbaabc",
                "baaabb",
                "aaabbb",
                "abbbcc",
                "abbccd",
                "cbccdc",
            },
        .stack = 64,

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

        // Sand's ripple, deepened. Water is the only material here that is
        // drawn as a surface seen from above rather than as a face seen from
        // the side, so the courses are broken twice over instead of offset once.
        .icon =
            {
                "aabaab",
                "bbbbbb",
                "babbba",
                "bbcbbc",
                "cccccc",
                "ccdccd",
            },
        .stack = 64,

        .rules = {.flows = true, .buoyancy = 1.0f},
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

inline constexpr bool ElementIconsAreSquare() {
    for (const ElementDef &def : kElements) {
        if (!IsSquare(PictureOf(def))) return false;
    }

    return true;
}

static_assert(ElementIconsAreSquare(), "every element icon is six rows of six characters");

// Two occupying materials sharing a precedence would overlap, since neither one
// gives way to the other, and the vertex they share would have no single
// answer to what is in it.
// A material laid by the cell has to be drawn on a grid that lands on the cell's
// own edges. Those are at multiples of config::kBuildCell offset by half a lattice
// step, so its texel has to divide both — which leaves 1 and 3.
//
// Checked here rather than left to be noticed, because what getting it wrong looks
// like is a wall with bites out of it, and it took a while to find the first time.
consteval bool BuildTexelsLandOnCells() {
    for (const ElementDef &def : kElements) {
        if (def.laying != Laying::Cell) continue;

        const int texel = static_cast<int>(def.paint.texel);

        if (static_cast<float>(texel) != def.paint.texel || texel <= 0) return false;
        if (config::kBuildCell % texel != 0) return false;
        if ((config::kResolution / 2) % texel != 0) return false;
    }

    return true;
}

static_assert(BuildTexelsLandOnCells(),
              "a material laid by the cell must be drawn on a texel dividing kBuildCell and half a lattice step");

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
// cover ever gets is its nominal depth plus the whole swing of *both* its noises,
// so that is what has to clear the ceiling.
consteval bool CoversFitUnderTheCrust() {
    for (const ElementDef &def : kElements) {
        if (def.spawn.generator != Generator::Cover) continue;
        if (def.spawn.thickness + def.spawn.thicknessVary + def.spawn.grainVary > kCoverCeiling) return false;
    }

    return true;
}

static_assert(CoversFitUnderTheCrust(), "a cover may not reach deeper than kCoverCeiling");

inline constexpr const ElementDef &Def(Element element) {
    return kElements[ElementIndex(element)];
}

// What digging this material puts in the hand.
//
// The one place ElementDef::yields is read, so that "itself" is spelled out once
// rather than at every call site — and so that a row which forgets to say
// anything still answers correctly.
inline constexpr Element YieldOf(Element element) {
    const Element what = Def(element).yields;

    return (what == Element::Count) ? element : what;
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
inline float CoverThickness(const ElementSpawn &spawn, float worldX, float surface, float temperature,
                            float humidity) {
    // The altitude first, because it is the cheap half and it is a gate rather
    // than a weight: a material with a crest is not thinner down in the valley,
    // it is absent, and asking the noise about a column that cannot hold it is
    // three fields spent to arrive at zero.
    //
    // Y grows downward, so high ground is the smaller number and the ramp runs
    // from the crest downward.
    if (spawn.crest < kUnboundedDepth) {
        if (surface >= spawn.crest + std::max(spawn.crestFade, 1e-3f)) return 0.0f;
    }

    const terrain::NoiseShape shape = SpawnNoise(spawn);

    const float jitter =
        (terrain::Sample({worldX + kCoverStride, 0.0f}, shape) - 0.5f) * 2.0f * spawn.climateJitter;

    const float suits = ClimateBell(spawn.climate, temperature, humidity) + jitter;

    // Full depth where the place suits it, nothing where it does not, and a short
    // run between the two. See ElementClimate::fullAt for what the alternatives
    // drew instead.
    float weight = ClimateRamp(spawn.climate.goneAt, spawn.climate.fullAt, suits);
    if (weight <= 0.0f) return 0.0f;

    // And then the rest of the snow line, as a share rather than the gate above:
    // the top of a range is under a full cap and the flank below it wears less and
    // less of one, which is what a snow line looks like from a distance. Ascending
    // and turned over, never ClimateRamp with its edges reversed — that guards its
    // span positive and would clamp the whole world to nothing.
    if (spawn.crest < kUnboundedDepth) {
        weight *= 1.0f - ClimateRamp(spawn.crest, spawn.crest + std::max(spawn.crestFade, 1e-3f), surface);
    }

    if (weight <= 0.0f) return 0.0f;

    // The regional swell and the local grain, summed. Both are read along the
    // horizontal axis alone for the reason given above, and both are signed, so a
    // column where the two agree is the deep pocket or the thin scrape and a
    // column where they disagree is ordinary ground.
    const float vary = (terrain::Sample({worldX, 0.0f}, shape) - 0.5f) * 2.0f * spawn.thicknessVary;

    const float grain =
        (spawn.grainVary > 0.0f) ? (terrain::Sample({worldX, 0.0f}, spawn.grain) - 0.5f) * 2.0f * spawn.grainVary
                                 : 0.0f;

    // Floored at nothing rather than at some minimum, which is what lets the rock
    // reach the surface where the fields agree to take the whole layer away. That
    // outcrop is not a special case anywhere — it is a column whose cover came out
    // at zero, so the grass finds nothing to grow on and a tree finds nothing to
    // root in without either being told about it. `--covers` reports the share of
    // the world it happens over, because it is the one number here that can run
    // away: a few per cent is a landscape with crags in it and twenty is a
    // landscape that has lost its soil.
    return std::max(weight * (spawn.thickness + vary + grain), 0.0f);
}

// Which cover lies on top of the rock in a column, or nothing where the rock is
// bare.
//
// A pure function of the horizontal position and the settings, exactly as the
// covers themselves are: a cover's depth is decided by the climate at its column
// and by its own noise along that column, and where two of them would both lie
// the higher precedence keeps the ground. So anything that has to know what a
// stretch of country is made of — a tree deciding whether it can root there, a
// crown deciding whether it is standing in a snowfield, a shower deciding whether
// it is falling on a desert — can ask without a chunk, a lattice or a world.
//
// It answers about the ground the *generator* lays and not the ground as built,
// which is the right question for placement and the wrong one for anything the
// player has changed. The reason is the one flora::Ground gives for reading the
// skyline rather than the surface: a wood must not rearrange itself around a hole
// somebody dug. A hand putting a sapling down asks the world instead — see
// Editor::rooted_.
inline std::optional<Element> SurfaceCoverAt(float worldX, const terrain::Settings &s) {
    const terrain::Climate climate = terrain::ClimateAt(worldX, s);

    // The land's own surface, which a cover with a crest is measured against. Free
    // here: ClimateAt has already paid for it — the lapse rate needs the elevation.
    const float surface = terrain::Height(worldX, s);

    std::optional<Element> found;
    int rank = -1;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];

        if (def.spawn.generator != Generator::Cover) continue;
        if (def.rules.precedence <= rank) continue;

        if (CoverThickness(def.spawn, worldX, surface, climate.temperature, climate.humidity) <= 0.0f) continue;

        found = static_cast<Element>(e);
        rank  = def.rules.precedence;
    }

    return found;
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
