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

Color Mix(Color a, Color b, float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);

    return {static_cast<unsigned char>(a.r + (b.r - a.r) * s + 0.5f),
            static_cast<unsigned char>(a.g + (b.g - a.g) * s + 0.5f),
            static_cast<unsigned char>(a.b + (b.b - a.b) * s + 0.5f), 255};
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

// Unit vector, or a fallback for a direction nobody set.
Vector2 Unit(Vector2 v, Vector2 fallback) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    if (length <= 1e-6f) return fallback;

    return {v.x / length, v.y / length};
}

// The surface a drop at a world position would land on.
//
// Nearest column rather than interpolated between two. The surface is a staircase
// of squares, not a slope: interpolating across the riser of a step would end the
// streak inside the block below it on one side and in the air above it on the
// other.
float GroundAt(const Ground &ground, float worldX, const terrain::Settings &terrain) {
    if (ground.top != nullptr && ground.count > 0) {
        const auto column = static_cast<int>(std::lround((worldX - ground.originX) / std::max(ground.spacing, 1e-3f)));

        if (column >= 0 && column < ground.count) return ground.top[column];
    }

    // Nothing prepared for this column. The shape of the land is the most that can
    // be said without asking the world, and it is what the rain landed on before
    // there was anything better to ask — but it costs eight samples of noise, so it
    // is reached only when a caller has left a gap rather than on every drop.
    return terrain::Height(worldX, terrain);
}

// A rain streak, drawn as a run of squares on the world's own lattice.
//
// Not a line, which is what it was. Nothing else in the scene is drawn off the
// lattice, and an unantialiased quad five pixels wide covers four to six of them
// depending on where between two squares it happens to fall — which is most of
// why some drops read as thicker than their neighbours. Rounding the two ends of
// the line instead would fix the width and wreck the slant: a drop is two to seven
// squares long, so moving an end by half a square swings the angle by tens of
// degrees, and the swing changes every frame as the drop falls. Quantising where
// each square is *drawn* leaves the direction exactly as it was.
//
// Walked by rows because the fall is nearly vertical. At the slant the wind gives
// it, a drop moves about a third of a square sideways per row, so the staircase is
// always joined: no gaps to fill and no square drawn twice to double its alpha.
void DrawStreak(Vector2 tail, Vector2 head, float pixel, Color colour) {
    const float slope = (head.x - tail.x) / std::max(head.y - tail.y, 1e-3f);

    const int n0 = static_cast<int>(std::floor(tail.y / pixel));
    const int n1 = static_cast<int>(std::ceil(head.y / pixel));

    for (int n = n0; n <= n1; n++) {
        // Squares whose centre the streak actually reaches, so the ends stop where
        // the drop stops rather than at the next lattice line.
        const float y = (static_cast<float>(n) + 0.5f) * pixel;
        if (y < tail.y || y >= head.y) continue;

        const int m = static_cast<int>(std::floor((head.x + (y - head.y) * slope) / pixel));

        DrawRectangleRec({static_cast<float>(m) * pixel, static_cast<float>(n) * pixel, pixel, pixel}, colour);
    }
}

} // namespace

// ---------------------------------------------------------------- the weather

Mood Sky::MoodOfSpell(long spell) const {
    // Drawn from the spell's index by weight, so the sequence is fixed for a world
    // and never has to be stored anywhere. Deliberately memoryless: a real sky does
    // not owe you a clear afternoon because the last one was wet.
    float total = 0.0f;
    for (const MoodDef &mood : settings_.moods) total += std::max(mood.likelihood, 0.0f);

    if (total <= 0.0f) return Mood::Clear;

    const auto mixed =
        static_cast<int>(static_cast<unsigned>(spell) * 2654435761u ^ static_cast<unsigned>(settings_.seed));

    float roll = Hash(mixed) * total;

    for (int index = 0; index < kMoodCount; index++) {
        roll -= std::max(settings_.moods[index].likelihood, 0.0f);
        if (roll <= 0.0f) return static_cast<Mood>(index);
    }

    return static_cast<Mood>(kMoodCount - 1);
}

