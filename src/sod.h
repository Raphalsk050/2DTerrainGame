#pragma once

#include "element.h"
#include "flora.h"
#include "raylib.h"
#include "soil.h"
#include "terrain.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

// The grass over the soil.
//
// Not a material, and that is the decision the rest of this rests on. Grass has
// to come back on its own where the player has dug, and a material that grew back
// would have to be written into the field — which means edits the player never
// made, in a record that never gives anything back. Derived from the soil's own
// field instead, it costs nothing to keep, it vanishes the instant the soil under
// it does, and it returns when a number goes back up.
//
// What that buys as a side effect is the right answer to what is under the
// player's shovel: the world says soil where the eye sees grass, so breaking the
// top of the ground gives dirt. Which is what Minecraft does, and for the same
// underlying reason.
namespace sod {

// A kind of ground cover, as a place in the two climate fields rather than as a
// biome name.
//
// The same bell every other climate-placed thing in this project uses, so a
// meadow, a pine and a desert are all selected by one kind of rule and can be
// reasoned about together.
// How thickly a stretch of ground is dressed, and with what.
//
// The half of a ground cover that is not its colour, and it had to become its own
// thing the moment there was a desert: a meadow and a desert are not two shades of
// the same lawn. What separates them is how much of the ground holds anything at
// all, and what the ground holds where it is not a plant.
//
// Both are read against the *cover* — how established the grass in a column is,
// which is what digging and regrowth move — and never folded into it. They are two
// different facts: a desert that is thinly vegetated is not a desert whose grass
// was dug up last minute and is coming back, and folding them would have a
// player's spade make a meadow out of a dune.
struct Dressing {
    // Share of the cells along the ground that hold a tuft.
    float tufts = 1.0f;

    // And of the ones that do not, the share that hold a stone instead.
    //
    // Stones rather than nothing, because bare ground with nothing on it reads as
    // ground that has not been finished. What a desert floor actually has between
    // its bushes is grit and pebbles, and one texel cluster per cell is the whole
    // of what it takes to say so.
    float stones = 0.0f;
};

struct Cover {
    const char *name;

    ElementClimate climate;

    Dressing dress;

