#include "light.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace light {
namespace {

// Runs `body` over [0, count) across the machine's cores.
//
// The march is the one part of the solve worth splitting. Every ray is
// independent of every other, there are tens of thousands of them, and each
// writes only to its own sample. The merge reads across probes and costs a
// fraction as much, so it stays where it is.
template <typename Body>
void Parallel(int count, Body body) {
    const unsigned cores = std::thread::hardware_concurrency();
    const int workers    = static_cast<int>(std::min(std::max(cores, 1u), 16u));

    // Splitting a handful of columns costs more in threads than it saves in
    // work, and the top cascades have very few.
    if (workers <= 1 || count < workers * 2) {
        for (int i = 0; i < count; i++) body(i);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);

    // Interleaved rather than split into blocks. A ray that leaves the region
    // dies within a few steps, and those rays are all gathered along one edge
    // of it, so a block split would hand one worker every expensive ray and
    // leave the rest idle.
    for (int w = 1; w < workers; w++) {
        pool.emplace_back([&body, count, workers, w] {
            for (int i = w; i < count; i += workers) body(i);
        });
    }

    for (int i = 0; i < count; i += workers) body(i);

    for (std::thread &worker : pool) worker.join();
}

// A ray with less than this left is treated as stopped. Carrying it on costs
// the same as carrying a bright one and can no longer change the result.
constexpr float kMinTransmittance = 0.002f;

// Bilinear weights and the four cells a continuous grid coordinate falls
// between. The coordinate is in cells, measured from the centre of cell zero.
struct Bilinear {
    int i0 = 0;
    int j0 = 0;
    float fx = 0.0f;
    float fy = 0.0f;
};

Bilinear Weights(float u, float v) {
    Bilinear at;

    at.i0 = static_cast<int>(std::floor(u));
    at.j0 = static_cast<int>(std::floor(v));
    at.fx = u - static_cast<float>(at.i0);
    at.fy = v - static_cast<float>(at.j0);

    return at;
}

int Clamp(int value, int limit) {
    return std::clamp(value, 0, limit - 1);
}

} // namespace

void Medium::Resize(int newCols, int newRows) {
    cols = newCols;
    rows = newRows;

    extinction.assign(static_cast<std::size_t>(cols) * rows, 0.0f);
    emission.assign(static_cast<std::size_t>(cols) * rows, Radiance{});
    skyline.assign(static_cast<std::size_t>(cols), 0.0f);
    cover.assign(static_cast<std::size_t>(cols), 0.0f);
}

void Medium::Clear() {
    std::fill(extinction.begin(), extinction.end(), 0.0f);
    std::fill(emission.begin(), emission.end(), Radiance{});
}

Rectangle Medium::Bounds() const {
    return {origin.x - spacing * 0.5f, origin.y - spacing * 0.5f, cols * spacing, rows * spacing};
}

float Field::IntervalStart(int level) const {
    // Intervals are laid end to end and each is four times the last, so the
    // start of one is the sum of everything below it: base * (4^level - 1) / 3.
    const float base = settings_.intervalScale * spacing_;

    return base * static_cast<float>((1 << (2 * level)) - 1) / 3.0f;
}