Weather Sky::WeatherAt(float seconds) const {
    const float spellLength = std::max(settings_.spellMinutes, 0.1f) * 60.0f;
    const float crossLength = std::clamp(settings_.crossMinutes * 60.0f, 0.0f, spellLength);

    const float position = seconds / spellLength;
    const auto spell     = static_cast<long>(std::floor(position));
    const float into     = (position - static_cast<float>(spell)) * spellLength;

    const MoodDef &from = settings_.moods[static_cast<int>(MoodOfSpell(spell))];
    const MoodDef &to   = settings_.moods[static_cast<int>(MoodOfSpell(spell + 1))];

    // Held at the spell's own mood until the crossing begins, then eased across.
    // Every field of the row moves together, because they are one fact about one
    // afternoon rather than five settings that happen to sit near each other.
    const float blend = (crossLength <= 0.0f) ? 0.0f : SmoothStep(spellLength - crossLength, spellLength, into);

    Weather weather;
    weather.name     = (blend < 0.5f) ? from.name : to.name;
    weather.cover    = from.cover + (to.cover - from.cover) * blend;
    weather.rain     = from.rain + (to.rain - from.rain) * blend;
    weather.shade    = from.shade + (to.shade - from.shade) * blend;
    weather.sunlight = Mix(from.sunlight, to.sunlight, blend);
    weather.ambient  = Mix(from.ambient, to.ambient, blend);

    return weather;
}

void Sky::Advance(float dt) {
    time_ += dt;
    now_ = WeatherAt(time_);
}

// ------------------------------------------------------------------ the field

float Sky::Field(Vector2 world) const {
    // What the whole sky does: travel with the wind. Everything below is written
    // against it, so the three layers hold together as one cloud however fast each
    // one is moving through itself.
    const float drift = time_ * settings_.wind;

    // Perlin-Worley, the base of every volumetric cloud since Nubis.
    //
    // The Perlin gives the drift and the large shape — where there is cloud at all.
    // The Worley, inverted so its cells read as bumps instead of as walls, is added
    // over it at several times the frequency, and that is what puts a rim of rounded
    // lobes on an outline that would otherwise be a smooth swell. A cauliflower,
    // which is the shape of a cumulus at every scale it has.
    //
    // The three are sampled at three positions, not one. Sharing a position was the
    // whole of what made the sky read as wallpaper: every layer travelled at exactly
    // the wind, so the cloud was a rigid cutout on a conveyor and no part of its
    // shape ever changed. What each layer does on top of the drift is what makes it
    // weather — the swell moves through the depth of its own field and so re-forms
    // as it goes, and the two Worleys, which have no depth to move through, crawl
    // across it instead.
    //
    // None of it can upset the calibration, which is worth knowing before touching
    // any of these numbers: the three fields carry different seeds and are
    // independent, so their joint distribution is the product of their own whatever
    // the offsets are, and every plane through a Perlin field is distributed like
    // every other. Cutoff measures the same field at any moment.
    terrain::NoiseShape swell = settings_.shape;
    swell.offsetZ += time_ * settings_.evolve;

    const float perlin = std::clamp(terrain::Sample({world.x - drift, world.y}, swell), 0.0f, 1.0f);

    const Vector2 overLobes = {world.x - drift - time_ * settings_.lobeCrawl.x,
                               world.y - time_ * settings_.lobeCrawl.y};

    const float bumps = 1.0f - terrain::Worley(overLobes, settings_.lobes);

    float value = perlin + (bumps - 0.5f) * settings_.worleyMix;

    // Then the silhouette is eaten into by a finer Worley still — the erosion pass.
    //
    // Sampled only near the edge. Deep inside a cloud it cannot change the outcome
    // and outside it there is no outline to erode, so the band where it matters is
    // narrow and skipping the rest is most of the cost of the whole field. The
    // optimisation is Nubis's own.
    if (settings_.erosion > 0.0f && std::abs(value - erosionAt_) < settings_.erosionBand) {
        const Vector2 overDetail = {world.x - drift - time_ * settings_.detailCrawl.x,
                                    world.y - time_ * settings_.detailCrawl.y};

        value -= terrain::Worley(overDetail, settings_.detail) * settings_.erosion;
    }

    return value;
}

