#pragma once

#include "world/element_def.h"

#include <iterator>
#include <string_view>

#include "world/elements/rock.h"
#include "world/elements/soil.h"
#include "world/elements/sand.h"
#include "world/elements/snow.h"
#include "world/elements/wood_plank.h"
#include "world/elements/cobblestone.h"
#include "world/elements/wood_wall.h"
#include "world/elements/coal.h"
#include "world/elements/copper.h"
#include "world/elements/iron.h"
#include "world/elements/gold.h"
#include "world/elements/diamond.h"
#include "world/elements/emerald.h"
#include "world/elements/water.h"

// The table, gathered from the rows.
//
// One line per material and nothing else on it. What a material *is* lives in its
// own file under `elements/`, and the shape of a row and the order of the table
// live in `element_def.h` — see the head of that file for why the order could not
// follow the items into a table that assembles itself.
//
// The order here has to match `enum class Element` exactly, since the enum is what
// indexes this array — and the assertions under it are what say so. Without them the
// two would part company in silence: every material after a swapped pair would be
// drawn, dug and generated as its neighbour, and the row that looks wrong would be
// the one nobody touched.
inline constexpr ElementDef kElements[] = {
    elements::kRock,
    elements::kSoil,
    elements::kSand,
    elements::kSnow,
    elements::kWoodPlank,
    elements::kCobblestone,
    elements::kWoodWall,
    elements::kCoal,
    elements::kCopper,
    elements::kIron,
    elements::kGold,
    elements::kDiamond,
    elements::kEmerald,
    elements::kWater,
};

// The join between the two lists, checked at compile time.
//
// A third list, and deliberately so: it is not a copy of either, it is the *pairing*
// — which enumerator names which row — and a pairing that is never written down is a
// pairing that cannot be wrong out loud. §16.2b's rule, met where the two halves of
// a table are a hundred lines and one file apart.
//
// The size assert catches the commoner mistake on its own: a row added to one list
// and forgotten in the other.
static_assert(std::size(kElements) == kElementCount, "every Element needs exactly one row, and no row needs two");

static_assert(std::string_view(kElements[ElementIndex(Element::Rock)].name) == "rock",
              "kElements is out of step with enum class Element at rock");
static_assert(std::string_view(kElements[ElementIndex(Element::Soil)].name) == "soil",
              "kElements is out of step with enum class Element at soil");
static_assert(std::string_view(kElements[ElementIndex(Element::Sand)].name) == "sand",
              "kElements is out of step with enum class Element at sand");
static_assert(std::string_view(kElements[ElementIndex(Element::Snow)].name) == "snow",
              "kElements is out of step with enum class Element at snow");
static_assert(std::string_view(kElements[ElementIndex(Element::WoodPlank)].name) == "wood plank",
              "kElements is out of step with enum class Element at wood plank");
static_assert(std::string_view(kElements[ElementIndex(Element::Cobblestone)].name) == "cobblestone",
              "kElements is out of step with enum class Element at cobblestone");
static_assert(std::string_view(kElements[ElementIndex(Element::WoodWall)].name) == "wood wall",
              "kElements is out of step with enum class Element at wood wall");
static_assert(std::string_view(kElements[ElementIndex(Element::Coal)].name) == "coal",
              "kElements is out of step with enum class Element at coal");
static_assert(std::string_view(kElements[ElementIndex(Element::Copper)].name) == "copper",
              "kElements is out of step with enum class Element at copper");
static_assert(std::string_view(kElements[ElementIndex(Element::Iron)].name) == "iron",
              "kElements is out of step with enum class Element at iron");
static_assert(std::string_view(kElements[ElementIndex(Element::Gold)].name) == "gold",
              "kElements is out of step with enum class Element at gold");
static_assert(std::string_view(kElements[ElementIndex(Element::Diamond)].name) == "diamond",
              "kElements is out of step with enum class Element at diamond");
static_assert(std::string_view(kElements[ElementIndex(Element::Emerald)].name) == "emerald",
              "kElements is out of step with enum class Element at emerald");
static_assert(std::string_view(kElements[ElementIndex(Element::Water)].name) == "water",
              "kElements is out of step with enum class Element at water");

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

// Seconds to break one full cell of `element` with `with` in the hand.
//
// Minecraft's arithmetic, kept as arithmetic: the hardness is the material's, the
// rate depends on whether what is held can harvest it, and the tool's speed divides
// what is left. The one place any of that is decided, so tools arriving means a
// ToolSpeed with more than one row in it and nothing else.
inline constexpr float BreakSeconds(Element element, const tool::Kit &with) {
    const ElementDef &def = Def(element);

    // Whether what is held is the tool this material asks for. Two things hang off
    // it and they are separate questions that happen to share an answer:
    //
    //   - **The rate.** Minecraft gives the tier multiplier only where the tool is
    //     the *right* one, so a pickaxe through dirt is a bare hand through dirt.
    //     Written the other way round — every tool fast at everything — one pickaxe
    //     would end the need for any other tool, which is the whole of what a tool
    //     table is for.
    //
    //   - **The divisor.** A material that `needsTool` is charged the refusing rate
    //     until the right one is held, which is where the seven and a half seconds
    //     of punching stone comes from.
    const bool right = def.tool != Tool::Hand && with.kind == def.tool;

    const bool harvests = !def.needsTool || right;

    return def.hardness * (harvests ? kHarvests : kRefuses) / (right ? with.speed : tool::kHand);
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
