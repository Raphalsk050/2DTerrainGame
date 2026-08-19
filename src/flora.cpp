#include "flora.h"

#include "element.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace flora {
namespace {

// Deterministic value in [0,1) from a cell index and a salt.
//
// The same idea as weather::Hash and for the same reason — a wood has thousands
// of trees and nothing about one is worth remembering between frames, so each is
// hashed out of its own index instead of being stored. Widened to sixty-four
// bits because a cell index is one: the world is unbounded, and a mix that threw
// away the high half would repeat the same wood every few million pixels.
float Roll(std::int64_t cell, int salt, int seed) {
    auto bits = static_cast<std::uint64_t>(cell) * 0x9E3779B97F4A7C15ull;

    bits ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(salt)) * 0xBF58476D1CE4E5B9ull;
    bits ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)) * 0x94D049BB133111EBull;

    bits ^= bits >> 30;
    bits *= 0xBF58476D1CE4E5B9ull;
    bits ^= bits >> 27;
    bits *= 0x94D049BB133111EBull;
    bits ^= bits >> 31;

    return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24);
}

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

terrain::NoiseShape Reseed(terrain::NoiseShape shape, int seed) {
    shape.seed += seed;
    return shape;
}

// Floor division, so a cell index stays correct left of the origin where
// truncation would round the wrong way and put two worlds' worth of trees in the
// cell straddling zero.
std::int64_t FloorDiv(float value, float span) {
    return static_cast<std::int64_t>(std::floor(value / std::max(span, 1e-3f)));
}

// How well a value sits inside a range, in [0,1]: one at the centre, thinning to
// nothing about a width away.
float Bell(float value, float centre, float width) {
    const float t = (value - centre) / std::max(width, 1e-3f);
    return std::exp(-t * t);
}

float Suitability(const SpeciesDef &def, const terrain::Climate &climate) {
    return Bell(climate.temperature, def.climate.temperature, def.climate.temperatureWidth) *
           Bell(climate.humidity, def.climate.humidity, def.climate.humidityWidth);
}

// Value the forest field exceeds over exactly `coverage` of the world.
//
// Measured rather than declared, for the reason terrain::Quantile is: a cutoff
// written by hand is a statement about the shape of the field, so every change
// to its frequency or octaves would quietly change how much of the world is
// wooded. A quantile is a statement about the share, which is what the setting
// is trying to say.
//
// Sampled along one axis alone, because the field is. Walking a square of the
// world would spend most of its samples re-reading the same column.
float Quantile(const terrain::NoiseShape &shape, float coverage, float span) {
    constexpr int kSamples = 8192;

    const float share = std::clamp(coverage, 0.0f, 1.0f);

    if (share <= 0.0f) return 1.0f;
    if (share >= 1.0f) return 0.0f;

    std::vector<float> values;
    values.reserve(kSamples);

    for (int i = 0; i < kSamples; i++) {
        const float x = (static_cast<float>(i) / kSamples - 0.5f) * span;
        values.push_back(terrain::Sample({x, 0.0f}, shape));
    }

    const auto index = static_cast<std::size_t>((1.0f - share) * (values.size() - 1));

    std::nth_element(values.begin(), values.begin() + index, values.end());

    return values[index];
}

// Widest a plant of a layer ever gets, so the caller can be told how far one may
// reach out of the cell it grew in.
float WidestCanopy(Layer layer) {
    float widest = 0.0f;

    for (const SpeciesDef &def : kSpecies) {
        if (def.layer != layer) continue;
        for (std::size_t stage = 0; stage < kStageCount; stage++) widest = std::max(widest, def.canopyWidth[stage]);
    }

    return widest;
}

// Largest a plant's own variation makes it, so a margin sized from the table
// still covers the ones that rolled big. Kept beside the two places the roll is
// made, since it is the ceiling of exactly that expression.
inline constexpr float kLargestScale = 1.14f;

// Share of a mass's radius that is actually drawn.
//
// The rasteriser lays a mass down as 1 − d² and cuts it at a half, so foliage
// stops at d = 1/√2 and a mass is visibly seventy-one per cent of the radius it
// is given. Anything sized against a radius has to allow for it or it is out by
// a factor of 1.41 — which is how every canopy came out narrower than the table
// said, by an amount that grew with the masses.
inline constexpr float kVisibleRadius = 0.7071f;

