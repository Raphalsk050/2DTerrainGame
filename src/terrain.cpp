#include "terrain.h"

#include "cave.h"

// stb_perlin ships inside raylib. Without STB_PERLIN_IMPLEMENTATION this header
// contributes declarations only; the implementation is already compiled into
// libraylib. The include path is set in CMakeLists.txt.
#include "stb_perlin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace terrain {
namespace {

// A distance no world position is ever that far from anything, so a layer that
// is switched off can report itself as unreachably distant instead of every
// caller having to ask whether it is on.
constexpr float kFar = 1.0e9f;

// Practical peak of a sum of Perlin octaves divided by their total amplitude.
//
// Such a sum concentrates well inside [-1,1] — measured over a wide area its
// root mean square is about 0.17 and its ninety-ninth percentile about 0.4 — so
// an amplitude multiplied by it straight would deliver less than half the
// pixels it asks for. Dividing by this is what makes `reliefAmplitude = 150`
// mean a hundred and fifty pixels of relief.
//
// Chosen so the ninety-ninth percentile lands near one. The rarest few samples
// overshoot it, which for a height is a peak worth having and for anything
// bounded is clamped where it is used.
constexpr float kFbmPeak = 0.45f;

// How much faster than its own frequency each octave travels through the third
// axis of the noise. See Fbm: it only has to be a ratio that is not a whole
// number, and small enough that an octave stays at roughly its own scale.
constexpr float kOctaveDepth = 0.31f;

// Step in pixels used to read the slope of a field by difference.
//
// Small against the finest octave in use, so it measures the slope at the point
// rather than across a feature, and large enough that the difference between two
// samples is well clear of the precision of a float.
constexpr float kProbe = 4.0f;

// Fractal Brownian motion: sums several octaves of Perlin noise, each with its
// frequency multiplied by `lacunarity` and its amplitude by `gain`. Layering is
// what gives a field its natural appearance; a single octave is featureless.
//
// The result is renormalised by the accumulated amplitude, which keeps contrast
// independent of the octave count so that a cutoff measured at one octave count
// stays meaningful at another, and then by kFbmPeak so that the range it
// practically covers is [-1,1] rather than a fraction of it.
float Fbm(float x, float y, float z, const NoiseShape &s) {
    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < s.octaves; o++) {
        // Perturbing the seed per octave decorrelates the layers. Sharing one
        // seed would repeat the same pattern at every scale.
        //
        // The depth is scaled by a little over the frequency each octave rather
        // than by the frequency alone. Perlin noise gives up a little of its
        // variance on the integer planes of its own lattice, and at a plain
        // frequency the octaves sit at one, two and four times the same depth —
        // so their flat spots line up and the field's contrast dips on a fixed
        // period. A ratio that is not a whole number spreads them out.
        //
        // Multiplied, never added. A field with no depth to it has to stay
        // exactly the two-dimensional field it always was, and every layer of
        // the terrain is one.
        const float depth = z * frequency * (1.0f + kOctaveDepth * static_cast<float>(o));

        sum += stb_perlin_noise3_seed(x * frequency, y * frequency, depth, 0, 0, 0, s.seed + o) * amplitude;

        maxAmplitude += amplitude;
        frequency *= s.lacunarity;
        amplitude *= s.gain;
    }

    return (maxAmplitude > 0.0f) ? (sum / (maxAmplitude * kFbmPeak)) : 0.0f;
}

// A layer's shape shifted by the world's master seed, so one number reseeds
// everything while the layers stay decorrelated from each other.
NoiseShape Reseed(NoiseShape shape, int seed) {
    shape.seed += seed;
    return shape;
}

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Aspect below one compresses the horizontal axis, which is what makes a
// feature run up and down rather than sideways. Clamped away from zero because
// it divides.
float Aspect(const NoiseShape &shape) { return std::max(shape.aspect, 1e-3f); }