void Field::BuildPyramid(const Medium &medium) {
    pyramid_.cols    = medium.cols;
    pyramid_.rows    = medium.rows;
    pyramid_.spacing = medium.spacing;
    pyramid_.origin  = medium.origin;

    // One level per cascade, since each cascade reads the level whose cells are
    // the size of its own step. A level whose cells outgrow the region is of no
    // use to anyone, so the stack stops there.
    int levels = 1;
    while (levels < settings_.cascades && ((medium.cols >> levels) > 1 || (medium.rows >> levels) > 1)) {
        levels++;
    }

    pyramid_.levels = levels;
    pyramid_.extinction.resize(levels);
    pyramid_.emission.resize(levels);

    pyramid_.extinction[0] = medium.extinction;
    pyramid_.emission[0]   = medium.emission;

    for (int level = 1; level < levels; level++) {
        const int cols = std::max(1, (medium.cols + (1 << level) - 1) >> level);
        const int rows = std::max(1, (medium.rows + (1 << level) - 1) >> level);

        const int fineCols = std::max(1, (medium.cols + (1 << (level - 1)) - 1) >> (level - 1));
        const int fineRows = std::max(1, (medium.rows + (1 << (level - 1)) - 1) >> (level - 1));

        std::vector<float> &extinction = pyramid_.extinction[level];
        std::vector<Radiance> &emission = pyramid_.emission[level];

        extinction.assign(static_cast<std::size_t>(cols) * rows, 0.0f);
        emission.assign(static_cast<std::size_t>(cols) * rows, Radiance{});

        const std::vector<float> &fineExtinction = pyramid_.extinction[level - 1];
        const std::vector<Radiance> &fineEmission = pyramid_.emission[level - 1];

        for (int i = 0; i < cols; i++) {
            for (int j = 0; j < rows; j++) {
                float stopped = 0.0f;
                float average = 0.0f;
                Radiance given;
                int counted = 0;


                for (int di = 0; di < 2; di++) {
                    for (int dj = 0; dj < 2; dj++) {
                        const int fi = i * 2 + di;
                        const int fj = j * 2 + dj;

                        if (fi >= fineCols || fj >= fineRows) continue;

                        const int cell = fi * fineRows + fj;

                        stopped = std::max(stopped, fineExtinction[cell]);
                        average += fineExtinction[cell];
                        given = given + fineEmission[cell];
                        counted++;
                    }
                }

                if (counted == 0) continue;

                // Occlusion takes the strongest of the four, emission the
                // average of them.
                //
                // Averaging the occlusion is the tempting choice, and it is
                // what lets light through walls. A coarse cell straddling a
                // surface comes out half solid, a long step reads it as letting
                // half the light past, and near any surface every step is such
                // a cell. What that draws is a cone of daylight spreading down
                // through the ground beneath anything standing on it. Taking
                // the strongest instead means a cell with any wall in it stops
                // the ray, so a wall is a wall at every scale it is looked at.
                //
                // The cost is that distant shadows come out a shade too dark,
                // since a coarse cell barely clipped by a corner blocks as much
                // as a solid one. That is the right way round to be wrong.
                const float share = 1.0f / static_cast<float>(counted);

                extinction[i * rows + j] = stopped * settings_.coarseOcclusion + average * share * (1.0f - settings_.coarseOcclusion);
                emission[i * rows + j]   = given * share;
            }
        }
    }
}

bool Field::Inside(Vector2 world) const {
    const float half = pyramid_.spacing * 0.5f;

    return world.x >= pyramid_.origin.x - half && world.y >= pyramid_.origin.y - half &&
           world.x < pyramid_.origin.x - half + pyramid_.cols * pyramid_.spacing &&
           world.y < pyramid_.origin.y - half + pyramid_.rows * pyramid_.spacing;
}

int Field::MipIndex(float world, float origin, float spacing, int level) const {
    // The finest cell is the square of one spacing centred on its own vertex,
    // so the index is a rounding rather than a truncation. Coarser levels are
    // whole groups of those.
    const int fine = static_cast<int>(std::floor((world - origin) / spacing + 0.5f));

    return (fine >= 0) ? (fine >> level) : -1;
}

float Field::ExtinctionAt(int level, Vector2 world) const {
    const int cols = std::max(1, (pyramid_.cols + (1 << level) - 1) >> level);
    const int rows = std::max(1, (pyramid_.rows + (1 << level) - 1) >> level);

    const int i = MipIndex(world.x, pyramid_.origin.x, pyramid_.spacing, level);
    const int j = MipIndex(world.y, pyramid_.origin.y, pyramid_.spacing, level);

    if (i < 0 || j < 0 || i >= cols || j >= rows) return 0.0f;

    return pyramid_.extinction[level][i * rows + j];
}

Radiance Field::EmissionAt(int level, Vector2 world) const {
    const int cols = std::max(1, (pyramid_.cols + (1 << level) - 1) >> level);
    const int rows = std::max(1, (pyramid_.rows + (1 << level) - 1) >> level);

    const int i = MipIndex(world.x, pyramid_.origin.x, pyramid_.spacing, level);
    const int j = MipIndex(world.y, pyramid_.origin.y, pyramid_.spacing, level);

    if (i < 0 || j < 0 || i >= cols || j >= rows) return {};

    return pyramid_.emission[level][i * rows + j];
}