// ------------------------------------------------------------- configuration

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

    // Two passes. The first measures the field with the erosion switched off, only
    // to find where its own middle is, because the erosion has to know which band
    // counts as "near the edge" before it can be applied at all. The second measures
    // the field as it will actually be drawn.
    for (int pass = 0; pass < 2; pass++) {
        values.clear();

        for (int i = 0; i < kSamplesPerAxis; i++) {
            for (int j = 0; j < kSamplesPerAxis; j++) {
                // Stretched on the horizontal axis exactly as the field is, so the
                // samples are one feature apart in the field's own terms rather than
                // in pixels.
                const Vector2 at = {(i - kSamplesPerAxis / 2) * feature * aspect * kStrideX,
                                    (j - kSamplesPerAxis / 2) * feature * kStrideY};

                values.push_back(Field(at));
            }
        }

        std::sort(values.begin(), values.end());

        // Step zero is a cutoff nothing clears, the last is one everything does, and
        // between them each step is the quantile leaving that share of the field
        // above it.
        for (int step = 0; step < kCutoffSteps; step++) {
            const float cover = static_cast<float>(step) / static_cast<float>(kCutoffSteps - 1);
            const auto index  = static_cast<std::size_t>((1.0f - cover) * (values.size() - 1));

            cutoff_[step] = values[index];
        }

        // Where the erosion pass should bite: around the cutoff for the weather this
        // world usually has. Set after the first pass so the second sees it.
        erosionAt_ = Cutoff(settings_.moods[static_cast<int>(Mood::Fair)].cover);
    }

    now_ = WeatherAt(time_);
}

float Sky::Cutoff(float cover) const {
    const float at   = std::clamp(cover, 0.0f, 1.0f) * static_cast<float>(kCutoffSteps - 1);
    const int step   = std::min(static_cast<int>(at), kCutoffSteps - 2);
    const float into = at - static_cast<float>(step);

    return cutoff_[step] + (cutoff_[step + 1] - cutoff_[step]) * into;
}

// -------------------------------------------------------------- the cloud now

Column Sky::ColumnAt(float worldX) const {
    // The front, drifting past on its own, and the climate underneath. Both only
    // ripple the level the weather set — a storm has to stay overcast everywhere in
    // it, and a clear afternoon clear.
    const float front = Share({worldX - time_ * settings_.frontWind, 0.0f}, settings_.front);

    const terrain::Climate climate = terrain::ClimateAt(worldX, terrain_);

    Column column;

    column.cover = std::clamp(now_.cover + Swing(front) * settings_.frontInfluence +
                                  Swing(climate.humidity) * settings_.humidityInfluence,
                              0.0f, 1.0f);

    column.cutoff = Cutoff(column.cover);

    // The weather's rain, not this column's. There used to be a twelve-step march
    // down the band here, measuring how thick the cloud was and raining out of the
    // thick ones — which is what made a small cluster rain while its neighbour, in
    // the same weather, stayed dry. It was also four fifths of what this function
    // cost.
    column.rain = now_.rain;

    return column;
}

