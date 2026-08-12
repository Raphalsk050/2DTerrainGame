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

// Union of two signed distances, flared over `blend` pixels where they meet.
//
// The exact union is the minimum, and a minimum creases: two corridors crossing
// leave a wedge of rock with a knife edge in each quadrant of the junction.
// Nothing in rock erodes to a point, so the crease is not only the worse picture
// but the wrong one, and rounding it is closer to the material than the exact
// answer is.
//
// The standard quadratic blend. Note that it is always at or below the true
// minimum, by up to a quarter of `blend`, so it carves as well as rounds.
float SmoothMin(float a, float b, float blend) {
    if (blend <= 0.0f) return std::min(a, b);

    const float h = std::clamp(0.5f + 0.5f * (b - a) / blend, 0.0f, 1.0f);

    return b + (a - b) * h - blend * h * (1.0f - h);
}

// Half-width a tunnel layer has at a position, in pixels, scaled by how much of
// the layer that position is entitled to.
//
// An allowance of zero leaves a half-width of zero, and a band of zero width
// carves nothing, so switching a layer off region by region needs no branch and
// pinches the tunnel shut smoothly instead of ending it on a flat wall.
//
// The width varies from place to place as well as with depth. Tunnel carves a
// band of exactly the width it is handed, by construction — so the only way a
// corridor gets a cross-section that changes along its length is for the width
// handed in to change, which is what the girth field is for. See TunnelLayer.
// `squeeze` is added to the layer's own pinch, for a caller that wants the
// corridor shut rather than narrowed. The two ways of holding a layer back are
// not interchangeable: an allowance scales the width, so a strong one still
// leaves a slit, while a pinch subtracts from it and takes it to nothing.
float TunnelWidth(Vector2 world, const TunnelLayer &layer, int seed, float depth, float allowance,
                  float squeeze = 0.0f) {
    const float target = (layer.widthAtDepth > 0.0f) ? layer.widthAtDepth : layer.width;
    const float grown  = layer.width + (target - layer.width) * SmoothStep(0.0f, layer.growthDepth, depth);

    const float pinch = layer.pinch + squeeze;

    if (layer.swing <= 0.0f && pinch <= 0.0f) return grown * allowance;

    // Signed rather than folded, so the modulation is centred: as much of the
    // corridor narrows as widens, and the layer's stated width stays the width it
    // averages rather than the width it never exceeds.
    const float swell = 1.0f + Signed(world, Reseed(layer.girth, seed)) * layer.swing - pinch;
    const float width = grown * std::max(swell, 0.0f) * allowance;

    // Collapsed below the width of one drawn texel.
    //
    // A width that tapers smoothly to nothing passes through every value on the
    // way, and the small ones are a passage the grid cannot express: what they
    // draw is a hairline scratch across the rock, one texel wide and hundreds
    // long, following the corridor that nearly was. Squaring the last texel of
    // width away takes those out without putting a step in the field, since it is
    // still continuous and still reaches zero at the same place.
    constexpr float kThinnest = 5.0f;

    return width * SmoothStep(0.0f, kThinnest, width);
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
//
// One field read against a cutoff that descends with depth, rather than two
// fields: the same stretch of world is the same stretch at every depth, so a
// system that is a crack near the surface opens into cave country under itself
// instead of a second, unrelated set of regions appearing further down.
float RegionAllowance(Vector2 world, float depth, const CaveSettings &c, int seed) {
    const float cutoff = c.calibration.regionShallow
                       + (c.calibration.region - c.calibration.regionShallow)
                             * SmoothStep(0.0f, std::max(c.regionDeepens, 1.0f), depth);

    const float value = Sample(world, Reseed(c.region, seed));

    return SmoothStep(cutoff - std::max(c.regionFade, 1e-3f), cutoff, value);
}

// How far a room's field is past its cutoff at a position, in pixels of rock.
//
// A room has no direction, so it is measured by how far the field is past its
// cutoff rather than by a distance to a curve. What gives it a way in and out is
// the tunnel layers crossing it.
float RoomAt(Vector2 world, const ChamberLayer &layer, float cutoff, int seed, float height) {
    // Divided by the span the field still has above its cutoff, so the middle of
    // a room reaches the height asked for whatever the cutoff came out at.
    // Without it, raising the coverage would also make every room taller.
    const float headroom = std::max(1.0f - cutoff, 1e-3f);

    return ((Sample(world, Reseed(layer.shape, seed)) - cutoff) / headroom) * height;
}

// Signed distance into a room in pixels, floor included.
float Chamber(Vector2 world, const ChamberLayer &layer, float cutoff, int seed, float depth, float allowance) {
    if (layer.coverage <= 0.0f || allowance <= 0.0f) return -kFar;

    // Rooms open out with depth, which is what makes the descent worth making.
    const float height = layer.height
                       + (layer.heightAtDepth - layer.height) * SmoothStep(layer.growthFrom, layer.growthTo, depth);

    if (height <= 0.0f) return -kFar;

    const float here = RoomAt(world, layer, cutoff, seed, height);

    if (layer.rubble <= 0.0f) return here * allowance;

    // The floor: a point stays open only if there is still room a rubble's
    // thickness *below* it. Where there is not, the point is within that distance
    // of the bottom of the void and is buried.
    //
    // Intersecting the room with itself lifted by that much takes the band off the
    // floor and leaves the ceiling untouched, which is the asymmetry a room needs
    // and the one a field cannot have on its own. Y grows downward, so below is
    // the larger number.
    //
    // Deliberately a hard minimum and not the blended one: this is the one place a
    // crease is wanted, because the crease *is* the join between a wall and the
    // floor it stands on.
    const float below = RoomAt({world.x, world.y + layer.rubble}, layer, cutoff, seed, height);

    return std::min(here, below) * allowance;
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

// Pixels of rock the wall roughness adds at a position, positive or negative.
//
// Applied to the finished distance rather than to any one layer, so one field
// roughens every wall in the world at the same scale — which is what a rock type
// does. Faded out away from a surface, because the term is added to the whole
// field and a roughness that reached everywhere would leave bubbles of air deep
// in the rock and pillars of rock in the middle of the air.
float Roughness(Vector2 world, float solid, const RoughnessSettings &r, int seed, float allowance) {
    if (r.amplitude <= 0.0f || allowance <= 0.0f) return 0.0f;

    const float near = 1.0f - SmoothStep(0.0f, std::max(r.reach, 1e-3f), std::abs(solid));
    if (near <= 0.0f) return 0.0f;

    // Folded, so the field creases where it crosses zero and domes where it peaks.
    // A smooth field added to a wall would only make the wall a slightly different
    // smooth wall; it is the creases that read as broken rock.
    const float fold = std::abs(Signed(world, Reseed(r.shape, seed))) - r.bias;

    return fold * r.amplitude * near * allowance;
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

    caves.calibration.chamber =
        Quantile(Reseed(caves.chambers.shape, settings.seed), caves.chambers.coverage, centre, kSampledWidth,
                 kSampledDepth);

    caves.calibration.cavern =
        Quantile(Reseed(caves.caverns.shape, settings.seed), caves.caverns.coverage, centre, kSampledWidth,
                 kSampledDepth);
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
float SolidityBelow(Vector2 world, float depth, const Settings &s) {
    const CaveSettings &caves = s.caves;

    // Above the ground there is nothing to carve out of, and the cave layers are
    // what this function spends its time on. Every position in the open sky
    // leaves here having sampled the surface alone.
    if (depth <= 0.0f) return depth;

    float solid = depth;

    // Layers are unioned with a blend rather than with a bare minimum, so a
    // junction opens out instead of leaving a knife edge of rock in each quadrant
    // of it. See SmoothMin.
    const float blend = caves.blend;

    const auto carve = [&](float into) {
        // A layer that is switched off here reports itself unreachably distant,
        // and blending against that number would lose the answer to it: the
        // subtraction inside SmoothMin has no bits left for a distance of a few
        // pixels once one side is a thousand million of them.
        if (into < -kFar * 0.5f) return;

        solid = SmoothMin(solid, -into, blend);
    };

    const float crust = CrustAllowance(depth, caves);

    // How honeycombed this stretch of the underground is, which is what leaves
    // some of it near-solid rock and the rest cave country.
    const float country = RegionAllowance(world, depth, caves, s.seed);

    // Entrances first, since they are the only layer that reaches into the crust.
    const float reach = ShaftAllowance(depth, caves);

    if (reach > 0.0f) {
        // Gated by the region where it would break daylight, and not at all below
        // that.
        //
        // The layer has two jobs and they want opposite densities. A hole in the
        // ground should lead somewhere, and the region field is where the
        // somewheres are — ungated, most openings led into dead rock and there was
        // one every two hundred and seventy pixels, which is a landscape with its
        // lid off rather than a find. A few pixels down it stops being an entrance
        // and becomes the only thing in the world carrying a route from one depth
        // to the next, and that has to be everywhere or the deep is sealed.
        // Read well below the opening rather than at it, and that is the whole
        // sense of the gate. What decides whether a hole in the ground is worth
        // being there is not the rock it is cut in but whether there is anything
        // under it, and cave country is scarce at the top by design — asked at the
        // surface the question is almost always answered no, and the entrances
        // close over an underground that is perfectly well connected to itself and
        // sealed from the sky.
        const float under = RegionAllowance({world.x, world.y + caves.regionDeepens},
                                            depth + caves.regionDeepens, caves, s.seed);

        // Cubed, so that an opening appears over the heart of a system and not
        // over its edge. The region field fades over `regionFade`, and read
        // straight that fade is wide enough that a shaft crossing the very margin
        // of one still cracks the surface — which put an entrance every three
        // hundred pixels, most of them onto a passage already pinching out.
        const float heart = under * under * under;

        const float floor   = std::clamp(caves.shafts.floor, 0.0f, 1.0f);
        const float visible = 1.0f - SmoothStep(0.0f, std::max(caves.mouthDepth, 1e-3f), depth);
        const float mouth   = 1.0f - visible * (1.0f - (floor + (1.0f - floor) * heart));

        // Held back by a squeeze and not by an allowance, so that a shaft the gate
        // turns down is closed outright rather than reduced to a slit. Scaled
        // past one, since the swing can still open the corridor a little and the
        // gate has to beat it: what is wanted is an entrance or no entrance, never
        // a crack too narrow to enter that nonetheless lets the daylight in.
        constexpr float kMouthSqueeze = 1.6f;

        carve(Tunnel(world, caves.shafts, s.seed,
                     TunnelWidth(world, caves.shafts, s.seed, depth, reach, (1.0f - mouth) * kMouthSqueeze)));
    }

    if (crust <= 0.0f) return solid;

    // A layer with a floor narrows towards solid rock rather than closing, so
    // wherever the player breaks in there is still somewhere to go.
    const auto allowance = [&](const TunnelLayer &layer) {
        const float floor = std::clamp(layer.floor, 0.0f, 1.0f);

        return crust * (floor + (1.0f - floor) * country);
    };

    carve(Tunnel(world, caves.crawlways, s.seed,
                 TunnelWidth(world, caves.crawlways, s.seed, depth, allowance(caves.crawlways))));

    carve(Tunnel(world, caves.galleries, s.seed,
                 TunnelWidth(world, caves.galleries, s.seed, depth, allowance(caves.galleries))));

    // The rooms follow the region outright: a room in dead rock is a bubble, and
    // what makes a room worth arriving in is the corridors that lead to it.
    const float region = crust * country;

    if (region > 0.0f) {
        carve(Chamber(world, caves.chambers, caves.calibration.chamber, s.seed, depth, region));
        carve(Chamber(world, caves.caverns, caves.calibration.cavern, s.seed, depth, region));
    }

    // And the wall, last, because what it roughens is the outline all of the
    // above agreed on rather than any one of them. Held to the crust allowance so
    // it cannot bite into the ground the player walks on.
    return solid + Roughness(world, solid, caves.roughness, s.seed, crust);
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
