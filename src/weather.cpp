#include "weather.h"

#include "config.h"
#include "grid.h"
#include "marching_squares.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace weather {
namespace {

constexpr float kPi = 3.14159265f;

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A field sampled as a share, so out of range is out of range whatever the noise
// did. terrain::Sample overshoots [0,1] on its rarest samples by design.
float Share(Vector2 world, const terrain::NoiseShape &shape) {
    return std::clamp(terrain::Sample(world, shape), 0.0f, 1.0f);
}

// Signed, in [-1,1], for a term that has to push a value both ways.
float Swing(float share) {
    return (share - 0.5f) * 2.0f;
}

unsigned char ToByte(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

Color Scale(Color c, float factor) {
    auto channel = [factor](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(v * factor, 0.0f, 255.0f));
    };

    return {channel(c.r), channel(c.g), channel(c.b), c.a};
}

Color Mix(Color a, Color b, float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);

    return {static_cast<unsigned char>(a.r + (b.r - a.r) * s),
            static_cast<unsigned char>(a.g + (b.g - a.g) * s),
            static_cast<unsigned char>(a.b + (b.b - a.b) * s), 255};
}

// Deterministic value in [0,1) from an integer. Rain has thousands of drops and no
// two of them may sit in the same place, but nothing about a drop is worth
// remembering between frames, so each one is hashed out of its own index instead
// of being stored.
float Hash(int value) {
    auto bits = static_cast<unsigned int>(value) * 2654435761u;
    bits ^= bits >> 15;
    bits *= 2246822519u;
    bits ^= bits >> 13;

    return static_cast<float>(bits & 0xffffffu) / static_cast<float>(0x1000000u);
}

} // namespace

float Sky::Field(Vector2 world) const {
    // Billow rather than plain fbm. Folding each octave puts a crease where the
    // field crosses zero and a dome where it peaks, so the contour comes out as a
    // mass of bulges pressed together — a cauliflower, which is the shape of a
    // cumulus cloud at every scale it has.
    return terrain::Billow({world.x - time_ * settings_.wind, world.y}, settings_.shape);
}

void Sky::Configure(const Settings &settings, const terrain::Settings &terrain) {
    settings_ = settings;
    terrain_  = terrain;

    // The gradient is measured from the ground, so raising the land raises the
    // horizon with it and nothing has to be re-picked.
    settings_.air.horizon = terrain.surface.level;

    // Sampled a little over one feature apart, and by a different irrational
    // multiple on each axis. Perlin noise is exactly zero at every corner of its own
    // lattice, so a grid stepping a whole number of features reads the same corner
    // over and over and every sample comes back at the midpoint of the field.
    constexpr int kSamplesPerAxis = 128;
    constexpr float kStrideX      = 1.618f;
    constexpr float kStrideY      = 1.303f;

    const float aspect  = std::max(settings_.shape.aspect, 1e-3f);
    const float feature = terrain::kFeatureSpan / std::max(settings_.shape.frequency, 0.01f);

    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(kSamplesPerAxis) * kSamplesPerAxis);

    for (int i = 0; i < kSamplesPerAxis; i++) {
        for (int j = 0; j < kSamplesPerAxis; j++) {
            // Stretched on the horizontal axis exactly as the field is, so the
            // samples are one feature apart in the field's own terms rather than in
            // pixels.
            const Vector2 at = {(i - kSamplesPerAxis / 2) * feature * aspect * kStrideX,
                                (j - kSamplesPerAxis / 2) * feature * kStrideY};

            // Through Field, so what is measured is the field that will be drawn.
            values.push_back(Field(at));
        }
    }

    std::sort(values.begin(), values.end());

    // Step zero is a cutoff nothing clears, the last is one everything does, and
    // between them each step is the quantile leaving that share of the field above
    // it.
    for (int step = 0; step < kCutoffSteps; step++) {
        const float cover = static_cast<float>(step) / static_cast<float>(kCutoffSteps - 1);
        const auto index  = static_cast<std::size_t>((1.0f - cover) * (values.size() - 1));

        cutoff_[step] = values[index];
    }
}

