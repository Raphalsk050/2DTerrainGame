#include "terrain.h"

// stb_perlin ships inside raylib. Without STB_PERLIN_IMPLEMENTATION this header
// contributes declarations only; the implementation is already compiled into
// libraylib. The include path is set in CMakeLists.txt.
#include "stb_perlin.h"

#include <algorithm>
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
float Fbm(float x, float y, const NoiseShape &s) {
    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < s.octaves; o++) {
        // Perturbing the seed per octave decorrelates the layers. Sharing one
        // seed would repeat the same pattern at every scale.
        sum += stb_perlin_noise3_seed(x * frequency, y * frequency, 0.0f, 0, 0, 0, s.seed + o) * amplitude;

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

    const float ledge = std::round(height / s.terraceStep) * s.terraceStep;

    return height + (ledge - height) * std::min(s.terrace, 1.0f);
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

// Half-width a tunnel layer has at a depth, in pixels, scaled by how much of
// the layer the position is entitled to.
//
// An allowance of zero leaves a half-width of zero, and a band of zero width
// carves nothing, so switching a layer off region by region needs no branch and
// pinches the tunnel shut smoothly instead of ending it on a flat wall.
float TunnelWidth(const TunnelLayer &layer, float depth, float allowance) {
    const float target = (layer.widthAtDepth > 0.0f) ? layer.widthAtDepth : layer.width;
    const float grown  = layer.width + (target - layer.width) * SmoothStep(0.0f, layer.growthDepth, depth);

    return grown * allowance;
}

// Signed distance into a tunnel in pixels: positive inside the corridor,
// negative in the rock around it.
//
// The corridor is a band around the *zero set* of the field, not around its
// peaks. That distinction is the whole layer: the region where a field exceeds
// a value is a set of patches, while its zero set is a family of long curves,
// so this gives corridors where a threshold would give pockets.
float Tunnel(Vector2 world, const TunnelLayer &layer, int seed, float halfWidth) {
    if (halfWidth <= 0.0f) return -kFar;

    const NoiseShape shape = Reseed(layer.shape, seed);
    const float value      = Signed(world, shape);

    // Distance to the zero set, taken from the field's slope here rather than
    // from an average of it over the world.
    //
    // The average is what makes a band balloon. Where the field happens to run
    // flat near zero, a fixed number of field units spans a great deal of
    // ground, and the corridor arrives as an open pit the size of the flat
    // patch — which is how the surface ended up with a hole four hundred pixels
    // across. Dividing by the local slope makes the corridor the width it was
    // asked for wherever it goes, and costs two more samples of the field.
    const float gx = (Signed({world.x + kProbe, world.y}, shape) - value) / kProbe;
    const float gy = (Signed({world.x, world.y + kProbe}, shape) - value) / kProbe;

    const float slope = std::sqrt(gx * gx + gy * gy);

    return halfWidth - std::abs(value) / std::max(slope, 1e-9f);
}

// How much of the regional cave layers a position is entitled to, in [0,1].
float RegionAllowance(Vector2 world, const CaveSettings &c, int seed) {
    const float cutoff = c.calibration.region;
    const float value  = Sample(world, Reseed(c.region, seed));

    return SmoothStep(cutoff - std::max(c.regionFade, 1e-3f), cutoff, value);
}

// Signed distance into a chamber in pixels.
//
// A room has no direction, so it is measured by how far the field is past its
// cutoff rather than by a distance to a curve. What gives a chamber a way in and
// out is the tunnel layers crossing it.
float Chamber(Vector2 world, const CaveSettings &c, int seed, float allowance) {
    if (c.chamberCoverage <= 0.0f || c.chamberDepth <= 0.0f || allowance <= 0.0f) return -kFar;

    const float cutoff = c.calibration.chamber;
    const float past   = Sample(world, Reseed(c.chamber, seed)) - cutoff;

    // Divided by the span the field still has above its cutoff, so the middle
    // of a chamber reaches the depth asked for whatever the cutoff came out at.
    // Without it, raising the coverage would also make every room taller.
    const float headroom = std::max(1.0f - cutoff, 1e-3f);

    return (past / headroom) * c.chamberDepth * allowance;
}

// How much of the cave layers the depth allows, in [0,1]. Zero within the crust,
// full below it.
float CrustAllowance(float depth, const CaveSettings &c) {
    return SmoothStep(c.crust, c.crust + c.crustFade, depth);
}

// How much of the entrance layer the depth allows, in [0,1].
//
// The one layer not held under the crust, since a cave nothing can walk into is
// scenery. It is bounded by its own reach instead, which has to clear the crust
// or an entrance opens onto solid rock and leads nowhere.
float ShaftAllowance(float depth, const CaveSettings &c) {
    if (depth < 0.0f) return 0.0f;

    // Held at full width for most of its reach and pinched shut over the last
    // third. Tapering the whole way down would leave a needle of rock-thin crack
    // below every entrance instead of a passage that ends.
    const float reach = std::max(c.shaftReach, 1.0f);

    return 1.0f - SmoothStep(reach * 0.7f, reach, depth);
}

} // namespace

float Signed(Vector2 world, const NoiseShape &shape) {
    // The vertical axis sets the scale and the aspect stretches the horizontal
    // one against it, so noise cells stay square at an aspect of one.
    const float nx = (world.x + shape.offsetX) * shape.frequency / (kFeatureSpan * Aspect(shape));
    const float ny = (world.y + shape.offsetY) * shape.frequency / kFeatureSpan;

    return Fbm(nx, ny, shape);
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

    caves.calibration.chamber =
        Quantile(Reseed(caves.chamber, settings.seed), caves.chamberCoverage, centre, kSampledWidth, kSampledDepth);
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

float Solidity(Vector2 world, const Settings &s) {
    const CaveSettings &caves = s.caves;

    const float depth = Depth(world, s);

    // Above the ground there is nothing to carve out of, and the cave layers are
    // what this function spends its time on. Every position in the open sky
    // leaves here having sampled the surface alone.
    if (depth <= 0.0f) return depth;

    float solid = depth;

    // Entrances first, since they are the only layer that reaches into the
    // crust and so the only one worth sampling this near the surface.
    const float reach = ShaftAllowance(depth, caves);
    if (reach > 0.0f) {
        solid = std::min(solid, -Tunnel(world, caves.shafts, s.seed, TunnelWidth(caves.shafts, depth, reach)));
    }

    const float crust = CrustAllowance(depth, caves);
    if (crust <= 0.0f) return solid;

    // How honeycombed this stretch of the underground is, which is what leaves
    // some of it near-solid rock and the rest cave country.
    const float country = RegionAllowance(world, caves, s.seed);

    // The halls only narrow towards solid rock rather than closing, so wherever
    // the player breaks in there is somewhere to go.
    const float floor = std::clamp(caves.galleryFloor, 0.0f, 1.0f);
    const float halls = crust * (floor + (1.0f - floor) * country);

    solid = std::min(solid, -Tunnel(world, caves.galleries, s.seed, TunnelWidth(caves.galleries, depth, halls)));

    // Everything else follows the region outright.
    const float region = crust * country;
    if (region <= 0.0f) return solid;

    solid = std::min(solid, -Tunnel(world, caves.crawlways, s.seed, TunnelWidth(caves.crawlways, depth, region)));
    solid = std::min(solid, -Chamber(world, caves, s.seed, region));

    return solid;
}

float Density(Vector2 world, const Settings &s) {
    return std::clamp(kSurfaceLevel + Solidity(world, s) / kDensitySpan, 0.0f, 1.0f);
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