// Value a field exceeds over exactly `coverage` of the area sampled.
//
// Measured rather than declared. A cutoff written by hand is a statement about
// the shape of the field, so every change to frequency or octaves would quietly
// change how much of the world it claims; a quantile is a statement about the
// share of the world, which is what the settings are trying to say.
float Quantile(const NoiseShape &shape, float coverage, Vector2 centre, float width, float height) {
    constexpr int kSamplesPerAxis = 96;

    const float share = std::clamp(coverage, 0.0f, 1.0f);

    // Nothing clears a cutoff of one and everything clears a cutoff of zero, so
    // the two extremes need no samples to answer.
    if (share <= 0.0f) return 1.0f;
    if (share >= 1.0f) return 0.0f;

    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(kSamplesPerAxis) * kSamplesPerAxis);

    for (int i = 0; i < kSamplesPerAxis; i++) {
        for (int j = 0; j < kSamplesPerAxis; j++) {
            const Vector2 at = {centre.x + (i / static_cast<float>(kSamplesPerAxis) - 0.5f) * width,
                                centre.y + (j / static_cast<float>(kSamplesPerAxis) - 0.5f) * height};

            values.push_back(Sample(at, shape));
        }
    }

    const auto index = static_cast<std::size_t>((1.0f - share) * (values.size() - 1));

    std::nth_element(values.begin(), values.begin() + index, values.end());

    return values[index];
}

// Heights snapped towards multiples of the ledge height.
//
// What it buys is flat ground on a slope. A continuous incline is walkable but
// featureless; ledges give somewhere to stand, and the risers between them read
// as cliffs.
float Terrace(float height, const SurfaceSettings &s) {
    if (s.terrace <= 0.0f || s.terraceStep <= 0.0f) return height;

    const float at = height / s.terraceStep;

    // Which ledge this is on, and how far up the riser above it.
    const float ledge = std::floor(at);
    const float climb = at - ledge;

    // The riser, shaped rather than jumped.
    //
    // This was `round`, which is to say: below the halfway mark take the ledge
    // under you, above it take the one over you. That is a step function, and a
    // step function is crossed in no distance at all — see
    // SurfaceSettings::terraceSharp for what a surface with no width in its risers
    // does to everything that reads it one column at a time.
    //
    // The curve here is the same shape with the discontinuity taken out: flat
    // where a ledge is, steep where the riser is, and continuous through both. At
    // a sharpness of one it is the identity and there is no terrace at all, which
    // is the right answer for a knob turned down.
    const float sharp = std::max(s.terraceSharp, 1.0f);

    const float up   = std::pow(climb, sharp);
    const float down = std::pow(1.0f - climb, sharp);

    const float shaped = (up + down > 1e-9f) ? up / (up + down) : climb;

    return height + ((ledge + shaped) * s.terraceStep - height) * std::min(s.terrace, 1.0f);
}

// Horizontal position the surface is read at.
//
// Displacing it by an amount that varies with height folds the surface, and a
// folded surface has overhangs. Nothing else in the generator can produce them:
// the surface is a function of one variable, and a function has one value per
// column.
float WarpedX(Vector2 world, const Settings &s) {
    const SurfaceSettings &surface = s.surface;
    if (surface.warpAmplitude <= 0.0f) return world.x;

    // Measured against the nominal level rather than against the real surface,
    // which is the thing being computed here. The two differ by the relief, and
    // all that changes is how quickly the fold dies out with depth.
    const float depth = std::abs(world.y - surface.level);
    const float reach = 1.0f - SmoothStep(0.0f, std::max(surface.warpDepth, 1.0f), depth);
    if (reach <= 0.0f) return world.x;

    return world.x + Signed(world, Reseed(surface.warp, s.seed)) * surface.warpAmplitude * reach;
}

// How much of the cave layers the depth allows, in [0,1]. Zero within the crust,
// full below it.
float CrustAllowance(float depth, const CaveSettings &c) {
    return SmoothStep(c.crust, c.crust + c.crustFade, depth);
}