    // Four tones per season, darkest first, expanded into the seven the grass is
    // drawn from — the same arrangement flora::SpeciesPalette uses for leaves,
    // and deliberately in the same greens, because grass and foliage under the
    // same sky are the same family of colour.
    Color tone[flora::kSeasonCount][kElementTones];
};

inline constexpr Cover kCovers[] = {
    {
        .name = "meadow",

        // Temperate and damp, and the widest of the three: this is the ground
        // cover of everywhere that is not remarkable, which is most places.
        .climate = {.temperature      = 0.58f,
                    .humidity         = 0.62f,
                    .temperatureWidth = 0.30f,
                    .humidityWidth    = 0.34f,
                    .fullAt           = 0.40f,
                    .goneAt           = 0.05f},

        // Every cell, and no stones. A meadow is closed ground: what is between
        // one tuft and the next is another tuft.
        .dress = {.tufts = 1.0f, .stones = 0.0f},
        .tone =
            {
                // Spring is the year's brightest and yellowest green, and it is
                // the one worth authoring first: new growth is what most people
                // picture when they picture grass.
                {{38, 92, 38, 255}, {62, 128, 46, 255}, {104, 176, 62, 255}, {158, 214, 96, 255}},

                // Summer darkens and deepens. The same hues held a shade longer.
                {{30, 84, 36, 255}, {52, 118, 44, 255}, {88, 158, 58, 255}, {136, 196, 86, 255}},

                // Autumn turns the green out of it rather than turning it orange:
                // grass goes to straw through yellow, and a lawn the colour of an
                // oak in October would read as fallen leaves, not as grass.
                {{84, 86, 34, 255}, {124, 120, 44, 255}, {170, 158, 60, 255}, {206, 192, 96, 255}},

                // Winter is dead growth over wet ground: grey through it, and
                // little left of the yellow.
                {{70, 72, 56, 255}, {98, 98, 74, 255}, {128, 126, 98, 255}, {158, 154, 124, 255}},
            },
    },
    {
        .name = "steppe",

        // Hot and dry, but well short of the desert — the sand's own bell centres
        // at 0.88 temperature and 0.14 humidity, so this is the belt between the
        // meadow and the bare ground, and it has to be a belt rather than a line
        // or a desert arrives with no approach to it.
        .climate = {.temperature      = 0.76f,
                    .humidity         = 0.28f,
                    .temperatureWidth = 0.24f,
                    .humidityWidth    = 0.26f,
                    .fullAt           = 0.40f,
                    .goneAt           = 0.05f},

        // Opening up, with the ground beginning to show through. The belt between
        // a meadow and a desert has to look like one or the desert arrives with no
        // approach to it, and that is as true of how thick the grass is as it is
        // of what colour it is.
        .dress = {.tufts = 0.72f, .stones = 0.10f},
        .tone =
            {
                {{84, 92, 40, 255}, {120, 128, 54, 255}, {162, 166, 76, 255}, {200, 198, 112, 255}},
                {{96, 90, 38, 255}, {134, 124, 50, 255}, {176, 162, 70, 255}, {212, 198, 108, 255}},
                {{104, 86, 36, 255}, {144, 120, 48, 255}, {186, 158, 66, 255}, {218, 196, 106, 255}},
                {{88, 80, 52, 255}, {118, 108, 74, 255}, {148, 138, 100, 255}, {178, 168, 130, 255}},
            },
    },
    {
        .name = "taiga",

        // Cold and wet. Centred where the pine is, so the floor of a pinewood is
        // the cover that belongs under a pinewood without either being told about
        // the other.
        .climate = {.temperature      = 0.28f,
                    .humidity         = 0.58f,
                    .temperatureWidth = 0.26f,
                    .humidityWidth    = 0.34f,
                    .fullAt           = 0.40f,
                    .goneAt           = 0.05f},

        // Nearly closed, with the odd stone showing. A pinewood floor is moss and
        // needle litter over rock that is never far down.
        .dress = {.tufts = 0.90f, .stones = 0.08f},
        .tone =
            {
                {{32, 74, 48, 255}, {50, 106, 64, 255}, {78, 142, 84, 255}, {118, 180, 116, 255}},
                {{28, 68, 46, 255}, {46, 98, 62, 255}, {72, 132, 80, 255}, {110, 170, 110, 255}},
                {{58, 72, 44, 255}, {86, 102, 58, 255}, {120, 136, 76, 255}, {158, 172, 104, 255}},
                {{56, 66, 60, 255}, {80, 92, 84, 255}, {106, 118, 108, 255}, {136, 148, 136, 255}},
            },
    },
    {
        .name = "desert",

        // Where the sand is. The same pair of numbers the sand's own row in the
        // element table asks for, near enough — four things describe this one
        // desert now, and they are meant to describe the same desert: the sand
        // under it, the scrub standing in it, the rain that does not fall on it,
        // and this.
        .climate = {.temperature      = 0.88f,
                    .humidity         = 0.16f,
                    .temperatureWidth = 0.26f,
                    .humidityWidth    = 0.28f,
                    .fullAt           = 0.38f,
                    .goneAt           = 0.05f},

        // A quarter of the ground holding anything, and a good share of the rest
        // holding a stone. This is the row the whole Dressing idea exists for: a
        // desert drawn as a meadow in different greens is a desert nobody believes,
        // and no palette can fix that — what says desert is the *ground showing*.
        .dress = {.tufts = 0.26f, .stones = 0.42f},
        .tone =
            {
                // Dead straw and bleached stalk, and almost no turn to the year:
                // there is no spring in a desert to be green in. The four rows are
                // very slightly apart so that the blend between seasons has
                // somewhere to go rather than being a hard repeat.
                {{112, 96, 56, 255}, {148, 130, 78, 255}, {184, 166, 106, 255}, {214, 200, 146, 255}},
                {{116, 98, 54, 255}, {152, 132, 74, 255}, {190, 170, 102, 255}, {220, 206, 148, 255}},
                {{110, 92, 52, 255}, {146, 126, 72, 255}, {182, 162, 100, 255}, {212, 198, 142, 255}},
                {{104, 90, 58, 255}, {138, 122, 80, 255}, {172, 156, 108, 255}, {202, 190, 148, 255}},
            },
    },
};

inline constexpr std::size_t kCoverCount = std::size(kCovers);

// How deep the green lies on the soil, in world pixels.
//
// A little over two terrain texels, which is as thin as the band can be and still
// carry a lit crest, a body and the shaded edge where it meets the earth. One
// texel would be a line drawn along the ground rather than a layer of it.
inline constexpr float kSodDepth = 11.0f;

// How far the grass leans towards dead straw when the ground is parched.
//
// Applied to whichever cover the climate chose, rather than by giving each of
// them a fifth palette: what a drought does to a meadow and what it does to a
// steppe is the same thing, and the steppe's own summer already *is* the colour
// grass goes when it has no water.
inline constexpr float kParch = 0.38f;

// How the grass carries its own texture, in tone steps. See ElementPaint for
// what the numbers mean and why the grain has to stay under a step.
//
// No bedding: layers are what stone is made of, and there is nothing laid down in
// a lawn. What structure grass has runs the other way — upright, and at the scale
// of a blade rather than of the ground — and that is drawn as blades rather than
// asked of the texture.
inline constexpr float kGrassGrain  = 0.50f;
inline constexpr float kGrassPatch  = 1.10f;
inline constexpr float kGrassStrata = 0.0f;

// How far a face has to turn from the sky before it stops holding grass.
//
// Read against the depth of the band: the sod is kSodDepth deep, so in the units
// the field is kept in it reaches kSodDepth / terrain::kDensitySpan, and this has
// to be above that or a level lawn would be thinned by its own flatness. What it
// does bind on is the turn: at forty-five degrees it starts to cut, and by
// vertical it has cut everything. Which is the rule a grass block follows — the
// top of the ground is green and the side of it is earth.
inline constexpr float kFacingReach = 0.60f;

// Everything one column's ground cover looks like: its colours, and how much of
// it there is.
//
// One struct and one call, because both halves are a mix over the same covers
// weighted the same way, and reading them separately would walk that mix twice
// for every column of every frame — and, worse, would let the two drift, so a
// stretch of country could be coloured as a desert and planted as a meadow.
struct Look {
    soil::Ramp ramp;
    Dressing dress;
};

// The look of one column: the climate's cover, the time of year, and how damp the
// ground is right now.
//
// `blend` is how far the year has turned towards the next season and `wet` is
// weather::Sky::HumidityAt — the place's own climate raised by rain that has
// fallen and lowered by the sun since, which is what makes a field visibly
// greener after a shower.
Look LookAt(const terrain::Climate &climate, flora::Season season, float blend, float wet);

// Blades in one tuft.
inline constexpr int kBladesPerTuft = 5;

// Horizontal spacing of the tufts, in world pixels.
//
// Exactly one plant texel per blade, and that is a requirement rather than a
// round number. At nine pixels the five blades were 1.8 apart, so snapping them
// to the two-pixel grid landed two of them in the same column and left another
// empty — a tuft drawn as five blades came out as three, which is most of why the
// first field of grass read as a spray of separate spikes instead of as ground
// cover.
inline constexpr float kTuftSpan = kBladesPerTuft * 2.0f;

// How tall a blade gets, in plant texels.
//
// Eight texels is sixteen world pixels — the height of a Minecraft block, and
// about two thirds of the player. Tall enough to sway visibly and read as grass,
// short enough that a field of it is not a field of ferns: the fern is 26 tall
// and has to stay the larger plant.
inline constexpr int kBladeTall = 8;

// How much shorter a tuft's outermost blade is than its middle one, as a share.
//
// A tuft is a small mound and not a row of railings. Without the taper every
// blade in a clump ends at its own height and the clump has no shape of its own;
// with it the clump has a crest, which is the form at the scale above the blade —
// the same job the dome does on a mass of foliage.
inline constexpr float kTuftTaper = 0.45f;

// How far a blade arcs on a still day, as a share of its own height.
//
// Small on purpose. Grass is not a set of vertical lines, so this has to exist —
// but the first setting was twice this, and what it did was carry the tallest
// blades two texels clear of the clump they grew in, which pulled every tuft
// apart into separate strokes.
inline constexpr float kBladeCurve = 0.15f;

// The sway, as the trees describe it — see the constants in grove.cpp, which this
// deliberately mirrors so that a wood and the grass under it lean together. The
// split into a hold and a quiver is argued for there and holds here for the same
// reason: what lays grass over is the pressure of the air on it, what ripples it in
// a light breeze is the turbulence in that air, and only one of the two goes to
// nothing when the wind does.
//
// The numbers that differ are the ones that should: a blade of grass bends most of
// its own length where a trunk bends a twentieth, being lighter it swings about
// twice as fast, and more of its movement is the ripple — a meadow is never still
// in the way a wood can be.
inline constexpr float kBladeHold    = 0.45f;
inline constexpr float kBladeSwing   = 0.20f;
inline constexpr float kBladeIdle    = 0.55f;
inline constexpr float kBladePeriod  = 1.5f;
inline constexpr float kBladeHurry   = 0.75f;
inline constexpr float kBladeUrgency = 1.30f;

// The grass under the view, as everything standing on it needs to be handed it.
//
// A view rather than a copy, and rather than a way back into the world: the
// caller has already worked all this out for the band it draws, and what this
// carries is the answers. The same arrangement flora::Ground and weather::Ground
// have, for the same reason.
struct Blades {
    const float *top   = nullptr; // World Y of the top of the ground.
    const float *cover = nullptr; // How established the grass is, in [0,1].
    const float *push  = nullptr; // Wind as a share of the hardest this world blows, in [-1,1].
    const Look *look   = nullptr; // What this stretch of country is dressed in.