float Sky::Cutoff(float cover) const {
    const float at   = std::clamp(cover, 0.0f, 1.0f) * static_cast<float>(kCutoffSteps - 1);
    const int step   = std::min(static_cast<int>(at), kCutoffSteps - 2);
    const float into = at - static_cast<float>(step);

    return cutoff_[step] + (cutoff_[step + 1] - cutoff_[step]) * into;
}

Column Sky::ColumnAt(float worldX) const {
    // The front, drifting past on its own and faster than it changes shape, so the
    // weather arrives, sits over the world a while, and moves on.
    const float front = Share({worldX - time_ * settings_.frontWind, 0.0f}, settings_.front);

    // The climate underneath, elevation included: humidityLift is what makes the
    // sky over a mountain cloudier than the sky over the plain beside it.
    const terrain::Climate climate = terrain::ClimateAt(worldX, terrain_);

    // Added rather than multiplied. Multiplying would let a dry region hold the
    // sky clear through any front that crossed it, and a wet one keep cloud
    // through every clear spell; adding lets each shift the other's answer without
    // being able to overrule it.
    Column column;

    column.cover = std::clamp(settings_.cover + Swing(front) * settings_.frontInfluence
                                  + Swing(climate.humidity) * settings_.humidityInfluence,
                              0.0f, 1.0f);

    column.cutoff = Cutoff(column.cover);

    // How much cloud actually stands here, measured rather than assumed.
    //
    // Everything above this line decides how much cloud the region gets; from here
    // down it is the cloud itself being asked. Rain used to be read straight off
    // `cover`, which meant a whole stretch of world rained at once whether or not
    // there was anything overhead to rain out of.
    constexpr int kSteps = 12;

    const float span = (settings_.base + settings_.rainDrop) - settings_.ceiling;

    if (span > 0.0f && column.cover > 0.0f) {
        float thickness = 0.0f;

        for (int step = 0; step < kSteps; step++) {
            const float through = (static_cast<float>(step) + 0.5f) / static_cast<float>(kSteps);

            thickness += DensityAt({worldX, settings_.ceiling + through * span}, column);
        }

        column.weight = std::clamp(thickness / static_cast<float>(kSteps) / std::max(settings_.weightFull, 0.01f),
                                   0.0f, 1.0f);
    }

    // And rain out of that. A cloud heavy enough drops what it carries; the one
    // beside it, in the same weather, does not.
    column.rain = SmoothStep(settings_.rainAt, settings_.rainFull, column.weight);

    return column;
}

float Sky::MarginAt(Vector2 world, const Column &column) const {
    if (column.cover <= 0.0f) return -1.0f;

    // The underside hangs lower under heavy weather, which is the visible half of a
    // rain cloud sitting closer to the ground than a fair-weather one.
    //
    // Driven by the region's cover rather than by the rain, and it has to be: the
    // rain is now measured by marching down this very band, so a band that moved
    // with the rain would be defining the thing that defines it. The height of a
    // cloud base is a property of the air mass anyway, which is regional.
    const float base = settings_.base + settings_.rainDrop * column.cover;
    const float span = base - settings_.ceiling;
    if (span <= 0.0f) return -1.0f;

    const float through = (world.y - settings_.ceiling) / span;
    if (through <= 0.0f || through >= 1.0f) return -1.0f;

    // Nothing at the top and bottom of the band, thickest through the middle. A
    // cloud with a flat top and a flat bottom reads as a slab of ceiling rather
    // than as weather.
    const float taper = (1.0f - std::sin(through * kPi)) * settings_.bandTaper;

    return Field(world) - column.cutoff - taper;
}

float Sky::DensityAt(Vector2 world, const Column &column) const {
    return std::clamp(MarginAt(world, column) / std::max(settings_.softness, 1e-3f), 0.0f, 1.0f);
}