float Sky::MarginAt(Vector2 world, const Column &column) const {
    if (column.cover <= 0.0f) return -1.0f;

    // The underside hangs lower in heavy weather, which is the visible half of a
    // rain cloud sitting closer to the ground than a fair-weather one.
    const float base = settings_.base + settings_.rainDrop * now_.rain;
    const float span = base - settings_.ceiling;
    if (span <= 0.0f) return -1.0f;

    const float through = (world.y - settings_.ceiling) / span;
    if (through <= 0.0f || through >= 1.0f) return -1.0f;

    // Nothing at the top and bottom of the band, thickest through the middle. A
    // cloud with a flat top and a flat bottom reads as a slab of ceiling rather than
    // as weather.
    //
    // The profile thins the cover, not the field. Cover is what the cutoff is
    // measured from, so a cover running to zero at the edge of the band takes the
    // cutoff with it and the deck always ends softly — where a fixed penalty on the
    // cutoff is a fixed number of field units and an overcast sky walks straight
    // through it.
    const float profile = std::pow(std::sin(through * kPi), std::max(settings_.bandTaper, 0.01f));

    return Field(world) - Cutoff(column.cover * profile);
}

float Sky::DensityAt(Vector2 world, const Column &column) const {
    return std::clamp(MarginAt(world, column) / std::max(settings_.softness, 1e-3f), 0.0f, 1.0f);
}

float Sky::CoverAt(float worldX) const {
    // An empty sky has no cloud to march through and no shadow to cast.
    if (now_.cover <= 0.0f) return 0.0f;

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
    constexpr int kSteps = 8;

    const float span = (settings_.base + settings_.rainDrop * now_.rain) - settings_.ceiling;
    if (span <= 0.0f) return 0.0f;

    float thickest = 0.0f;

    for (int step = 0; step < kSteps; step++) {
        const float through = (static_cast<float>(step) + 0.5f) / static_cast<float>(kSteps);

        thickest = std::max(thickest, DensityAt({worldX, settings_.ceiling + through * span}, column));
    }

    return thickest;
}

float Sky::UndersideAt(float worldX) const {
    const float bottom = settings_.base + settings_.rainDrop * now_.rain;

    // An empty sky has no cloud to leave, and the bottom of the band is where the
    // rain used to start from unconditionally. Answering with it means a caller
    // always has a height and never a "no cloud here" to handle.
    if (now_.cover <= 0.0f) return bottom;

    const float span = bottom - settings_.ceiling;
    if (span <= 0.0f) return bottom;

    const Column column = ColumnAt(worldX);

    // Marched upward from the bottom of the band and stopped at the first edge: the
    // lowest cloud in the column, not the thickest one. A drop leaves the first
    // underside there is, and whatever hangs above that is behind it.
    //
    // Nine samples, held a little inside the band's two edges so `through` is never
    // out of range and MarginAt answers with a margin rather than with its own
    // out-of-band sentinel. About the march the shadow makes, so the rain leaves the
    // cloud at the height the cloud is drawn at rather than at one of its own.
    constexpr int kSteps   = 8;
    constexpr float kInset = 0.02f;

    auto heightAt = [&](int step) {
        const float through =
            kInset + (1.0f - 2.0f * kInset) * (1.0f - static_cast<float>(step) / static_cast<float>(kSteps));

        return settings_.ceiling + through * span;
    };

    float belowAt     = heightAt(0);
    float belowMargin = MarginAt({worldX, belowAt}, column);

    // The cloud fills the band as far down as it goes, so that is where the drop
    // leaves it.
    if (belowMargin > kCloudEdge) return belowAt;

    for (int step = 1; step <= kSteps; step++) {
        const float at     = heightAt(step);
        const float margin = MarginAt({worldX, at}, column);

        if (margin > kCloudEdge) {
            // The edge itself, found between the two samples straddling it, rather
            // than the sample above it. The margin is signed about the cloud's own
            // outline — that is what a cloud edge of zero means — so this is the
            // same root the terrain contour is drawn through. Without it every drop
            // in the world would start at one of nine heights and the top of the
            // rain would be a staircase.
            return belowAt + (at - belowAt) * (-belowMargin / (margin - belowMargin));
        }

        belowAt     = at;
        belowMargin = margin;
    }

    return bottom;
}

float Sky::ShadeAt(float worldX) const {
    return CoverAt(worldX) * std::clamp(settings_.shade * now_.shade, 0.0f, 1.0f);
}