    int count       = 0;
    int firstColumn = 0;
    float spacing   = 1.0f;

    // What is left of each tuft, in [0,1], on the tuft's own grid rather than on
    // the lattice. One where nobody has touched it and zero where it has been
    // cut, and nothing in between: cut grass is gone.
    //
    // Its own array because a tuft is ten pixels wide and a column is six: what
    // has happened to one tuft is not a property of any column, and rounding it
    // onto the lattice would have a cut take its neighbour's grass with it.
    //
    // Kept as a share rather than as a flag because what it is is a height, and
    // whatever puts grass back — a bonemeal in the hand, most likely — will want
    // to bring one up rather than switch it on.
    const float *standing   = nullptr;
    int cells               = 0;
    std::int64_t firstCell  = 0;

    bool Empty() const { return look == nullptr || count <= 0; }
};

// The stone that sits where a tuft does not, in plant texels.
//
// Three by two at the largest and one texel at the smallest, drawn from the
// ground's own ramp rather than a colour of its own — a pebble is a piece of the
// ground it is lying on, and one authored grey would be the same grey in a desert
// and on a taiga floor.
//
// Deliberately tiny. What this is for is breaking up bare ground, not putting
// scenery on it: at four texels across it stops reading as grit and starts
// reading as a boulder somebody forgot to make collidable.
inline constexpr int kStoneWide = 3;
inline constexpr int kStoneTall = 2;

// How far a stone's tone sits from the middle of the ramp, in tone steps. Below
// it, so a pebble is the dark mark on pale ground that grit actually is.
inline constexpr float kStoneShade = 2.2f;

// How established the grass in a column has to be before a tuft standing in it
// is worth anything.
//
// All of it. Grass creeping back onto turned earth is grass that has not finished
// growing, and cutting it takes it away without paying for it — the same rule
// Minecraft applies to a crop, and the reason a player waits rather than
// harvesting the moment a blade shows.
//
// Just under one rather than one exactly, so a column that has finished
// establishing counts on the frame it finishes and not on whichever later frame
// the arithmetic lands on the number itself.
inline constexpr float kRipe = 0.995f;

// Weather minutes it takes grass to establish on earth that has just been turned
// over, once it has something to spread from.
//
// Turned over covers both hands: soil the player laid down, and soil a dig
// uncovered. They are the same event to the ground — a face of bare earth that
// had no grass on it a moment ago — and a rule that treated them differently
// would have a shovel make lawn out of a hillside while a trowel could not.
//
// On the clock the mowing already runs on, and a little under it: coming back
// after a cut is regrowth from living roots, and this is a colony arriving
// somewhere it was not.
inline constexpr float kTakeMinutes = 1.5f;

// How fast the edge of a field advances into it, in world pixels per weather
// minute.
//
// Six blocks a minute. What this decides is the shape of the thing rather than
// its speed: a column is turfed a delay after the column beside it, so the green
// crosses new ground as a front and a wide platform is still bare in the middle
// long after its edges have taken. Set it high enough and the whole platform
// greens at once, which is the fault this was written to fix wearing a slower
// coat.
//
// At this rate a hole five blocks across closes from both sides in forty seconds
// and a platform thirty blocks wide takes two and a half minutes to meet in the
// middle, which is the range a player actually builds in.
inline constexpr float kCreepPerMinute = 96.0f;

// How far the front will travel at all, in world pixels.
//
// Grass spreads from grass, so ground further than this from any is ground with
// nothing to spread from. Rather than leave it bare for ever — which is what
// Minecraft does, and which here would mean a platform built out over a canyon
// that never greened and never said why — the front simply stops advancing and
// the delay saturates, so the far middle of a very large build is the last thing
// to turn and turns at a known time. That bound is also what lets a record be
// dropped: past kTakeMinutes plus this distance's worth of creep, no column can
// still be waiting.
//
// Bounded from above by something other than taste: the distance is measured
// across the band ReadSod works out, so it can only find grass that band reaches.
// Keeping it inside the margin that band is widened by means every column on
// screen can see every source within reach of it, and the answer therefore does
// not change as the view scrolls over it. The static assertion in ReadSod is what
// holds the two together.
inline constexpr float kCreepReach = 240.0f;

// The longest any turned column can be waiting, in weather seconds.
inline constexpr float kSettleSeconds = (kTakeMinutes + kCreepReach / kCreepPerMinute) * 60.0f;

// The tufts standing on the band, drawn texel by texel on the plant grid.
//
// Drawn rather than baked. The sheet the trees are rasterised into has a budget
// of a couple of dozen plants a frame and hands back nothing when it is spent —
// which is right for a tree, where the alternative is the wrong tree, and wrong
// for grass, where it would mean the field filling itself in every time the view
// moved.
//
// `now` is the weather clock, so the sway runs with the wind that drives it and
// both speed up together under F7.
void DrawTufts(const Blades &ground, Rectangle view, float now, int seed);

// Whatever is cut by a blow landing in `hitbox`, as the cells it took.
//
// Reported rather than acted on: what a cut tuft is worth belongs to the caller,
// exactly as World::Excavate reports a yield rather than deciding what it buys.
//
// `ripe` is filled alongside `into` and says, of each cell taken, whether the
// grass there was fully established — see kRipe. Both are reported because they
// are two different facts and the caller needs both: every cell taken is a cell
// that has been cleared, and only the ripe ones are cells that pay.
int Cut(const Blades &ground, Rectangle hitbox, int seed, std::int64_t *into, bool *ripe, int room);

} // namespace sod
