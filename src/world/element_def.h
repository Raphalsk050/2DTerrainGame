#pragma once

#include "core/config.h"
#include "core/picture.h"
#include "core/tool.h"
#include "raylib.h"
#include "world/terrain.h"

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
// ---
//
// **This file is the shape of a row and the order of the rows, and nothing else.**
// What each material actually *says* lives in `world/elements/<name>.h`, one file
// each, and `world/element.h` is where they are gathered back into the table.
//
// The split is §19's, arrived at as far as it can go here. What cannot follow the
// items into a self-registering table is the `Element` enum below: `kElementCount`
// sizes a chunk's fields, the editor's ledger, the light's medium, every silhouette
// and a dozen probe arrays, all at compile time, and every one of those is on a hot
// path. So the *order* is still central and adding a material is still two lines
// here — but they are two lines in a forty-line enum instead of a row inserted into
// the middle of nineteen hundred lines that are already correct, which was the whole
// of the complaint.
//
// The other half of why it stops here: a registry hands out ids by sorting on the
// name, and the paint seeds every material's texture off its own index. Moving the
// table would repaint the entire world — see CLAUDE.md §29.

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

// Drawn as blotches through whatever it sits in, rather than as a solid body of
// its own colour.
//
// An ore is not a rock made of ore. It is metal scattered through the stone it
// formed in, and drawing it as a filled region of its own tone gives a compact
// disc of pure colour sitting in the hillside like a sticker — which is what the
// world looked like, and it read as a decal rather than as a find.
//
// **What the blotches are drawn against is deliberately not written down here.**
// Every material is painted as the union of itself and everything that outranks
// it — see World::Occupancy — so by the time an ore's own pass runs, the whole of
// its outline has already been painted by every material beneath it in the
// exclusion order. Leaving a texel alone therefore shows the rock the vein is
// actually in, whatever that rock happens to be. A second kind of stone added
// later at a depth of its own needs nothing said in this struct and nothing said
// in any ore's row: a vein inside it is drawn inside it, because the pass under
// this one has already put it there.
struct ElementVein {
    // Share of the vein's core drawn in the material's own tones.
    //
    // One is a solid body of it, which is what everything the ground is built out
    // of wants, so a row says nothing about this unless it is an inclusion.
    //
    // Read against the percolation threshold of a square lattice, 0.593: below it
    // the blotches are islands and the vein reads as flecks in the rock, above it
    // they join up and it reads as a mass of ore with rock showing through. The
    // ores are written either side of that line by how rich a seam of each one
    // ought to look.
    float share = 1.0f;

    // Side of one blotch, in world pixels.
    //
    // A whole number of texels, or a blotch straddles two of them and comes out a
    // different shape depending on where in the world it fell — §10.4's arithmetic
    // about kPixelSize, one grid further out. Two texels is the smallest mark a
    // pixel artist would make; blotches that land beside each other run together,
    // so this sets the grain of the scatter and not the size of every clump in it.
    float blotch = 2.0f * config::kPixelSize;

    // How far in from the vein's face the share climbs to its full value, as a
    // share of the vein's own half-width.
    //
    // This is what breaks the outline, and without it the whole change is worth
    // very little: blotches that stop along the very curve the solid disc was drawn
    // to leave the eye to reassemble the disc, and a stippled disc is still a disc.
    // Thinning them out towards the edge is what makes a vein end *in* the rock
    // rather than against a line.
    //
    // **A share and not a width in pixels, and that was measured rather than
    // argued.** Written as six pixels it looked right on coal, which is forty-eight
    // across, and swallowed gold, diamond and emerald whole — those are eighteen to
    // twenty-one, so the same six pixels was 88% of the vein, the density never once
    // reached the share the row asked for, and what the sheet showed was three or
    // four lonely texels. A fringe is a proportion of a shape, not a distance: the
    // job it does is to stop the outline reading as a curve, and how much of a shape
    // that takes scales with the shape.
    //
    // soil::For turns it into the pixels the painter wants, off the vein width
    // ElementSpawn::veinCells names — so the two cannot drift apart, and an ore
    // whose seams are made narrower tomorrow gets a narrower fringe with them.
    float fringe = 0.15f;

    // A solid material fills its own outline and is drawn the way everything in
    // the ground has always been drawn. Every test of this is a test for "is this
    // an inclusion", so it is asked here once rather than by comparing with one in
    // four different files.
    constexpr bool Solid() const { return share >= 1.0f; }
};

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

    // Whether this is a body of the material or a scatter of it through something
    // else. See ElementVein: the default is a body, and only an inclusion says
    // otherwise.
    ElementVein vein{};

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