float Sky::RainAt(float worldX) const {
    (void)worldX;

    // The weather's, everywhere. Kept taking a position so the caller does not have
    // to know that, and so a future local squall has somewhere to go.
    return now_.rain;
}

// ------------------------------------------------------------------- the air

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

    // Then washed towards grey by the weather. What is being looked at under a full
    // sky is the underside of the cloud deck, not the air.
    return Mix(lit, air.overcast, std::clamp(cover, 0.0f, 1.0f) * air.overcastWash);
}

void Sky::DrawAtmosphere(Rectangle view) const {
    const float band = std::max(settings_.air.bandHeight, 1.0f);

    // Snapped to the world, so the bands do not crawl up and down as the view
    // scrolls.
    const float top = std::floor(view.y / band) * band;

    // The weather's own cover, not a column's. The wash is a property of the sky
    // over the whole view; reading it per column would draw a comb of vertical
    // stripes across the air behind the clouds.
    for (float y = top; y < view.y + view.height; y += band) {
        DrawRectangleRec({view.x, y, view.width, band}, AirAt(y + band * 0.5f, now_.cover));
    }
}

// --------------------------------------------------------------- the lighting

float Sky::Lighting(float depthToSun, float here) const {
    const Shading &shading = settings_.shading;

    // Beer-Lambert, on the path to the sun. One when nothing is in the way, falling
    // off as more cloud stands between this point and the light.
    const float beer = std::exp(-shading.absorption * std::max(depthToSun, 0.0f));

    // The powder term, on the density at this point rather than on the path.
    //
    // It models the light that scatters back out of a thin edge, and it runs the
    // other way: zero where there is almost no cloud, rising as the cloud thickens.
    // So the very fringe comes out dark, the body just inside it bright, and the far
    // side dark again because Beer has taken the light. That is the double curve the
    // eye reads as cloud rather than as a shape with a gradient on it, and it is why
    // both terms are needed and why they cannot be measured on the same thing.
    const float powder = 1.0f - std::exp(-2.0f * shading.powderScale * std::max(here, 0.0f));

    const float lit = beer * (powder * shading.powder + (1.0f - shading.powder));

    return std::clamp(lit, 0.0f, 1.0f);
}

float Sky::BandLight(int index, int count) const {
    const Shading &shading = settings_.shading;

    // Divided by the last index rather than by the count, so the top band actually
    // reaches `lightest`. Over the count it stops one step short and the brightest
    // part of every cloud comes out greyer than it was asked to be.
    const float at = (count > 1) ? static_cast<float>(index) / static_cast<float>(count - 1) : 1.0f;

    return shading.darkest + (shading.lightest - shading.darkest) * at;
}

Color Sky::CloudTint(float lit) const {
    return Mix(now_.ambient, now_.sunlight, std::clamp(lit, 0.0f, 1.0f));
}

// ------------------------------------------------------------------ the cloud