// Pixels of rock the wall roughness adds at a position, positive or negative.
//
// Applied to the finished distance rather than to any one layer, so one field
// roughens every wall in the world at the same scale — which is what a rock type
// does. Faded out away from a surface, because the term is added to the whole
// field and a roughness that reached everywhere would leave bubbles of air deep
// in the rock and pillars of rock in the middle of the air.
float Roughness(Vector2 world, float solid, const RoughnessSettings &r, int seed, float allowance) {
    if (allowance <= 0.0f) return 0.0f;
    if (r.amplitude <= 0.0f && r.lobeAmplitude <= 0.0f) return 0.0f;

    const float near = 1.0f - SmoothStep(0.0f, std::max(r.reach, 1e-3f), std::abs(solid));
    if (near <= 0.0f) return 0.0f;

    float moved = 0.0f;

    if (r.amplitude > 0.0f) {
        // Folded, so the field creases where it crosses zero and domes where it
        // peaks. A smooth field added to a wall would only make the wall a
        // slightly different smooth wall; it is the creases that read as broken
        // rock.
        moved += (std::abs(Signed(world, Reseed(r.shape, seed))) - r.bias) * r.amplitude;
    }

    if (r.lobeAmplitude > 0.0f) {
        // And the bites. Worley is zero at each cell's own feature point and
        // rises towards the walls between them, so subtracting the *inverse* of
        // it takes a rounded bite out of the rock centred on every feature point
        // and leaves the rock between them standing. `lobeBite` is how far into a
        // cell one bite reaches; past the point where neighbouring bites meet the
        // wall is scalloped edge to edge and stops reading as rock again.
        const float cell = Worley(world, Reseed(r.lobes, seed));

        moved -= (1.0f - SmoothStep(0.0f, std::clamp(r.lobeBite, 1e-3f, 1.0f), cell)) * r.lobeAmplitude;
    }

    return moved * near * allowance;
}

} // namespace

float Signed(Vector2 world, const NoiseShape &shape) {
    // The vertical axis sets the scale and the aspect stretches the horizontal
    // one against it, so noise cells stay square at an aspect of one.
    const float nx = (world.x + shape.offsetX) * shape.frequency / (kFeatureSpan * Aspect(shape));
    const float ny = (world.y + shape.offsetY) * shape.frequency / kFeatureSpan;

    // Scaled the same way, so a depth written in world pixels covers as much of
    // the field as the same number of pixels travelled across it.
    const float nz = shape.offsetZ * shape.frequency / kFeatureSpan;

    return Fbm(nx, ny, nz, shape);
}

float Worley(Vector2 world, const NoiseShape &shape) {
    const float aspect = std::max(shape.aspect, 1e-3f);

    const float nx = (world.x + shape.offsetX) * shape.frequency / (kFeatureSpan * aspect);
    const float ny = (world.y + shape.offsetY) * shape.frequency / kFeatureSpan;

    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < shape.octaves; o++) {
        const float x = nx * frequency;
        const float y = ny * frequency;

        const float cx = std::floor(x);
        const float cy = std::floor(y);

        // One feature point per cell, placed inside it by a hash of the cell. The
        // nearest one is always in this cell or one of the eight around it, so nine
        // is the whole search.
        float nearest = 2.0f;

        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                const float gx = cx + static_cast<float>(dx);
                const float gy = cy + static_cast<float>(dy);

                const auto cell = static_cast<unsigned>(static_cast<int>(gx) * 738295859
                                                        ^ static_cast<int>(gy) * 1103515245
                                                        ^ (shape.seed + o) * 2654435761u);

                // Two independent values out of the one hash, for the point's place
                // in its cell.
                unsigned bits = cell;
                bits ^= bits >> 15;
                bits *= 2246822519u;
                bits ^= bits >> 13;

                const float px = gx + static_cast<float>(bits & 0xffffu) / 65536.0f;
                const float py = gy + static_cast<float>((bits >> 16) & 0xffffu) / 65536.0f;

                const float ox = px - x;
                const float oy = py - y;

                nearest = std::min(nearest, ox * ox + oy * oy);
            }
        }

        // Squared until here, because comparing squares avoids a root per cell.
        sum += std::min(std::sqrt(nearest), 1.0f) * amplitude;

        maxAmplitude += amplitude;
        frequency *= shape.lacunarity;
        amplitude *= shape.gain;
    }

    return (maxAmplitude > 0.0f) ? std::clamp(sum / maxAmplitude, 0.0f, 1.0f) : 0.0f;
}