// Horizontal position of the trunk at a height, for hanging the crown off.
float TrunkAt(const Skeleton &skeleton, float y) {
    if (y <= skeleton.trunk[0].y) return skeleton.trunk[0].x;

    for (int n = 1; n < kTrunkNodes; n++) {
        if (y > skeleton.trunk[n].y) continue;

        const float span = std::max(skeleton.trunk[n].y - skeleton.trunk[n - 1].y, 1e-3f);
        const float t    = (y - skeleton.trunk[n - 1].y) / span;

        return skeleton.trunk[n - 1].x + (skeleton.trunk[n].x - skeleton.trunk[n - 1].x) * t;
    }

    return skeleton.trunk[kTrunkNodes - 1].x;
}

} // namespace

void Calibrate(Settings &settings) {
    // Wide enough that the measurement covers many features of the lowest
    // frequency field in play, rather than the handful a screen holds.
    constexpr float kSampledSpan = 400000.0f;

    for (std::size_t layer = 0; layer < kLayerCount; layer++) {
        settings.calibration.forest[layer] =
            Quantile(Reseed(settings.forest, settings.seed), settings.layer[layer].coverage, kSampledSpan);
    }
}

float GroundAt(const Ground &ground, float worldX) {
    if (ground.top == nullptr || ground.count <= 0) return 0.0f;

    // Nearest column rather than interpolated between two, the same way rain
    // finds what it lands on: the surface is a staircase of squares and not a
    // slope, so a trunk based on the interpolation between two columns would
    // stand buried on one side of a riser and floating on the other.
    const auto column = static_cast<int>(std::lround((worldX - ground.originX) / std::max(ground.spacing, 1e-3f)));

    return ground.top[std::clamp(column, 0, ground.count - 1)];
}

float SunkAt(const Ground &ground, float worldX) {
    if (ground.sunk == nullptr || ground.count <= 0) return 0.0f;

    // The same nearest-column rule GroundAt uses, since the two are read together
    // and an answer taken from a different column than the height would be about
    // somewhere else.
    const auto column = static_cast<int>(std::lround((worldX - ground.originX) / std::max(ground.spacing, 1e-3f)));

    return ground.sunk[std::clamp(column, 0, ground.count - 1)];
}

std::int64_t CellAt(Layer layer, const Settings &settings, float worldX) {
    return FloorDiv(worldX, settings.layer[LayerIndex(layer)].cellSpan);
}

float Margin(Layer layer, const Settings &settings) {
    // A plant's centre always stays inside its own cell, and the interlock is the
    // only thing that lets any part of it out: the jitter is bounded by the cell
    // less a width, so raising the interlock by a share hands back half that
    // share of a width as overhang. At zero nothing crosses a cell border at all.
    const float interlock = std::clamp(settings.layer[LayerIndex(layer)].interlock, 0.0f, 1.0f);

    return WidestCanopy(layer) * kLargestScale * interlock * 0.5f;
}