float Sky::CoverAt(float worldX) const {
    const Column column = ColumnAt(worldX);

    // The thickest cloud anywhere down the column, rather than the cloud at one
    // chosen height.
    //
    // Reading a single height was wrong in a way that was obvious the moment the
    // shadows were drawn next to the clouds casting them: whether a cloud happened
    // to cross that exact line had nothing to do with whether it was there, so the
    // ground had shadows under clear sky and clouds with no shadow beneath them.
    //
    // The maximum rather than the average, because the band is thin enough that any
    // cloud standing in it is effectively opaque. A second layer of cloud behind the
    // first does not make the ground darker.
    constexpr int kSteps = 12;

    const float span = (settings_.base + settings_.rainDrop * column.rain) - settings_.ceiling;
    if (span <= 0.0f) return 0.0f;

    float thickest = 0.0f;

    for (int step = 0; step < kSteps; step++) {
        const float through = (static_cast<float>(step) + 0.5f) / static_cast<float>(kSteps);

        thickest = std::max(thickest, DensityAt({worldX, settings_.ceiling + through * span}, column));
    }

    return thickest;
}

float Sky::ShadeAt(float worldX) const {
    return CoverAt(worldX) * std::clamp(settings_.shade, 0.0f, 1.0f);
}

float Sky::RainAt(float worldX) const {
    return ColumnAt(worldX).rain;
}

Color Sky::AirAt(float worldY, float cover) const {
    const Atmosphere &air = settings_.air;

    // Altitude above the horizon. Y grows downward, so the subtraction is this way
    // round, and below the horizon there is no more air to add.
    const float altitude = std::max(air.horizon - worldY, 0.0f);

    // How much air stands in the line of sight from here. It thins exponentially
    // with height, and this one number is the entire gradient.
    const float airmass = air.thickness * std::exp(-altitude / std::max(air.scaleHeight, 1.0f));

    // What that air scatters, channel by channel. Blue scatters some five times
    // more strongly than red, so thin air passes blue alone and thick air scatters
    // everything until it is white. The pale horizon, the blue overhead and the
    // dark above it are all this expression — none of the three is written down
    // anywhere.
    const Color lit = {ToByte(1.0f - std::exp(-air.rayleigh.x * airmass)),
                       ToByte(1.0f - std::exp(-air.rayleigh.y * airmass)),
                       ToByte(1.0f - std::exp(-air.rayleigh.z * airmass)), 255};

    // Then washed towards grey by the cloud standing over the column. What is being
    // looked at under a full sky is the underside of the cloud, not the air.
    return Mix(lit, air.overcast, cover);
}

void Sky::DrawAtmosphere(Rectangle view) const {
    const float band = std::max(settings_.air.bandHeight, 1.0f);

    // Snapped to the world, so the bands do not crawl up and down as the view
    // scrolls.
    const float top = std::floor(view.y / band) * band;

    // How overcast the region is, not whether a cloud stands over this exact spot.
    // Read once for the whole view, for two reasons: per column it would draw a comb
    // of vertical stripes across the sky behind the clouds, and per cloud it would
    // wash out the very background the cloud has to be seen against.
    const float cover = ColumnAt(view.x + view.width * 0.5f).cover
                      * std::clamp(settings_.air.overcastWash, 0.0f, 1.0f);

    for (float y = top; y < view.y + view.height; y += band) {
        DrawRectangleRec({view.x, y, view.width, band}, AirAt(y + band * 0.5f, cover));
    }
}

float Sky::LayerMargin(const Column &column, int index, int count) const {
    if (count <= 1 || index <= 0) return kCloudEdge;

    const Shading &shading = settings_.shading;

    // How far in this layer is, from the outer edge to the core.
    const float into = static_cast<float>(index) / static_cast<float>(count - 1);

    // Each layer covers a smaller share of the sky than the one under it, and the
    // margin it needs is the difference between the two measured cutoffs. Taking it
    // from the calibration rather than by stepping a fixed amount is what keeps the
    // bands evenly weighted whatever the field is doing: every layer is a stated
    // fraction of the cloud's area, not a stated distance up an unknown slope.
    //
    // A raining cloud packs them tighter, so more of its body falls in the dark
    // bands. That is the other half of a rain cloud being darker — it is deeper, so
    // less of it is near the light.
    const float pack   = 1.0f - shading.rainPack * column.rain;
    const float shrink = std::clamp(shading.coreShrink, 0.0f, 0.98f) * into * pack;

    return Cutoff(column.cover * (1.0f - shrink)) - column.cutoff;
}