float Billow(Vector2 world, const NoiseShape &shape) {
    const float aspect = std::max(shape.aspect, 1e-3f);

    const float nx = (world.x + shape.offsetX) * shape.frequency / (kFeatureSpan * aspect);
    const float ny = (world.y + shape.offsetY) * shape.frequency / kFeatureSpan;

    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < shape.octaves; o++) {
        // The magnitude of the octave rather than its value. Everything that made
        // this field smooth is in the sign it is throwing away.
        const float value =
            stb_perlin_noise3_seed(nx * frequency, ny * frequency, 0.0f, 0, 0, 0, shape.seed + o);

        sum += std::abs(value) * amplitude;

        maxAmplitude += amplitude;
        frequency *= shape.lacunarity;
        amplitude *= shape.gain;
    }

    // Normalised by the same peak the signed field uses, so a billow and a sample
    // of the same shape cover comparable ranges and a cutoff measured against one
    // means something against the other.
    return (maxAmplitude > 0.0f) ? std::min(sum / (maxAmplitude * kFbmPeak), 1.0f) : 0.0f;
}

float Sample(Vector2 world, const NoiseShape &shape) {
    // Not clamped, deliberately. The rarest ore's cutoff is a quantile in the far
    // tail of this field, and clamping flattens the whole of that tail to one: the
    // cutoff comes out as one, nothing can clear it, and the ore never generates.
    // A caller that needs a strict share clamps for itself.
    return (Signed(world, shape) + 1.0f) * 0.5f;
}

void Calibrate(Settings &settings) {
    // Sampled over the underground rather than over the whole world, since
    // that is the only part either field is consulted about. Wide enough that
    // the very low frequencies get more than a handful of features into it.
    constexpr float kSampledWidth = 8000.0f;
    constexpr float kSampledDepth = 3000.0f;

    CaveSettings &caves  = settings.caves;
    const Vector2 centre = {0.0f, settings.surface.level + kSampledDepth * 0.5f};

    caves.calibration.region =
        Quantile(Reseed(caves.region, settings.seed), caves.regionCoverage, centre, kSampledWidth, kSampledDepth);

    caves.calibration.regionShallow = Quantile(Reseed(caves.region, settings.seed), caves.regionCoverageShallow, centre,
                                              kSampledWidth, kSampledDepth);
}

float Height(float worldX, const Settings &s) {
    const SurfaceSettings &surface = s.surface;
    const Vector2 at               = {worldX, 0.0f};

    // Erosion comes first because it scales the two terms under it, so a flat
    // stretch of world is flat in both the hills and the detail rather than
    // smooth at one scale and rough at the other.
    // Clamped, since this one is read as a share: the field overshoots [0,1] on
    // its rarest samples, and an erosion above one would put a dent in the ground
    // rather than flattening it.
    const float eroded = std::clamp(Sample(at, Reseed(surface.erosion, s.seed)), 0.0f, 1.0f);
    const float relief = 1.0f - eroded * (1.0f - std::clamp(surface.erosionFloor, 0.0f, 1.0f));

    float elevation = Signed(at, Reseed(surface.relief, s.seed)) * surface.reliefAmplitude;
    elevation += Signed(at, Reseed(surface.hills, s.seed)) * surface.hillAmplitude * relief;
    elevation += Signed(at, Reseed(surface.detail, s.seed)) * surface.detailAmplitude * relief;

    // Y grows downward, so elevation is subtracted: more of it is higher ground
    // and a smaller Y.
    return Terrace(surface.level - elevation, surface);
}

Climate ClimateAt(float worldX, const Settings &s) {
    const ClimateSettings &c = s.climate;
    const Vector2 at         = {worldX, 0.0f};

    // Height of the ground above the level it sits at with every modifier
    // neutral. Y grows downward, so higher ground is the smaller number and the
    // subtraction is this way round.
    const float elevation = std::max(s.surface.level - Height(worldX, s), 0.0f);

    Climate climate;

    climate.temperature =
        std::clamp(Sample(at, Reseed(c.temperature, s.seed)) - elevation * c.temperatureLapse, 0.0f, 1.0f);

    climate.humidity = std::clamp(Sample(at, Reseed(c.humidity, s.seed)) + elevation * c.humidityLift, 0.0f, 1.0f);

    return climate;
}