void Sky::DrawClouds(Rectangle view, int spacing) const {
    // The band, clipped to the view. Underground there is no overlap at all and the
    // whole cost of the sky goes away without anything having to ask where the
    // player is.
    const float deepest = settings_.base + settings_.rainDrop;

    const float top    = std::max(view.y, settings_.ceiling);
    const float bottom = std::min(view.y + view.height, deepest);
    if (bottom <= top) return;

    const int bands = std::max(settings_.shading.layers, 1);

    // Coarser than the world's own lattice. A cloud is some four hundred pixels
    // across, so this is still thirty-odd samples over one of them, and the
    // rasteriser interpolates between samples anyway — the shape does not coarsen,
    // only the sampling of it, and the field is much the most expensive thing here.
    const float step = static_cast<float>(std::max(spacing, 1)) * settings_.fieldStep;

    // Room for the march towards the sun to read outside the visible band.
    const float margin = settings_.shading.sunReach + step;

    const Vector2 origin = {std::floor((view.x - margin) / step) * step, std::floor((top - margin) / step) * step};

    const int cols = static_cast<int>(std::ceil((view.width + 2.0f * margin) / step)) + 2;
    const int rows = static_cast<int>(std::ceil((bottom - top + 2.0f * margin) / step)) + 2;

    // One grid, filled once. There used to be eight of these and six full copies a
    // frame, because each shading layer was a separate pass over its own shifted
    // copy of the same numbers.
    Grid field(origin, cols, rows, static_cast<int>(step));

    for (int i = 0; i < cols; i++) {
        // Once per column, not once per sample: the weather over a column is the
        // same at every height in it.
        const Column column = ColumnAt(origin.x + i * step);

        for (int j = 0; j < rows; j++) field.SetValue(i, j, MarginAt(field.PointAt(i, j), column));
    }

    // How far towards the sun the depth is read, in grid cells.
    const Vector2 sun     = Unit(settings_.shading.sun, {0.8f, -0.6f});
    const float reach     = settings_.shading.sunReach;
    const Vector2 towards = {sun.x * reach, sun.y * reach};

    // One pass. Each cell is shaded from its own depth and from the cloud between it
    // and the sun, both read out of this same grid, so the shading is a property of
    // the cloud and cannot depend on where the camera is standing.
    DrawShaded(field, towards, bands);
}