bool Grow(Layer layer, std::int64_t cell, const Settings &settings, const terrain::Settings &terrain,
          const Ground &ground, Plant &out) {
    const LayerSettings &rules = settings.layer[LayerIndex(layer)];
    const int seed             = settings.seed;

    const float centre = (static_cast<float>(cell) + 0.5f) * rules.cellSpan;

    // The climate is read at the centre of the cell rather than at the position
    // the plant ends up in, which breaks what would otherwise be a circle: the
    // jitter is bounded by the species' own width, so the species has to be
    // known before the position is. It costs nothing to be right about — a cell
    // is a hundred pixels and a climate feature is several thousand, so the two
    // readings differ by less than the field's own rounding.
    const terrain::Climate climate = terrain::ClimateAt(centre, terrain);

    // And what the ground here is made of, read at the same point and for the
    // same reason the climate is: the position a plant ends up in depends on how
    // wide it is, which depends on which species it is, which is what is being
    // decided. A cell is a hundred and ten pixels; the coarsest of the three
    // cover fields has features four times that, and its border is broken into
    // patches by its own jitter — so a reading half a cell off is inside the
    // noise the border already has.
    const std::optional<Element> cover = SurfaceCoverAt(centre, terrain);

    // Every species of this layer weighed against this place: whether it will
    // root in the ground at all, how well the climate suits it, how common it is
    // meant to be, and how much this stretch of country favours it.
    float weight[kSpeciesCount] = {};
    float suited[kSpeciesCount] = {};

    float total = 0.0f;
    float best  = 0.0f;

    for (std::size_t e = 0; e < kSpeciesCount; e++) {
        const SpeciesDef &def = kSpecies[e];
        if (def.layer != layer) continue;

        // The ground first, and as a gate rather than a weight.
        //
        // Before `best` as well as before the roll, which is the half of it that
        // actually emptied the desert. `best` is what the support floor lifts, and
        // the floor is deliberately generous — it is what makes the country
        // between two species' ranges a thin wood instead of a bare one. A desert
        // is not between two ranges, it is past the end of every one of them, and
        // a suitability of a thousandth still cleared the floor and put a full
        // half-thickness wood on the sand. A species that cannot root here does
        // not get to say how much life the place supports.
        if (!RootsIn(def, cover)) continue;

        suited[e] = Suitability(def, climate);
        best      = std::max(best, suited[e]);

        // Each species reads the stand field at its own stride along it, so the
        // four of them have four slow swells that peak in different places. Where
        // one is high and the others are low the wood comes out nearly pure, and
        // the sharpening is what makes that "nearly" rather than "somewhat".
        const float stand = std::max(
            terrain::Sample({centre + static_cast<float>(e) * settings.standStride, 0.0f},
                            Reseed(settings.stand, seed)),
            0.0f);

        weight[e] = suited[e] * def.climate.abundance * std::pow(stand, settings.standSharpness);
        total += weight[e];
    }

    if (total <= 0.0f || best <= 0.0f) return false;

    // Which species, by a single roll across the cumulative weights.
    std::size_t chosen = kSpeciesCount;
    float roll         = Roll(cell, 11, seed) * total;

    for (std::size_t e = 0; e < kSpeciesCount; e++) {
        if (weight[e] <= 0.0f) continue;

        roll -= weight[e];
        if (roll > 0.0f) continue;

        chosen = e;
        break;
    }

    if (chosen >= kSpeciesCount) return false;

    const SpeciesDef &def = kSpecies[chosen];

    // How big this one came out. Its own variation, and then how well the place
    // suits it, so a species at the edge of its range stands there stunted
    // rather than either absent or full size.
    const float scale =
        (0.86f + 0.28f * Roll(cell, 17, seed)) * (0.78f + 0.22f * std::clamp(suited[chosen], 0.0f, 1.0f));

    const float width = def.canopyWidth[StageIndex(Stage::Mature)] * scale;

    // Jittered inside the cell, bounded by the cell less the plant's own width.
    // At an interlock of zero the canopy cannot cross the cell border and no two
    // can touch; at one two neighbours reach into each other by a full width.
    // This is the whole of the spacing rule — there is no neighbour to consult,
    // because the bound already answered the question.
    const float room = std::max(rules.cellSpan - width * (1.0f - std::clamp(rules.interlock, 0.0f, 1.0f)), 0.0f);
    const float x    = centre + (Roll(cell, 23, seed) - 0.5f) * room;

    const float surface = GroundAt(ground, x);

    // Something to stand on. Measured across the trunk's own footing rather than
    // as a slope, because the thing this has to catch is not a steep hillside but
    // a hole: a column open into a shaft is not steep, and a slope taken across
    // it averages straight over the gap.
    const float footing = std::max(def.canopyWidth[StageIndex(Stage::Mature)] * def.shape.trunkWidth * scale * 0.5f,
                                   ground.spacing);

    const float leftFoot  = GroundAt(ground, x - footing);
    const float rightFoot = GroundAt(ground, x + footing);

    if (std::max({surface, leftFoot, rightFoot}) - std::min({surface, leftFoot, rightFoot}) > rules.dropLimit) {
        return false;
    }

    // And not down a hole, which the footing test cannot see. It compares the
    // ground against itself, so a level ledge inside a cave mouth passes it three
    // times over; only the distance to the land's own surface tells them apart.
    if (SunkAt(ground, x) > rules.rootLimit) return false;

    // Then the lie of the land, which thins a wood out on a hillside rather than
    // ending it. Measured over a wide baseline on purpose: the surface is
    // terraced into risers a quarter of a jump high, and a baseline narrow enough
    // to sit on one riser would read every hillside as a cliff.
    const float rise  = GroundAt(ground, x + rules.slopeSpan) - GroundAt(ground, x - rules.slopeSpan);
    const float slope = std::fabs(rise) / std::max(2.0f * rules.slopeSpan, 1e-3f);
    const float flat  = 1.0f - SmoothStep(rules.slopeLimit * 0.55f, rules.slopeLimit, slope);

    // Y grows downward, so ground above the ceiling is the smaller number and
    // this is zero there.
    const float treeline =
        SmoothStep(def.climate.ceiling, def.climate.ceiling + def.climate.ceilingFade, surface);

    // Where the woods are at all. Ramped across the cutoff rather than cut at it,
    // so a wood has an edge it thins out over instead of a fence around it. The
    // ramp is symmetric, so it costs the measured coverage nothing on average.
    const float cutoff = settings.calibration.forest[LayerIndex(layer)];
    const float field  = terrain::Sample({x, 0.0f}, Reseed(settings.forest, seed));
    const float wood   = SmoothStep(cutoff - settings.edgeBand, cutoff + settings.edgeBand, field);

    // The best suitability of any species here rather than the chosen one's,
    // because this decides how much life the place supports and the roll above
    // has already decided what kind. Using the chosen one would dip the density
    // of a whole wood wherever an uncommon species happened to win a cell.
    //
    // Lifted onto a floor rather than used outright, for the reason written
    // against supportFloor: a bell peaks at exactly one point and averages far
    // below it, so the raw figure thinned every wood in the world at once.
    const float lowest  = std::clamp(settings.supportFloor, 0.0f, 1.0f);
    const float support = lowest + (1.0f - lowest) * best;

    const float density = wood * support * treeline * flat;

    if (Roll(cell, 29, seed) >= density) return false;

    // And the ground again, now under the trunk rather than under the middle of
    // the cell it grew in.
    //
    // The gate above had to run before the species was known, and the jitter can
    // carry a plant half a cell from where that reading was taken — which over a
    // whole world left a handful of oaks standing a few pixels onto a snowfield
    // and one scrub bush in the grass. Small, and the wrong kind of small: the
    // rule this is enforcing is "never", and a rule that is kept 99.6% of the time
    // is one somebody will find the exception to and report as a bug. What it
    // costs is a bare pixel or two at a border a plant would have straddled, which
    // is what the edge of a desert looks like anyway.
    if (!RootsIn(def, SurfaceCoverAt(x, terrain))) return false;

    out.id      = PlantId(layer, cell);
    out.species = static_cast<Species>(chosen);
    out.scale   = scale;

    // Exactly on the drawn surface, and *not* a half step under it.
    //
    // It used to be sunk by half a lattice step, and the reasoning was sound at
    // the time: the surface was the first solid vertex, what is drawn is the
    // square between that vertex and the empty one above it, so a trunk based on
    // the vertex stood a few pixels clear of the ground it grew out of. Sinking it
    // was the fix available then.
    //
    // It is not the fix now. `Ground::top` is filled with marching_squares::
    // DrawnTop — the world Y of the top edge of the topmost square the ground is
    // actually drawn as — so the gap that half step was closing is already closed,
    // by measurement rather than by allowance. What was left was the allowance,
    // applied on top: three pixels of a five pixel texel, which is a trunk visibly
    // buried in the soil and is the shape the fault arrived in.
    //
    // Two corrections for one error is the usual way this goes, and the tell is
    // that the second one is a constant. A trunk sits on the ground because the
    // number underneath it says where the ground is, not because a fudge happens
    // to be the right size.
    out.base = {x, surface};

    return true;
}