Radiance Field::SkyAt(Vector2 world) const {
    const Sky &sky = settings_.sky;

    // Faded over a band rather than cut at one height, so a ray that ends just
    // under the horizon is dimmer than one that ends above it instead of the
    // two differing by everything.
    const float share = std::clamp((sky.horizon + sky.fade - world.y) / std::max(sky.fade, 1.0f), 0.0f, 1.0f);

    // Then dimmed by whatever stands over the column without stopping the sky
    // outright. Applied here, at the end of a ray that reached the sky, so a cloud
    // shades the ground beneath itself and nothing else: the shadow lands where the
    // rays that would have been sunlight were going.
    // And only below the cloud that casts it. A ray ending in the open sky, or
    // inside the cloud itself, has not passed under anything.
    const float under = std::clamp((world.y - sky.coverBelow) / std::max(sky.coverFade, 1.0f), 0.0f, 1.0f);

    return sky.radiance * (share * (1.0f - CoverAt(world.x) * under));
}

float Field::CoverAt(float worldX) const {
    if (cover_.empty()) return 0.0f;

    const int column = std::clamp(static_cast<int>(std::floor((worldX - pyramid_.origin.x) / pyramid_.spacing + 0.5f)),
                                  0, static_cast<int>(cover_.size()) - 1);

    return cover_[column];
}

float Field::SkylineAt(float worldX) const {
    if (skyline_.empty()) return -kUnreachable;

    const int column =
        std::clamp(static_cast<int>(std::floor((worldX - pyramid_.origin.x) / pyramid_.spacing + 0.5f)), 0,
                   static_cast<int>(skyline_.size()) - 1);

    return skyline_[column];
}

bool Field::ReachesSky(Vector2 from, Vector2 to) const {
    if (skyline_.empty()) return true;

    // A handful of samples along the ray. The skyline is a smooth outline at
    // this scale, and testing every cell of a ray a thousand pixels long would
    // cost more than marching it did.
    constexpr int kSamples = 8;

    for (int s = 1; s <= kSamples; s++) {
        const float t = static_cast<float>(s) / static_cast<float>(kSamples);

        const Vector2 at = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};

        if (at.y > SkylineAt(at.x)) return false;
    }

    return true;
}

void Field::March(int level) {
    Cascade &cascade = cascades_[level];

    const float from = IntervalStart(level);
    const float to   = IntervalStart(level + 1);

    // The step matches the cascade's own scale, and the medium is read from the
    // pyramid level whose cells are that size. Ray length quadruples per
    // cascade while the step only doubles, so a cascade costs twice the one
    // below it rather than four times, and the whole stack stays affordable.
    const int mip    = std::min(level, pyramid_.levels - 1);
    const float step = pyramid_.spacing * static_cast<float>(1 << mip);

    // One step covers one cell of the level being read, so a cell's worth of
    // emission is exactly what a step collects, and its extinction applies once.
    const float perStep = static_cast<float>(1 << mip);

    const float turn = 2.0f * PI / static_cast<float>(cascade.directions);

    const bool top = (level + 1 == static_cast<int>(cascades_.size()));

    Parallel(cascade.cols, [&](int i) {
        for (int j = 0; j < cascade.rows; j++) {
            // Each cascade has its own probe spacing, so the position cannot
            // come from the base grid's accessor.
            const Vector2 probe = {origin_.x + (static_cast<float>(i) + 0.5f) * cascade.spacing,
                                   origin_.y + (static_cast<float>(j) + 0.5f) * cascade.spacing};

            for (int d = 0; d < cascade.directions; d++) {
                const float angle = (static_cast<float>(d) + 0.5f) * turn;
                const Vector2 heading = {std::cos(angle), std::sin(angle)};

                Radiance gathered;
                float transmittance = 1.0f;

                // Where the ray would have got to, whether or not the region
                // reached that far.
                //
                // It has to be the end of the interval and never the point
                // where the ray left the region. That edge is a screen's width
                // from the camera and moves with it, so a sky credited there
                // changes every time the player takes a step, and the whole
                // world flickers in time with the walking.
                const Vector2 end = {probe.x + heading.x * to, probe.y + heading.y * to};

                for (float t = from; t < to; t += step) {
                    const Vector2 at = {probe.x + heading.x * t, probe.y + heading.y * t};

                    // The region is a rectangle, so a ray that leaves it never
                    // comes back. Everything beyond it is empty and unlit, and
                    // stopping here is what keeps the far cascades cheap: most
                    // of their rays are pointed out of the region and end
                    // within a few steps.
                    if (!Inside(at)) break;

                    gathered = gathered + EmissionAt(mip, at) * (transmittance * perStep);

                    transmittance *= 1.0f - ExtinctionAt(mip, at);

                    if (transmittance <= kMinTransmittance) {
                        transmittance = 0.0f;
                        break;
                    }
                }

                // Only the topmost cascade sees the sky. It is the one whose
                // rays are still travelling when the stack runs out; adding it
                // lower down would count the same sky once per cascade.
                if (top && transmittance > 0.0f) {
                    const Vector2 end = {probe.x + heading.x * to, probe.y + heading.y * to};

                    gathered = gathered + SkyAt(end) * transmittance;
                }

                const int index = cascade.Index(i, j, d);

                cascade.radiance[index]      = gathered;
                cascade.transmittance[index] = transmittance;
            }
        }
    });

    rays_ += static_cast<long>(cascade.cols) * cascade.rows * cascade.directions;
}