float Depth(Vector2 world, const Settings &s) {
    return world.y - Height(WarpedX(world, s), s);
}

WaterTable TableAt(float worldX, const Settings &s) {
    const AquiferSettings &a = s.aquifer;

    // Read on the line y = 0, like the surface and the climate, so that it is a
    // function of the horizontal position alone. A level that varied with height
    // would not be one.
    const float raw = s.surface.level + a.depth + Signed({worldX, 0.0f}, Reseed(a.level, s.seed)) * a.swing;

    // Snapped, so that it is flat rather than merely nearly flat. See
    // AquiferSettings::step.
    const float step = std::max(a.step, 1.0f);

    return {std::floor(raw / step) * step};
}

namespace {

// Solidity at a position whose depth below the surface has already been found.
//
// Split out so that a caller wanting both numbers pays for the surface once. The
// depth is the whole of what Solidity needs from the surface, so handing it in is
// the entire saving.
// Every system that could reach a position, built once and kept.
//
// The memo is not state in any sense that matters: it holds the value of a pure
// function of the cell index, and throwing it away changes nothing but the time.
// Per thread, because the light solve runs on several and a shared one would need
// a lock on the hottest path in the generator.
//
// Small on purpose. A chunk is 192 px against a cell 1600 wide, so the nine cells
// a chunk needs are asked for over and over and then never again; a handful of
// entries is the whole of what there is to gain, and an unbounded map would keep
// every system the player ever walked past.
struct Memo {
    struct Entry {
        std::int64_t cellX = 0;
        std::int64_t cellY = 0;
        int seed           = 0;
        bool built         = false;
        cave::System system;
    };

    static constexpr std::size_t kEntries = 32;

    std::array<Entry, kEntries> entries{};
    std::size_t next = 0;
};

// Whether a cell holds a system, and where it starts.
//
// The gate as well as the placement: a cell whose origin lands in the crust or
// outside cave country holds nothing at all. Applied to the *origin* rather than
// along the walk, so a system is one thing — a cave either is here or is not,
// rather than fading out along its own length the way a thresholded field does.
bool Sited(std::int64_t cellX, std::int64_t cellY, const Settings &s, Vector2 &outOrigin, float &outDepth) {
    const CaveSettings &caves = s.caves;

    if (!cave::Origin(cellX, cellY, caves.systems, s.seed, outOrigin)) return false;

    outDepth = outOrigin.y - Height(outOrigin.x, s);
    if (outDepth <= caves.crust) return false;

    const float cutoff = caves.calibration.regionShallow
                       + (caves.calibration.region - caves.calibration.regionShallow)
                             * SmoothStep(0.0f, std::max(caves.regionDeepens, 1.0f), outDepth);

    return Sample(outOrigin, Reseed(caves.region, s.seed)) > cutoff;
}

const cave::System &Systems(std::int64_t cellX, std::int64_t cellY, const Settings &s) {
    static thread_local Memo memo;
    static const cave::System kNone;

    for (const Memo::Entry &entry : memo.entries) {
        if (entry.built && entry.cellX == cellX && entry.cellY == cellY && entry.seed == s.seed) return entry.system;
    }

    Memo::Entry &slot = memo.entries[memo.next];

    memo.next = (memo.next + 1) % Memo::kEntries;

    slot.cellX  = cellX;
    slot.cellY  = cellY;
    slot.seed   = s.seed;
    slot.built  = true;
    slot.system = cave::System{};

    const CaveSettings &caves = s.caves;

    Vector2 origin{};
    float depth = 0.0f;

    if (!Sited(cellX, cellY, s, origin, depth)) return slot.system;

    // The four neighbours, so the corridors between systems can be aimed. Each
    // side digs to the midpoint of the two origins, so both halves meet without
    // either having seen the other's walk.
    cave::Neighbour around[4];

    constexpr std::int64_t kStepX[4] = {-1, 1, 0, 0};
    constexpr std::int64_t kStepY[4] = {0, 0, -1, 1};

    for (int n = 0; n < 4; n++) {
        float ignored = 0.0f;

        around[n].has = Sited(cellX + kStepX[n], cellY + kStepY[n], s, around[n].origin, ignored);
    }

    // The shallowest the walks may reach, taken over the whole span a walk could
    // cross rather than at the origin alone: the ground rises and falls, and a
    // ceiling measured under a valley would let a walk out through the hill
    // beside it.
    const float bound   = cave::Reach(caves.systems);
    const float surface = Height(origin.x, s);

    float highest = surface;

    for (int i = 0; i <= 8; i++) {
        highest = std::min(highest, Height(origin.x + bound * (static_cast<float>(i) / 4.0f - 1.0f), s));
    }

    slot.system = cave::Build(cellX, cellY, caves.systems, s.seed, origin, depth,
                              highest + caves.crust + caves.crustFade, surface, around);

    return slot.system;
}

// Solidity at a position whose depth below the surface has already been found.
//
// Split out so that a caller wanting both numbers pays for the surface once. The
// depth is the whole of what Solidity needs from the surface, so handing it in is
// the entire saving.
// Solidity at a position whose depth below the surface has already been found.
//
// Split out so that a caller wanting both numbers pays for the surface once. The
// depth is the whole of what Solidity needs from the surface, so handing it in is
// the entire saving.
float SolidityBelow(Vector2 world, float depth, const Settings &s) {
    const CaveSettings &caves = s.caves;

    // Above the ground there is nothing to carve out of. Every position in the
    // open sky leaves here having sampled the surface alone.
    if (depth <= 0.0f) return depth;

    const cave::Settings &dug = caves.systems;

    const auto cellX = static_cast<std::int64_t>(std::floor(world.x / std::max(dug.cellSpan, 1.0f)));
    const auto cellY = static_cast<std::int64_t>(std::floor(world.y / std::max(dug.cellRise, 1.0f)));

    // The nine cells around this one, and no more. A walk is hard-stopped at one
    // cell's reach, so nothing outside them can have dug here — see cave::Reach,
    // which is what makes that a fact rather than a hope.
    float into = -kFar;

    for (std::int64_t dy = -1; dy <= 1; dy++) {
        for (std::int64_t dx = -1; dx <= 1; dx++) {
            into = std::max(into, -cave::Carve(world, Systems(cellX + dx, cellY + dy, s)));
        }
    }

    float solid = std::min(depth, -into);

    // And the wall, last, because what it roughens is the outline every system
    // agreed on rather than any one of them. Held to the crust allowance so it
    // cannot bite into the ground the player walks on.
    return solid + Roughness(world, solid, caves.roughness, s.seed, CrustAllowance(depth, caves));
}

} // namespace

