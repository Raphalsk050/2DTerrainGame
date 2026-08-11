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

void Sky::Configure(const Settings &settings, const terrain::Settings &terrain) {
    settings_ = settings;
    terrain_  = terrain;

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

            values.push_back(Share(at, settings_.shape));
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

    // Rain from the regional cover, not from the cloud directly overhead. Rain that
    // switched off in the gap between two clouds would read as a fault, and what
    // actually decides whether it rains is how much water the air over this stretch
    // of world is carrying.
    column.rain = SmoothStep(settings_.rainAt, settings_.rainFull, column.cover);

    return column;
}

float Sky::DensityAt(Vector2 world, const Column &column) const {
    if (column.cover <= 0.0f) return 0.0f;

    // The underside hangs lower where it is raining, which is the visible half of a
    // rain cloud sitting closer to the ground than a fair-weather one.
    const float base = settings_.base + settings_.rainDrop * column.rain;
    const float span = base - settings_.ceiling;
    if (span <= 0.0f) return 0.0f;

    const float through = (world.y - settings_.ceiling) / span;
    if (through <= 0.0f || through >= 1.0f) return 0.0f;

    // Nothing at the top and bottom of the band, thickest through the middle. A
    // cloud with a flat top and a flat bottom reads as a slab of ceiling rather
    // than as weather.
    const float profile = std::sin(through * kPi);

    // The shape itself, on the wind.
    const float shape = Share({world.x - time_ * settings_.wind, world.y}, settings_.shape);

    // The measured cutoff for the share of sky this column is carrying, raised
    // towards the edges of the band. Because the cutoff was measured from the field
    // and the taper is zero through the middle, `cover` is the share of the sky that
    // is cloud there — the number means what it says.
    const float cutoff = Cutoff(column.cover) + (1.0f - profile) * settings_.bandTaper;

    return std::clamp((shape - cutoff) / std::max(settings_.softness, 1e-3f), 0.0f, 1.0f);
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

void Sky::DrawClouds(Rectangle view, int spacing) const {
    // The band, clipped to the view. Underground there is no overlap at all and the
    // whole cost of the sky goes away without anything having to ask where the
    // player is.
    const float deepest = settings_.base + settings_.rainDrop;

    const float top    = std::max(view.y, settings_.ceiling);
    const float bottom = std::min(view.y + view.height, deepest);
    if (bottom <= top) return;

    const float step = static_cast<float>(std::max(spacing, 1));

    // Snapped to the lattice, so the cloud's squares line up with the world's and
    // do not crawl as the view scrolls.
    const Vector2 origin = {std::floor(view.x / step) * step, std::floor(top / step) * step};

    const int cols = static_cast<int>(std::ceil(view.width / step)) + 2;
    const int rows = static_cast<int>(std::ceil((bottom - top) / step)) + 2;

    Grid cloud(origin, cols, rows, static_cast<int>(step));
    Grid raining(origin, cols, rows, static_cast<int>(step));

    for (int i = 0; i < cols; i++) {
        // Once per column, not once per sample. This is the expensive half.
        const Column column = ColumnAt(origin.x + i * step);

        for (int j = 0; j < rows; j++) {
            const float density = DensityAt(cloud.PointAt(i, j), column);

            cloud.SetValue(i, j, density);

            // The same field scaled by how hard it is raining, rather than masked
            // where it is not. Scaling shrinks the dark core smoothly into the lit
            // cloud around it; masking would end it on a ruled vertical line
            // running down the middle of a cloud.
            raining.SetValue(i, j, density * column.rain);
        }
    }

    // Two passes over the one field: every cloud in its lit colour, then the part of
    // it carrying rain in a darker one over the top.
    marching_squares::DrawPixelated(cloud, kCloudLevel, settings_.tint, BLANK, config::kPixelSize);
    marching_squares::DrawPixelated(raining, kCloudLevel, settings_.rainTint, BLANK, config::kPixelSize);
}

void Sky::DrawRain(Rectangle view) const {
    const float lowest = settings_.base + settings_.rainDrop;

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

        DrawLineEx({x, tail}, {x, head}, 1.0f, Fade(settings_.rainLine, 0.35f + 0.45f * column.rain));
    }
}

} // namespace weather