// Minecraft's own two multipliers, and they are the whole of why a bare hand is
// hopeless against stone and fine against dirt.
//
// The game computes damage per tick as `speed / hardness / divisor`, with the
// divisor 30 where the tool can *harvest* the block and 100 where it cannot. At
// twenty ticks to the second that is hardness x 1.5 seconds when it can and
// hardness x 5 when it cannot. Dirt drops with anything, so a bare hand gets 1.5;
// stone drops nothing without a pickaxe, so a bare hand gets 5 — and 1.5 hardness
// times 5 is the seven and a half seconds everyone who has punched stone remembers.
inline constexpr float kHarvests = 1.5f;
inline constexpr float kRefuses  = 5.0f;

// `Tool` and the tier speeds live in `core/tool.h`, which the item table also
// reads. See the head of that file for why they are not written down here.

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

    // Minecraft's own hardness for the block this stands for, unscaled.
    //
    // Written as the wiki writes it rather than as a number of seconds, because the
    // seconds are a *consequence* — of the hardness, of whether the hand can harvest
    // it at all, and of what is being swung. BreakSeconds does that arithmetic and is
    // the only place that should.
    //
    // A cell and not a block-you-can-see, which is the figure to keep in mind when
    // tuning: `config::kBuildCell` is eighteen pixels and the character is four of
    // them tall, where a Minecraft block is half a player. So these run well under
    // Minecraft's own numbers and should — a cell here is a smaller bite of world.
    //
    // The resulting time is charged per *vertex* actually standing there, over
    // kVerticesPerBlock of them to the cell — so a cell buried in a hillside costs
    // the whole figure and one at its ragged edge, holding two vertices of nine,
    // costs two ninths. A contour edge comes away quickly rather than at the price of
    // solid rock, and the time is the time to clear what is really there.
    //
    // It follows that **digging runs at the same vertices per second whatever the
    // brush is set to**. That invariant is deliberate and it is the same one the
    // economy already keeps: a cell costs one block to place and returns one to dig,
    // whatever brush laid it. The span decides the *shape* of a stroke, never its
    // rate.
    //
    // Zero means it comes away the instant it is touched, which is what a liquid
    // does and what anything not really dug should do.
    float hardness = 0.5f;

    // Which tool it gives way to. `Tool::Hand` means none in particular.
    Tool tool = Tool::Hand;

    // What it throws up underfoot, as a share of a full puff of dust.
    //
    // Here rather than in scuff.cpp, where it was a switch over material names. The
    // switch was defensible — how much a ground kicks up is a fact about that ground
    // and nothing else in this row implies it — and it was still the wrong place:
    // adding a material meant remembering to go and edit a file about *footsteps*,
    // and forgetting was silent, because the switch had a default.
    //
    // A field here cannot be forgotten in the same way. It is the row you are
    // already writing, with the question next to the answer: how loose is this stuff
    // to walk on? The default is what the switch's default was, so a row that says
    // nothing behaves exactly as it did.
    float loose = 0.28f;

    // Whether that tool is *required* to get anything out of it.
    //
    // The difference between dirt and stone, and it is the whole of why one is a
    // moment and the other is seven and a half seconds — see kHarvests. Dirt asks for
    // a shovel and gives its dirt up to a fist; stone asks for a pickaxe and gives a
    // fist nothing at all, so the fist is charged the refusing rate.
    bool needsTool = false;

    ElementRules rules;
    ElementSpawn spawn;
    ElementLight light;
};

// Whether a material is drawn from the silhouette it shares with everything that
// outranks it, or from its own field alone.
//
// The union is what stops a gap of open sky opening between two materials whose
// squares fall differently — see ElementPaint::texel, where it is measured — and
// it is load-bearing for anything that fills its own outline. Two kinds of
// material do not want it, for entirely different reasons:
//
//   - Anything that is not part of the ground. The union is stored as the
//     strongest of several fields, and interpolating that is not the same as
//     interpolating each and taking the strongest. Against a hand-placed
//     material, which is one inside its brush and zero outside, the union's
//     crossing lands a fraction of a cell further out than the material's own and
//     a ring of whatever was beneath shows in the gap.
//
//   - A vein. Its silhouette holds every material that outranks it whether or not
//     the vein is anywhere near — that is what a union *is* — so coal drawn from
//     one would scatter coal blotches through every diamond in the chunk, and
//     through every seam of soil above it. Today that is invisible because each
//     later pass paints solidly over the last; the moment a pass stops filling its
//     outline, it is the whole picture.
//
// Nothing is lost by dropping it there, because an inclusion provides no coverage
// in the first place. What fills a vein's outline is the pass underneath it, and
// that pass *is* drawn from a union that includes the vein.
inline constexpr bool DrawnUnioned(const ElementDef &def) {
    return def.rules.blocksBodies && def.paint.vein.Solid();
}

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