float Solidity(Vector2 world, const Settings &s) {
    return SolidityBelow(world, Depth(world, s), s);
}

Ground SampleGround(Vector2 world, const Settings &s) {
    const float depth = Depth(world, s);
    const float solid = SolidityBelow(world, depth, s);

    return {std::clamp(kSurfaceLevel + solid / kDensitySpan, 0.0f, 1.0f), depth, solid};
}

float Density(Vector2 world, const Settings &s) {
    return SampleGround(world, s).density;
}

bool IsSolid(Vector2 world, const Settings &s, float threshold) {
    return Density(world, s) > threshold;
}

void Fill(Grid &grid, const Settings &s) {
    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            grid.SetValue(i, j, Density(grid.PointAt(i, j), s));
        }
    }
}

Field Generate(const Settings &s, Vector2 origin, int cols, int rows, int spacing) {
    Field field;
    field.cols = cols;
    field.rows = rows;
    field.value.resize(static_cast<std::size_t>(cols) * rows);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const Vector2 world = {origin.x + static_cast<float>(i * spacing),
                                   origin.y + static_cast<float>(j * spacing)};

            // The finished density rather than a bare noise sample, so what the
            // debug view shows is the world that will be generated.
            field.value[i * rows + j] = Density(world, s);
        }
    }

    return field;
}

Image ToImage(const Field &field) {
    Image image = GenImageColor(field.cols, field.rows, BLACK);

    for (int i = 0; i < field.cols; i++) {
        for (int j = 0; j < field.rows; j++) {
            const auto v = static_cast<unsigned char>(field.At(i, j) * 255.0f);
            ImageDrawPixel(&image, i, j, {v, v, v, 255});
        }
    }

    return image;
}

} // namespace terrain