Vector2 Sky::LayerShift(int index) const {
    const Shading &shading = settings_.shading;

    const float length = std::sqrt(shading.sun.x * shading.sun.x + shading.sun.y * shading.sun.y);
    if (length <= 0.0f) return {0.0f, 0.0f};

    const float reach = static_cast<float>(index) * shading.sunOffset;

    return {shading.sun.x / length * reach, shading.sun.y / length * reach};
}

Color Sky::LayerTint(const Column &column, int index, int count) const {
    const Shading &shading = settings_.shading;

    // Depth of this layer below the surface facing the sun, in layers. The core is
    // shifted furthest towards the light, so it is the shallowest; the outermost
    // ring is what is left showing on the far side, and is the deepest.
    const float depth = (count > 1) ? (1.0f - static_cast<float>(index) / static_cast<float>(count - 1)) : 0.0f;

    // Beer's law. This is the whole shading model, and it is why the bands crowd
    // together near the light and spread out into the shadow — a linear ramp would
    // space them evenly and read as a machined gradient rather than as a cloud.
    const float lit = std::exp(-shading.absorption * depth) * (1.0f - shading.rainDarken * column.rain);

    // Over the ambient the cloud is sitting in, never towards black. The shaded side
    // of a cloud is lit by the whole hemisphere above it, which is why it is blue
    // and not dark — and under a storm what changes is which ambient that is.
    const Color ambient = Mix(shading.ambient, shading.rainAmbient, std::clamp(column.rain, 0.0f, 1.0f));

    return Mix(ambient, shading.sunlight, lit);
}

void Sky::DrawClouds(Rectangle view, int spacing) const {
    // The band, clipped to the view. Underground there is no overlap at all and the
    // whole cost of the sky goes away without anything having to ask where the
    // player is.
    const float deepest = settings_.base + settings_.rainDrop;

    const float top    = std::max(view.y, settings_.ceiling);
    const float bottom = std::min(view.y + view.height, deepest);
    if (bottom <= top) return;

    const int layers = std::max(settings_.shading.layers, 1);
    const float step = static_cast<float>(std::max(spacing, 1));

    // Widened by the reach of the shift, so a layer moved towards the sun still has
    // field under it rather than being clipped along the edge of the grid.
    const float margin = std::abs(LayerShift(layers).x) + std::abs(LayerShift(layers).y) + step;

    const Vector2 origin = {std::floor((view.x - margin) / step) * step, std::floor((top - margin) / step) * step};

    const int cols = static_cast<int>(std::ceil((view.width + 2.0f * margin) / step)) + 2;
    const int rows = static_cast<int>(std::ceil((bottom - top + 2.0f * margin) / step)) + 2;

    Grid field(origin, cols, rows, static_cast<int>(step));

    // The same field held back by however much water each column is carrying.
    //
    // Two grids rather than one because they answer different questions. The plain
    // field draws the silhouette, which a heavy cloud must not lose any of; the
    // held-back one draws the shading inside it, and holding it back is what sinks a
    // laden column into the darker layers.
    Grid shaded(origin, cols, rows, static_cast<int>(step));

    // One pass over the field, and one column of weather per column of grid. The
    // layers are drawn from these same grids at rising thresholds, so the shading
    // costs rasterising and not sampling.
    std::vector<Column> columns(static_cast<std::size_t>(cols));

    for (int i = 0; i < cols; i++) {
        columns[i] = ColumnAt(origin.x + i * step);

        // How far down the stack this column's water pushes it, in the field's own
        // units: a share of the whole depth the layers span.
        const float sink = settings_.shading.weightSink * columns[i].weight
                         * LayerMargin(columns[i], layers - 1, layers);

        for (int j = 0; j < rows; j++) {
            const float margin = MarginAt(field.PointAt(i, j), columns[i]);

            field.SetValue(i, j, margin);
            shaded.SetValue(i, j, margin - sink);
        }
    }

    // The weather in the middle of the view decides the layer margins and colours.
    // They cannot be per-column: a layer is drawn as one shape across the whole
    // grid, and a threshold that changed along it would tear the bands apart.
    const Column middle = columns[static_cast<std::size_t>(cols / 2)];

    // Outermost first, each nested inside the last and shifted a little further
    // towards the sun, so what is left showing of each is a band of shading on the
    // side away from the light.
    for (int layer = 0; layer < layers; layer++) {
        const Vector2 shift = LayerShift(layer);

        Grid shifted(Vector2{origin.x + shift.x, origin.y + shift.y}, cols, rows, static_cast<int>(step));

        // Layer zero is the silhouette and takes the field as it is. Everything above
        // it is shading and takes the field held back by the column's own water.
        const Grid &source = (layer == 0) ? field : shaded;

        for (int i = 0; i < cols; i++) {
            for (int j = 0; j < rows; j++) shifted.SetValue(i, j, source.ValueAt(i, j));
        }

        marching_squares::DrawPixelated(shifted, LayerMargin(middle, layer, layers),
                                        LayerTint(middle, layer, layers), BLANK, config::kPixelSize);
    }
}