void Field::Merge(int level) {
    Cascade &lower       = cascades_[level];
    const Cascade &upper = cascades_[level + 1];

    // Four upper directions span the angular range of one lower direction, and
    // their average points the same way. That correspondence is the whole
    // reason the two cascades can be added together at all.
    const int fan   = upper.directions / lower.directions;
    const float mix = 1.0f / static_cast<float>(fan);

    // Split the same way as the march. Every lower sample is written once and
    // reads only from the cascade above, which is finished, so the columns are
    // independent. The merge turned out to cost as much as the march does:
    // there are as many samples in it, and each one reads sixteen.
    Parallel(lower.cols, [&](int i) {
        for (int j = 0; j < lower.rows; j++) {
            // Where this probe falls among the upper cascade's probes, which
            // are twice as far apart. Interpolating rather than taking the
            // nearest is what stops the coarse grid from showing through as
            // blocks in the light.
            const float u = ((static_cast<float>(i) + 0.5f) * lower.spacing) / upper.spacing - 0.5f;
            const float v = ((static_cast<float>(j) + 0.5f) * lower.spacing) / upper.spacing - 0.5f;

            const Bilinear at = Weights(u, v);

            const int i0 = Clamp(at.i0, upper.cols);
            const int i1 = Clamp(at.i0 + 1, upper.cols);
            const int j0 = Clamp(at.j0, upper.rows);
            const int j1 = Clamp(at.j0 + 1, upper.rows);

            const float w00 = (1.0f - at.fx) * (1.0f - at.fy);
            const float w10 = at.fx * (1.0f - at.fy);
            const float w01 = (1.0f - at.fx) * at.fy;
            const float w11 = at.fx * at.fy;

            for (int d = 0; d < lower.directions; d++) {
                Radiance beyond;

                for (int m = 0; m < fan; m++) {
                    const int up = d * fan + m;

                    beyond = beyond + upper.radiance[upper.Index(i0, j0, up)] * w00 +
                             upper.radiance[upper.Index(i1, j0, up)] * w10 +
                             upper.radiance[upper.Index(i0, j1, up)] * w01 +
                             upper.radiance[upper.Index(i1, j1, up)] * w11;
                }

                const int index = lower.Index(i, j, d);

                // Weighted by what the near interval let through. Light from
                // further away reaches the probe only through whatever did not
                // stop it on the way.
                lower.radiance[index] = lower.radiance[index] + beyond * (mix * lower.transmittance[index]);
            }
        }
    });
}

