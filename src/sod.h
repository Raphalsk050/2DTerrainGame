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
struct Cover {
    const char *name;

    ElementClimate climate;

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
        .tone =
            {
                {{32, 74, 48, 255}, {50, 106, 64, 255}, {78, 142, 84, 255}, {118, 180, 116, 255}},
                {{28, 68, 46, 255}, {46, 98, 62, 255}, {72, 132, 80, 255}, {110, 170, 110, 255}},
                {{58, 72, 44, 255}, {86, 102, 58, 255}, {120, 136, 76, 255}, {158, 172, 104, 255}},
                {{56, 66, 60, 255}, {80, 92, 84, 255}, {106, 118, 108, 255}, {136, 148, 136, 255}},
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

// The greens for one column: the climate's cover, the time of year, and how damp
// the ground is right now.
//
// `blend` is how far the year has turned towards the next season and `wet` is
// weather::Sky::HumidityAt — the place's own climate raised by rain that has
// fallen and lowered by the sun since, which is what makes a field visibly
// greener after a shower.
soil::Ramp RampAt(const terrain::Climate &climate, flora::Season season, float blend, float wet);

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
// deliberately mirrors so that a wood and the grass under it lean together.
//
// The two numbers that differ are the two that should: a blade of grass bends
// most of its own length where a trunk bends a twentieth, and being lighter it
// swings about twice as fast.
inline constexpr float kBladeReach   = 0.40f;
inline constexpr float kBladePeriod  = 1.5f;
inline constexpr float kBladeUrgency = 0.45f;

// The grass under the view, as everything standing on it needs to be handed it.
//
// A view rather than a copy, and rather than a way back into the world: the
// caller has already worked all this out for the band it draws, and what this
// carries is the answers. The same arrangement flora::Ground and weather::Ground
// have, for the same reason.
struct Blades {
    const float *top    = nullptr; // World Y of the top of the ground.
    const float *cover  = nullptr; // How established the grass is, in [0,1].
    const float *push   = nullptr; // Wind as a share of the strongest gust, in [-1,1].
    const soil::Ramp *ramp = nullptr;

    int count       = 0;
    int firstColumn = 0;
    float spacing   = 1.0f;

    // How much of each tuft has grown back since it was cut, in [0,1], on the
    // tuft's own grid rather than on the lattice.
    //
    // Its own array because a tuft is nine pixels wide and a column is six: what
    // has happened to one tuft is not a property of any column, and rounding it
    // onto the lattice would have a cut take its neighbour's grass with it.
    const float *standing   = nullptr;
    int cells               = 0;
    std::int64_t firstCell  = 0;

    bool Empty() const { return ramp == nullptr || count <= 0; }
};

// Weather minutes a cut tuft takes to stand again.
//
// On the same clock the plants grow and the weather runs on, so F7 shows a mown
// field coming back in seconds rather than leaving it to be taken on trust.
inline constexpr float kMowMinutes = 2.5f;

// How much of a tuft is left the moment it is cut.
//
// Not zero, and that is deliberate: grass cut to the ground is bare earth, and
// what a blow with a hand tool actually leaves is stubble. It also means a cut
// reads as a change to the grass rather than as the grass disappearing.
inline constexpr float kStubble = 0.18f;

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
int Cut(const Blades &ground, Rectangle hitbox, int seed, std::int64_t *into, int room);

} // namespace sod