void Scatter(Layer layer, float fromX, float toX, const Settings &settings, const terrain::Settings &terrain,
             const Ground &ground, std::vector<Plant> &out) {
    out.clear();

    const float span   = settings.layer[LayerIndex(layer)].cellSpan;
    const float margin = Margin(layer, settings);

    // Widened by how far a plant may hang out of its own cell, so one rooted just
    // off the edge still has its cell considered and does not pop into being as
    // the view reaches it.
    const std::int64_t first = FloorDiv(fromX - margin, span);
    const std::int64_t last  = FloorDiv(toX + margin, span);

    Plant plant;

    for (std::int64_t cell = first; cell <= last; cell++) {
        if (Grow(layer, cell, settings, terrain, ground, plant)) out.push_back(plant);
    }
}

Skeleton Build(Species species, Stage stage, std::int64_t seed, float scale) {
    const SpeciesDef &def   = Def(species);
    const SpeciesShape &art = def.shape;

    // The plant's own seed, mixed with what it is and how far along it is. Two
    // trees of a species are two different trees; one tree asked about twice is
    // the same tree.
    const std::int64_t key = seed * 1000003 + static_cast<std::int64_t>(SpeciesIndex(species) * 977 + StageIndex(stage) * 131) + 1;

    Skeleton skeleton;

    skeleton.height = def.height[StageIndex(stage)] * scale;
    skeleton.width  = def.canopyWidth[StageIndex(stage)] * scale;

    const float half      = skeleton.width * 0.5f;
    const float baseWidth = skeleton.width * art.trunkWidth;

    // Two coefficients give the trunk a bend rather than a tilt: a straight lean
    // reads as a tree that was pushed, a curve reads as one that grew.
    const float bendA = (Roll(key, 3, 0) - 0.5f) * 2.0f;
    const float bendB = (Roll(key, 5, 0) - 0.5f) * 2.0f;

    for (int n = 0; n < kTrunkNodes; n++) {
        const float u = static_cast<float>(n) / static_cast<float>(kTrunkNodes - 1);

        skeleton.trunk[n] = {art.lean * baseWidth * (bendA * u * u + bendB * u * 0.5f),
                             u * art.trunkReach * skeleton.height};

        skeleton.trunkWidth[n] = baseWidth * (1.0f - u * (1.0f - art.trunkTaper));
    }

    const float crownFoot = art.clearance * skeleton.height;
    const float crownSpan = std::max(skeleton.height - crownFoot, 1e-3f);

    const bool frond = art.crown == Crown::Frond;

    // A tier of a conifer lies across the trunk and is squashed flat; a clump is
    // a ball on the end of a limb and keeps its roundness. Three masses across a
    // clumped tier and two across a flat one, which is what fills the crown to
    // the width the table says it is: two alone left a narrow column of foliage
    // up the middle with the outer half of the crown empty.
    // A tier is squashed rather than flattened away. At a half it came out as a
    // row of horizontal dashes up a bare pole with sky between every one of
    // them; a conifer's tiers read as notches in one continuous mass, not as
    // separate marks, so each has to overlap the one above it.
    const float flatten = frond ? 0.80f : 1.0f;

    const int tiers = std::clamp(art.tiers, 1, kMaxLobes / 3);

    for (int tier = 0; tier < tiers && skeleton.lobeCount + 3 <= kMaxLobes; tier++) {
        const float t = (tiers > 1) ? static_cast<float>(tier) / static_cast<float>(tiers - 1) : 0.0f;
        const float y = crownFoot + t * crownSpan;

        // Three masses across the upper crown and two across the lower.
        //
        // The gap this leaves is the point. Every reference tree of this kind
        // shows its trunk climbing through the middle of the lower crown with
        // foliage to either side and sky between — and a tier with a mass in the
        // middle closes exactly that gap. Three across the top gives the dome
        // that sits over it.
        const int across = (frond || t >= 0.5f) ? 3 : 2;

        // The outline of the crown, as one number per height. Three terms, and
        // between them they draw both reference shapes:
        //
        //   fall   thins towards the top by `taper` — near one it barely thins
        //          and the crown is a column, well below one it closes to a
        //          point and the crown is a cone
        //   pinch  pulls the very bottom in, so a crown is a mass sitting on the
        //          trunk rather than a skirt hanging off it
        //   cap    closes the last third, so neither shape ends in a flat lid
        const float fall  = 1.0f - (1.0f - art.taper) * t;
        const float pinch = 0.72f + 0.28f * SmoothStep(0.0f, 0.30f, t);
        // The cap closes a rounded crown over its last third. A conifer needs
        // none of it: its taper already brings the tiers to a point, and closing
        // them twice shrank the top ones below the size at which they can hold
        // any foliage — which left either a scatter of dark specks or, once
        // those were dropped, a bare leader standing out of the tree.
        const float cap = frond ? 1.0f : 1.0f - 0.35f * SmoothStep(0.70f, 1.0f, t);

        // The masses are placed at 0.42 of this and are 0.58 of it wide, so the
        // outermost one reaches exactly this far — which makes `reach` the half
        // width of the crown at this height, and the canopy width in the table
        // the width the crown actually comes out. Getting this wrong by a factor
        // is what left the first crowns a third narrower than they were meant to
        // be, and reading as a puff on a pole.
        const float reach = half * art.reach * fall * pinch * cap;
        const float axis  = TrunkAt(skeleton, y);

        for (int k = 0; k < across; k++) {
            const int salt = tier * 8 + k;

            // Spread over the full width of the tier rather than to one side or
            // the other, so a crown fills out instead of alternating.
            const float side = (across > 1) ? (static_cast<float>(k) / static_cast<float>(across - 1)) * 2.0f - 1.0f
                                            : 0.0f;

            const float wobbleX = (Roll(key, salt * 2 + 51, 0) - 0.5f) * 2.0f * art.jitter * reach;
            const float wobbleY = (Roll(key, salt * 2 + 53, 0) - 0.5f) * 2.0f * art.jitter * crownSpan * 0.4f;

            Lobe &lobe = skeleton.lobes[skeleton.lobeCount++];

            // Set apart by whatever the mass does not take, so the two together
            // reach exactly the tier's own half width and the canopy width in the
            // table stays the width the crown comes out at, whatever `mass` is.
            //
            // Against the *visible* radius, not the nominal one. A mass is laid
            // down as 1 − d² and cut at a half, so foliage only reaches d = 1/√2
            // and the drawn radius is seventy-one per cent of the one written
            // here. Budgeting the full radius quietly spent width that never
            // arrived: every crown in the table came out an eighth narrower than
            // it asked for, and the shortfall grew as the masses did.
            lobe.at     = {axis + side * reach * (1.0f - art.mass * kVisibleRadius) + wobbleX, y + wobbleY};
            lobe.radius = reach * art.mass * (0.90f + 0.20f * Roll(key, salt * 2 + 57, 0));
            lobe.flatten = flatten;
            lobe.salt    = salt;

            // Zero at the top of the crown and one at its foot, so whatever
            // shades the tones has a depth to read without being told where the
            // sun is.
            lobe.depth = 1.0f - t;

            // A tier droops away from the trunk: its outer end hangs below where
            // it left, which is the line every drawn conifer is made of.
            if (frond) lobe.at.y -= std::fabs(side) * reach * 0.30f;

            // Only the masses out at the edge get a limb drawn to them, and not
            // the very topmost tier: up there the foliage has closed to almost
            // nothing, and a branch drawn into it stands clear of the crown as a
            // dark spike against the sky.
            if (std::fabs(side) < 0.5f || t > 0.82f || skeleton.branchCount >= kMaxBranches) continue;

            // Stopped short of the middle of the mass, so the join is inside the
            // leaves and only the bare stretch shows through the gap.
            Branch &branch = skeleton.branches[skeleton.branchCount++];

            branch.from  = {axis, y - (frond ? 0.0f : reach * 0.12f)};
            branch.tip   = lobe.at;
            branch.to    = {branch.from.x + (lobe.at.x - branch.from.x) * art.branchReach,
                            branch.from.y + (lobe.at.y - branch.from.y) * art.branchReach};
            branch.width = std::max(baseWidth * 0.30f * (1.0f - 0.45f * t), 1.0f);
        }
    }

    // The box the whole plant stands in, foliage included, so a rasteriser can
    // size its canvas without walking the masses again to find out.
    skeleton.left  = 0.0f;
    skeleton.right = 0.0f;

    for (int n = 0; n < kTrunkNodes; n++) {
        skeleton.left  = std::min(skeleton.left, skeleton.trunk[n].x - skeleton.trunkWidth[n]);
        skeleton.right = std::max(skeleton.right, skeleton.trunk[n].x + skeleton.trunkWidth[n]);
    }

    for (int i = 0; i < skeleton.lobeCount; i++) {
        skeleton.left  = std::min(skeleton.left, skeleton.lobes[i].at.x - skeleton.lobes[i].radius);
        skeleton.right = std::max(skeleton.right, skeleton.lobes[i].at.x + skeleton.lobes[i].radius);
    }

    return skeleton;
}

} // namespace flora