void Sky::DrawShaded(const Grid &field, Vector2 towards, int bands) const {
    const float step  = static_cast<float>(field.Spacing());
    const float pixel = std::max(config::kPixelSize, 1.0f);

    // The depth towards the sun, in the field's own units, read at the offset. A
    // lookup into the grid already built — not a new sample of the noise.
    auto depthToSun = [&](Vector2 at) {
        const Vector2 probe = {at.x + towards.x, at.y + towards.y};

        int i = 0;
        int j = 0;
        field.ToLocal(probe, i, j);

        // Off the edge of the grid is open sky, which is nothing in the way.
        if (!field.InBounds(i, j)) return 0.0f;

        return std::max(field.ValueAt(i, j), 0.0f);
    };

    for (int i = 0; i < field.Cols() - 1; i++) {
        for (int j = 0; j < field.Rows() - 1; j++) {
            const float a = field.ValueAt(i, j);
            const float b = field.ValueAt(i + 1, j);
            const float c = field.ValueAt(i, j + 1);
            const float d = field.ValueAt(i + 1, j + 1);

            // Nothing of the cloud in this cell. Most cells, so this is the test
            // that makes the whole pass affordable.
            if (a <= kCloudEdge && b <= kCloudEdge && c <= kCloudEdge && d <= kCloudEdge) continue;

            const Vector2 at = field.PointAt(i, j);

            // Squares whose centre falls in this cell. Anchored to the world rather
            // than to the cell, so the grid does not shift by a fraction of a square
            // at every cell border — the same anchoring marching_squares uses, and
            // it has to match or the cloud will not line up with the ground.
            const int m0 = static_cast<int>(std::floor(at.x / pixel));
            const int m1 = static_cast<int>(std::ceil((at.x + step) / pixel));
            const int n0 = static_cast<int>(std::floor(at.y / pixel));
            const int n1 = static_cast<int>(std::ceil((at.y + step) / pixel));

            for (int n = n0; n <= n1; n++) {
                const float y = (static_cast<float>(n) + 0.5f) * pixel;
                if (y < at.y || y >= at.y + step) continue;

                // A run of squares in the same band is one rectangle. Bands are
                // wide, so most of a cloud is a handful of runs rather than a few
                // hundred squares.
                int run  = -1;
                int from = m0;

                for (int m = m0; m <= m1 + 1; m++) {
                    const float x = (static_cast<float>(m) + 0.5f) * pixel;

                    int band = -1;

                    if (m <= m1 && x >= at.x && x < at.x + step) {
                        // Bilinear, so the outline follows the field between samples
                        // exactly as the contour would rather than stepping cell to
                        // cell.
                        const float fx = (x - at.x) / step;
                        const float fy = (y - at.y) / step;

                        const float here = (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;

                        if (here > kCloudEdge) {
                            const float lit = Lighting(depthToSun({x, y}), here);

                            // Quantised last. The model is continuous; the flat bands
                            // are the pixel-art step over the top of it.
                            band = std::clamp(static_cast<int>(lit * static_cast<float>(bands)), 0, bands - 1);
                        }
                    }

                    if (band != run) {
                        if (run >= 0) {
                            DrawRectangleRec({from * pixel, y - pixel * 0.5f, (m - from) * pixel, pixel},
                                             CloudTint(BandLight(run, bands)));
                        }

                        run  = band;
                        from = m;
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------- the rain

Vector2 Sky::RainFall() const {
    // The slant is the wind against the fall, so the rain leans into a gale and
    // stands up in still air without either being written down. A drop is far
    // lighter than a cloud and is in faster air, so it feels the wind much harder —
    // that is `rainDrift`, and without it the slant is a degree and a half.
    return Unit({settings_.wind * settings_.rainDrift, settings_.rainSpeed}, {0.0f, 1.0f});
}

float Sky::RainReach() const {
    const Vector2 fall = RainFall();

    return std::abs(fall.x / std::max(fall.y, 1e-3f)) * std::max(settings_.rainSpan, 1.0f);
}

void Sky::DrawRain(Rectangle view, const Ground &ground) const {
    // No weather, no cost. This used to walk every drop across the view and read a
    // full weather column for each one before finding out it was not raining.
    if (now_.rain <= 0.0f) return;

    const Settings &s = settings_;

    const Vector2 fall = RainFall();

    const float perDrop = terrain::kFeatureSpan / std::max(s.rainDensity, 1.0f);
    const float span    = std::max(s.rainSpan, 1.0f);

    // Widened, because a slanted drop starting off the left of the view can still
    // cross it further down.
    const float lean = RainReach();
    const int first  = static_cast<int>(std::floor((view.x - lean) / perDrop));
    const int last   = static_cast<int>(std::ceil((view.x + view.width + lean) / perDrop));

    const float pixel = std::max(config::kPixelSize, 1.0f);

    // Where the drops come from: the underside of the cloud over each column,
    // sampled across the stretch the loop covers and interpolated between.
    //
    // It was one height for the whole world — `base + rainDrop * rain` — and that is
    // the whole of what was wrong with it. The band's underside is a hundred pixels
    // below where the cloud actually stops in most columns, so the rain began in
    // open sky, on a ruled horizontal line, under clouds that plainly were not
    // producing it.
    //
    // Once per sample rather than once per drop. A drop is four pixels from its
    // neighbour and a march up the band is nine reads of the cloud field, so asking
    // per drop is the twelve-step march that came out of ColumnAt for costing four
    // fifths of it. Ten squares between samples is still eight across the smallest
    // thing in the sky worth following.
    //
    // Floored to its own step, so the samples sit at fixed places in the world
    // rather than wherever the camera starts. Anchored to the view instead, every
    // sample point would move as the player walks and the interpolated base would
    // ripple in step with their footsteps — the same reason the atmosphere bands and
    // the cloud grid are floored to theirs.
    constexpr float kBaseStep = 50.0f;

    const float firstX = std::floor((view.x - lean) / kBaseStep) * kBaseStep;
    const int bases    = static_cast<int>(std::ceil((view.width + 2.0f * lean) / kBaseStep)) + 2;

    std::vector<float> base(static_cast<std::size_t>(bases));
    for (int i = 0; i < bases; i++) base[i] = UndersideAt(firstX + static_cast<float>(i) * kBaseStep);

    for (int drop = first; drop <= last; drop++) {
        // Thinned by how hard it is raining, from the drop's own hash rather than by
        // a count: a drizzle is the same drops falling, fewer of them.
        if (Hash(drop * 7919 + 13) > now_.rain) continue;

        const float column = (drop + Hash(drop)) * perDrop;

        // Size. Three rough gauges rather than one, because rain of a single gauge
        // is a comb; the big ones are longer, faster and more opaque, which is most
        // of what makes a downpour read as heavy rather than merely dense.
        //
        // Weight used to be carried by width as well, and it was the wrong axis to
        // spend it on: at the size of a square, one gauge wider is twice as wide,
        // and a drop ten pixels across and fifteen long is a brick. What is left is
        // the two axes a drop can afford — how long it is and how solid.
        const float gauge = Hash(drop * 6151 + 3);
        const float scale = 1.0f - s.rainSpread + 2.0f * s.rainSpread * gauge;

        const float length = s.rainLength * scale;

        // Position along the fall, over a span that does not depend on the weather,
        // so the speed is exactly `rainSpeed` whatever the sky is doing. Bigger drops
        // fall faster, which is also true.
        const float travel = std::fmod(Hash(drop * 104729 + 7) * span + time_ * s.rainSpeed * scale, span);

        // The cloud this one fell out of, read between the two samples either side
        // of its column. Interpolated rather than picked, or the top of the rain
        // would step down in fifty-pixel blocks.
        const float along = (column - firstX) / kBaseStep;
        const int cell    = std::clamp(static_cast<int>(along), 0, bases - 2);
        const float into  = std::clamp(along - static_cast<float>(cell), 0.0f, 1.0f);

        const float from = base[cell] + (base[cell + 1] - base[cell]) * into;

        Vector2 head       = {column + fall.x * travel, from + fall.y * travel};
        const Vector2 tail = {head.x - fall.x * length, head.y - fall.y * length};

        // Off the view, tested before the ground under it. The loop runs over a
        // stretch half again as wide as the screen, because a slanted drop starting
        // off the left of it can still cross it further down — so most drops reached
        // here are off the side, and finding the ground beneath one costs eight
        // samples of noise. Both are plain tests on the same position, so which goes
        // first changes nothing but the bill.
        if (head.y < view.y || tail.y > view.y + view.height) continue;
        if (head.x < view.x - pixel || tail.x > view.x + view.width + pixel) continue;

        // Landed. Cut off at the surface rather than dropped whole, so the streak
        // ends *on* what it hits: a drop tested only at its head disappears a whole
        // streak-length early, and the rain stops in a band of clear air above the
        // ground instead of against it.
        //
        // The surface is the world as it is, not the shape of the land the generator
        // describes. terrain::Height is a function of the column alone and cannot
        // know what has been built, so rain fell straight through anything standing
        // above the ground — which is the whole point of asking: a roof has to keep
        // the rain out.
        const float land = GroundAt(ground, head.x, terrain_);

        if (head.y > land) {
            const float over = (head.y - land) / std::max(fall.y, 1e-3f);

            head = {head.x - fall.x * over, land};
        }

        // Wholly under it. Skipped rather than wrapped, so the drop keeps its own
        // cadence and the number in the air over a column follows how far they have
        // to fall, which is what a deep valley in the rain looks like.
        if (head.y <= tail.y) continue;

        // Lighter than the air behind it, always.
        //
        // The drop takes the sky at its own height and is lifted towards the pale
        // rain colour, so it reads against a bright noon and against a dark storm
        // alike — and against a night, once there is one, without anything here
        // being touched. It was a fixed colour multiplied down to thirty per cent,
        // which came out very nearly black.
        const Color behind = AirAt(head.y, now_.cover);
        const Color line   = Mix(behind, s.rainLine, 0.55f + 0.35f * gauge);

        // Opened downwards rather than up. The heaviest drops are as solid as they
        // were; the finest are what got fainter, which is where the weight the width
        // used to carry has gone.
        DrawStreak(tail, head, pixel, Fade(line, 0.40f + 0.50f * gauge));
    }
}

} // namespace weather