void Sky::DrawRain(Rectangle view) const {
    // Drops are placed by index across the view, so the same drop stays the same
    // drop as the camera moves and the rain does not reshuffle itself every frame.
    const float perDrop = terrain::kFeatureSpan / std::max(settings_.rainDensity, 1.0f);

    const int first = static_cast<int>(std::floor(view.x / perDrop));
    const int last  = static_cast<int>(std::ceil((view.x + view.width) / perDrop));

    for (int drop = first; drop <= last; drop++) {
        const float jitter = Hash(drop);
        const float x      = (drop + jitter) * perDrop;

        // One column read per drop. The rain has to know how hard it is falling
        // here, and whether the sky over this column is raining at all.
        const Column column = ColumnAt(x);
        if (column.rain <= 0.0f) continue;

        // Thinned out by how hard it is raining, using the drop's own second hash
        // rather than a count: a drizzle is the same drops falling, fewer of them.
        if (Hash(drop * 7919 + 13) > column.rain) continue;

        const float from = settings_.base + settings_.rainDrop * column.rain;
        const float span = std::max(settings_.rainSpan, 1.0f);

        // Position along the fall, from the time and the drop's own offset, over a
        // span that does not depend on the weather. The speed is therefore exactly
        // `rainSpeed`, always.
        const float phase  = Hash(drop * 104729 + 7);
        const float travel = std::fmod(phase * span + time_ * settings_.rainSpeed, span);

        const float head = from + travel;
        const float tail = std::max(head - settings_.rainLength, from);

        // Landed. Skipped rather than wrapped, so the drop keeps its own cadence and
        // the number of drops in the air over a column follows how far they have to
        // fall, which is what a deep valley in the rain looks like.
        if (head > terrain::Height(x, terrain_)) continue;

        if (head < view.y || tail > view.y + view.height) continue;

        // Coloured against the sky the drop is actually falling through, not against
        // a colour written down here.
        //
        // A fixed pale blue was fine while the background was flat white and became
        // invisible the moment the air behind it turned blue as well — the streak and
        // the sky were within a few values of each other. Taking the air at the
        // drop's own height and darkening it means the rain reads whatever the sky is
        // doing, which is also what rain looks like: seen against daylight it is a
        // darker streak, because what it does to the light behind it is get in the
        // way of it.
        const Color behind = AirAt(head, column.rain);
        const float weight = 0.42f + 0.28f * column.rain;

        const Color line = Mix(behind, settings_.rainLine, 0.35f);

        // Drawn as a bar a pixel-square wide and snapped to the same grid the world
        // is rasterised on, so the rain belongs to the picture instead of being a
        // hairline ruled over it.
        const float pixel = std::max(config::kPixelSize, 1.0f);
        const float left  = std::floor(x / pixel) * pixel;

        DrawRectangleRec({left, tail, pixel, head - tail}, Fade(Scale(line, 1.0f - weight), 0.85f));
    }
}

} // namespace weather