void Field::Spread() {
    const float reach = settings_.surfaceReach;
    if (reach <= 0.0f) return;

    const std::size_t count = probes_.size();

    distance_.assign(count, kUnreachable);
    previous_.assign(count, Radiance{});

    // Open space is its own answer, and the source everything solid draws from.
    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            const int cell = i * rows_ + j;
            if (solid_[cell] > 0.0f) continue;

            distance_[cell] = 0.0f;
            previous_[cell] = probes_[cell];
        }
    }

    const float straight = spacing_;
    const float diagonal = spacing_ * 1.41421356f;

    // Knight's moves as well as the eight around.
    //
    // Eight neighbours can only measure distance along an axis or a diagonal,
    // and everything between is overstated by up to a twenty-fifth. That error
    // is a function of direction alone, so it does not average away: it draws a
    // fixed star of dimmer wedges radiating from every light through the rock
    // around it. The longer step measures the angles between, and takes the
    // star out.
    const float knight = spacing_ * 2.23606798f;

    // The nearest open space wins, not the brightest.
    //
    // That distinction is the whole of this pass. Carrying the brightest
    // instead lets sunlight cross a wall: a slab of rock lit from above hands
    // its light down through itself, and the ceiling of a dark cave beneath
    // comes out brighter than the cave. Light does not travel through rock, and
    // a face does not show light that arrived from behind it.
    //
    // Nearest is also the right thing to be showing. What a rock face is lit by
    // is the space in front of it, and the space in front of it is the open
    // space it is nearest to.
    const auto take = [&](int cell, int ni, int nj, float cost) {
        if (ni < 0 || nj < 0 || ni >= cols_ || nj >= rows_) return;

        const int neighbour = ni * rows_ + nj;
        const float reached = distance_[neighbour] + cost;

        if (reached >= distance_[cell]) return;

        distance_[cell] = reached;
        previous_[cell] = previous_[neighbour];
    };

    // Two sweeps, rather than one pass per probe of depth. A forward sweep and
    // a backward one carry a distance any distance at all in one go, which is
    // the usual way a distance transform is done and is exactly what this is.
    const auto sweep = [&](int i, int j, int step) {
        const int cell = i * rows_ + j;
        if (solid_[cell] <= 0.0f) return;

        take(cell, i - step, j, straight);
        take(cell, i, j - step, straight);
        take(cell, i - step, j - step, diagonal);
        take(cell, i + step, j - step, diagonal);

        take(cell, i - step, j - 2 * step, knight);
        take(cell, i + step, j - 2 * step, knight);
        take(cell, i - 2 * step, j - step, knight);
        take(cell, i + 2 * step, j - step, knight);
    };

    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) sweep(i, j, 1);
    }

    for (int i = cols_ - 1; i >= 0; i--) {
        for (int j = rows_ - 1; j >= 0; j--) sweep(i, j, -1);
    }

    // Then the light of whatever was nearest, dimmed by how far it had to come.
    // Never more than the space it faces was itself given, which is what keeps a
    // wall from outshining the cave it stands in.
    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            const int cell = i * rows_ + j;
            if (solid_[cell] <= 0.0f) continue;

            // Falls to nothing at `reach` and stays there, rather than trailing
            // off for ever.
            //
            // An exponential is the tempting curve and it is what let light
            // through the ground. It never reaches zero, and radiance has no
            // ceiling, so a bright enough lamp stays visible however far its
            // light is decayed: under a lantern the rock below it came out as a
            // cone of daylight widening downward, and turning the lamp up made
            // the cone longer. A curve that ends means rock further than this
            // from open space is black whatever is shining on the other side.
            // The lip comes off the distance first, so the face of a material
            // shows the light in front of it rather than a discounted copy of
            // it. See Settings::surfaceLip.
            const float into = std::max(0.0f, distance_[cell] - settings_.surfaceLip);

            const float share = std::max(0.0f, 1.0f - into / reach);

            probes_[cell] = previous_[cell] * (share * share);
        }
    }

    // One averaging pass to take the seams out.
    //
    // A distance transform keeps only the nearest source, and neighbouring
    // columns of a hillside settle on different ones. The two answers differ by
    // little, but the switch between them is abrupt, and abrupt is what the eye
    // finds: the crust comes out ruled with faint vertical bands.
    //
    // Averaged over solid neighbours alone. Reaching into open space would pull
    // a wall back up towards the cave beside it and undo the rule above.
    previous_ = probes_;

    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            const int cell = i * rows_ + j;
            if (solid_[cell] <= 0.0f) continue;

            Radiance total;
            float counted = 0.0f;

            for (int di = -1; di <= 1; di++) {
                for (int dj = -1; dj <= 1; dj++) {
                    const int ni = i + di;
                    const int nj = j + dj;

                    if (ni < 0 || nj < 0 || ni >= cols_ || nj >= rows_) continue;

                    const int neighbour = ni * rows_ + nj;
                    if (solid_[neighbour] <= 0.0f) continue;

                    total = total + previous_[neighbour];
                    counted += 1.0f;
                }
            }

            if (counted > 0.0f) probes_[cell] = total * (1.0f / counted);
        }
    }
}

void Field::Gather() {
    const Cascade &base = cascades_[0];

    cols_ = base.cols;
    rows_ = base.rows;

    probes_.assign(static_cast<std::size_t>(cols_) * rows_, Radiance{});
    solid_.assign(static_cast<std::size_t>(cols_) * rows_, 0.0f);

    // How much of each probe's own patch is filled. A probe covers a square
    // block of medium cells, and which of them are wall is what decides whether
    // this probe is a surface waiting for light or open space that already has
    // its answer.
    const int cover = std::max(1, settings_.probeCells);

    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            float filled = 0.0f;
            int counted  = 0;

            for (int di = 0; di < cover; di++) {
                for (int dj = 0; dj < cover; dj++) {
                    const int mi = i * cover + di;
                    const int mj = j * cover + dj;

                    if (mi >= pyramid_.cols || mj >= pyramid_.rows) continue;

                    filled += pyramid_.extinction[0][mi * pyramid_.rows + mj];
                    counted++;
                }
            }

            if (counted > 0) solid_[i * rows_ + j] = filled / static_cast<float>(counted);
        }
    }

    // Averaged over directions rather than summed: what a surface or a rule
    // asks for is how much light is here, not how much came down each of four
    // arbitrary bearings, and the count of those is an implementation detail
    // that changes between cascades.
    const float share = 1.0f / static_cast<float>(base.directions);

    for (int i = 0; i < cols_; i++) {
        for (int j = 0; j < rows_; j++) {
            Radiance total;

            for (int d = 0; d < base.directions; d++) {
                total = total + base.radiance[base.Index(i, j, d)];
            }

            probes_[i * rows_ + j] = total * share;
        }
    }
}

void Field::Solve(const Medium &medium, const Settings &settings) {
    settings_ = settings;
    rays_     = 0;

    if (medium.cols <= 0 || medium.rows <= 0) {
        cols_ = 0;
        rows_ = 0;
        probes_.clear();
        return;
    }

    spacing_ = medium.spacing * static_cast<float>(std::max(1, settings.probeCells));

    // Anchored to the corner of the region the cells cover, so that a probe
    // sits at the centre of its own cell. The grid can then be handed straight
    // to a texture, whose samples are at cell centres too.
    origin_ = {medium.origin.x - medium.spacing * 0.5f, medium.origin.y - medium.spacing * 0.5f};

    BuildPyramid(medium);

    skyline_ = medium.skyline;
    cover_   = medium.cover;

    const Rectangle bounds = medium.Bounds();
    const int count        = std::max(1, settings.cascades);

    cascades_.resize(count);

    for (int level = count - 1; level >= 0; level--) {
        Cascade &cascade = cascades_[level];

        cascade.spacing    = spacing_ * static_cast<float>(1 << level);
        cascade.cols       = std::max(1, static_cast<int>(std::ceil(bounds.width / cascade.spacing)));
        cascade.rows       = std::max(1, static_cast<int>(std::ceil(bounds.height / cascade.spacing)));
        cascade.directions = std::max(1, settings.baseDirections) << (2 * level);

        const auto samples = static_cast<std::size_t>(cascade.cols) * cascade.rows * cascade.directions;

        cascade.radiance.assign(samples, Radiance{});
        cascade.transmittance.assign(samples, 0.0f);

        March(level);

        if (level + 1 < count) Merge(level);
    }

    Gather();
    Spread();
}

Vector2 Field::ProbePosition(int i, int j) const {
    return {origin_.x + (static_cast<float>(i) + 0.5f) * spacing_,
            origin_.y + (static_cast<float>(j) + 0.5f) * spacing_};
}

Radiance Field::ProbeAt(int i, int j) const {
    if (i < 0 || j < 0 || i >= cols_ || j >= rows_) return {};

    return probes_[i * rows_ + j];
}

Radiance Field::At(Vector2 world) const {
    if (cols_ <= 0 || rows_ <= 0) return {};

    const float u = (world.x - origin_.x) / spacing_ - 0.5f;
    const float v = (world.y - origin_.y) / spacing_ - 0.5f;

    const Bilinear at = Weights(u, v);

    const int i0 = Clamp(at.i0, cols_);
    const int i1 = Clamp(at.i0 + 1, cols_);
    const int j0 = Clamp(at.j0, rows_);
    const int j1 = Clamp(at.j0 + 1, rows_);

    return ProbeAt(i0, j0) * ((1.0f - at.fx) * (1.0f - at.fy)) + ProbeAt(i1, j0) * (at.fx * (1.0f - at.fy)) +
           ProbeAt(i0, j1) * ((1.0f - at.fx) * at.fy) + ProbeAt(i1, j1) * (at.fx * at.fy);
}

float Field::LevelAt(Vector2 world) const {
    return Expose(Luminance(At(world)), settings_.exposure);
}

} // namespace light
