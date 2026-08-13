#include "weather.h"

#include "config.h"
#include "grid.h"
#include "marching_squares.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace weather {
namespace {

constexpr float kPi  = 3.14159265f;
constexpr float kLn2 = 0.69314718f;

// Share of a whole day run through per second while a skip is owed.
//
// A twelfth, so a quarter of a day arrives in about three seconds: long enough to
// watch the light turn over, short enough not to be waiting for it. Paid out of the
// same dt the weather is stepped with, so the debug key that runs the weather fast
// makes this near-instant too, which is the right way round — with that on there is
// nothing to skip.
constexpr float kSkipRate = 1.0f / 12.0f;

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

// A colour multiplied channel by channel, which is what light does to a surface.
//
// Mix cannot stand in for this: it slides one colour towards another, so a white
// tint leaves the colour alone and a black one takes it to black, where a *scale*
// of one leaves it alone and a scale of a half halves it. Tinting sunlight by the
// colour the sun has left after crossing the air is a multiplication.
Color Scale(Color colour, Vector3 by) {
    return {ToByte(colour.r / 255.0f * by.x), ToByte(colour.g / 255.0f * by.y), ToByte(colour.b / 255.0f * by.z), 255};
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
    // Held at one kind of weather, with no spell and no crossing — see ForceMood.
    // Taken before anything else, so the whole of the rest of this module goes on
    // reading one weather and knows nothing about being held at it.
    if (forcedMood_ >= 0) {
        const MoodDef &held = settings_.moods[forcedMood_ % kMoodCount];

        return {.name     = held.name,
                .cover    = held.cover,
                .rain     = held.rain,
                .shade    = held.shade,
                .sunlight = held.sunlight,
                .ambient  = held.ambient};
    }

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

// ------------------------------------------------------------------- the day

float Sky::SecondsPerDay() const {
    return std::max(settings_.day.dayMinutes, 0.1f) * 60.0f;
}

float Sky::SunLightAt(float seconds) const {
    const Day &day = settings_.day;

    const float phase = seconds / SecondsPerDay() + day.startAt;

    // The sun's height, as a sine of the turn. Midnight at the bottom, noon at the
    // top, and the two crossings are sunrise and sunset without either being written
    // down anywhere.
    const float elevation = std::sin((phase - std::floor(phase)) * 2.0f * kPi - kPi * 0.5f);

    // Eased across the horizon rather than switched at it, over a band that begins
    // below zero: light arrives before the sun does and outlasts it, which is what
    // twilight is and what makes the day longer than the night.
    return SmoothStep(day.darkAt, day.litAt, elevation);
}

Daylight Sky::DaylightAt(float seconds) const {
    const Day &day = settings_.day;

    // `startAt` belongs here and was missing.
    //
    // SunLightAt has always added it and this has never done, so the two
    // disagreed about what time it was — and since this is the one that produces
    // the Daylight a frame is lit and drawn by, the setting had no effect on
    // anything anybody could see. A fresh world opened at midnight in the pitch
    // dark, against the plain intent of the field's own comment.
    const float turns = seconds / SecondsPerDay() + day.startAt;

    Daylight light;
    light.phase     = turns - std::floor(turns);
    light.elevation = std::sin(light.phase * 2.0f * kPi - kPi * 0.5f);
    light.light     = SmoothStep(day.darkAt, day.litAt, light.elevation);

    light.name = (light.light >= 0.98f)                           ? "day"
                 : (light.light <= 0.02f)                         ? "night"
                 : (light.elevation > 0.0f || light.phase < 0.5f) ? "dawn"
                                                                  : "dusk";

    // Where it lies. The tilt is held off the vertical on purpose: a sun directly
    // overhead lights only the tops of the clouds and takes the side off them, which
    // is the one thing the shading settings warn about.
    //
    // Nothing is drawn there. This is a direction the cloud is shaded from, not a
    // body in the sky — and it keeps pointing after the sun has set, which is what
    // lights the underside of a cloud at dusk and costs nothing.
    const float across = std::cos(light.phase * 2.0f * kPi - kPi * 0.5f);

    light.sun = Unit({across, -light.elevation * day.sunTilt}, {0.8f, -0.6f});

    // How far the light has come through the air to arrive. Longest along the
    // horizon, nothing overhead.
    //
    // Keyed to the sun's height and not to the daylight, and the difference is the
    // whole look of it: golden hour is when the sun is low and it is *still broad
    // day*. Keyed to the daylight, the colour would peak at half brightness and read
    // as brown.
    light.travel = day.travel * (1.0f - std::clamp(light.elevation, 0.0f, 1.0f));

    // And what is left of it after that journey.
    //
    // This is the sunset, and it is the one thing about the sky that could not be
    // had by thickening the air in the line of sight. That air only ever *adds*: in
    // scattering saturates every channel towards one, so more of it whitens. What
    // reddens the light is the air it already crossed, which scatters the blue out
    // of the beam before it arrives — the same coefficients, read the other way
    // round.
    const Vector3 &scatter = settings_.air.rayleigh;

    const Vector3 through = {std::exp(-scatter.x * light.travel), std::exp(-scatter.y * light.travel),
                             std::exp(-scatter.z * light.travel)};

    // Normalised to its strongest channel, so this only ever says what colour the
    // light is. How much of it there is, is `light`, and it is applied once.
    const float strongest = std::max({through.x, through.y, through.z, 1e-4f});

    // Then run back to white as the light itself goes.
    //
    // A beam that has set has no colour left to give, and the tint has to go with
    // it: below the horizon `travel` is at its longest and stays there, so without
    // this the sky keeps its sunset all night. That matters more than it sounds,
    // because the whole scene is *also* multiplied by the light — an orange sky
    // under a blue moonlight is the one place two tints meet, and what comes out is
    // brown. The colour peaks where it should anyway, just above the horizon, where
    // the sun is low and it is still broad day.
    const float shown = light.light;

    light.beam = {1.0f + (through.x / strongest - 1.0f) * shown, 1.0f + (through.y / strongest - 1.0f) * shown,
                  1.0f + (through.z / strongest - 1.0f) * shown};

    return light;
}

void Sky::SkipToQuarter() {
    const float span = SecondsPerDay();

    // Where the day already is, counting anything still owed from a previous ask, so
    // that pressing twice queues two quarters rather than landing on the same one.
    const float ahead = (time_ + dayOffset_ + daySkip_) / span;
    const float into  = ahead - std::floor(ahead);

    daySkip_ += (0.25f - std::fmod(into, 0.25f)) * span;
}

void Sky::Advance(float dt) {
    time_ += dt;

    // Any skip still owed, paid out over a few seconds rather than in one step. What
    // is usually being looked at *is* the transition, and a jump lands on the far
    // side of it.
    if (daySkip_ > 0.0f) {
        const float step = std::min(daySkip_, SecondsPerDay() * kSkipRate * std::max(dt, 0.0f));

        dayOffset_ += step;
        daySkip_ -= step;
    }

    // Wrapped, so a session left running overnight cannot walk the offset out of the
    // precision a float has left.
    dayOffset_ = std::fmod(dayOffset_, SecondsPerDay());

    now_   = WeatherAt(time_);
    today_ = DaylightAt(time_ + dayOffset_);

    Reckon();
}

void Sky::Reckon() {
    const Day &day = settings_.day;

    // How much it has rained lately, and how long the ground has stood in the sun.
    //
    // Read backwards out of the clock rather than accumulated forwards through it.
    // That is only possible because the weather is a pure function of time, and it
    // is worth saying what it buys: the ground has a memory without the world having
    // any state, so two views of it at the same moment agree and there is nothing to
    // save. An exponential kernel is exactly the leaky integrator
    // `w += (rain - w) * k * dt` with the accumulator taken out of it.
    //
    // Two dozen samples over the window. Fewer aliases badly: the rain is close to a
    // square wave on a five-minute period with a crossing of about one, so a coarse
    // comb beats against it and the reading jitters as the clock moves. This costs a
    // couple of microseconds once a frame — the weather at a moment is a table
    // lookup and a hash, with no noise in it at all.
    constexpr int kSamples = 24;

    const float window = std::max(day.wetMinutes, 0.1f) * 60.0f;
    const float decay  = kLn2 / std::max(day.wetHalfLife, 0.01f) / 60.0f;

    float rain    = 0.0f;
    float sun     = 0.0f;
    float weights = 0.0f;

    for (int step = 0; step < kSamples; step++) {
        const float back = (static_cast<float>(step) + 0.5f) / static_cast<float>(kSamples) * window;

        const float weight = std::exp(-decay * back);

        rain += WeatherAt(time_ - back).rain * weight;
        sun += SunLightAt(time_ + dayOffset_ - back) * weight;

        weights += weight;
    }

    wet_     = (weights > 0.0f) ? rain / weights : 0.0f;
    drought_ = (weights > 0.0f) ? sun / weights : 0.0f;
}

float Sky::HumidityAt(float worldX) const {
    const terrain::Climate climate = terrain::ClimateAt(worldX, terrain_);
    const Day &day                 = settings_.day;

    // The climate of the place, raised by what has fallen on it and lowered by what
    // has stood over it. Warm ground gives up its water faster than cold, which is
    // the first thing anything in this world has ever asked of the temperature.
    const float dried = drought_ * day.dryGain * (0.5f + climate.temperature);

    return std::clamp(climate.humidity + wet_ * day.wetGain - dried, 0.0f, 1.0f);
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

    // The moment, as it stands. Filled here as well as in Advance so that a sky
    // configured and then drawn before the first frame is stepped has a time of day
    // and a soil that has been rained on, rather than the midnight and the drought a
    // default-constructed one would report.
    now_   = WeatherAt(time_);
    today_ = DaylightAt(time_ + dayOffset_);

    Reckon();
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
    const float held = std::clamp(settings_.shade * now_.shade, 0.0f, 1.0f);

    // Eased off as the light goes.
    //
    // Without this a storm at midnight is black, and for a reason worth stating: the
    // shade is a *share* of the daylight held back, and holding back three quarters
    // of a moonlit sky leaves nothing at all. The cloud is still there and the ground
    // under it is still darker; it simply cannot take away light that was never
    // arriving. At noon this is one exactly, so nothing about a daytime storm moved.
    const float day = settings_.day.nightShade + (1.0f - settings_.day.nightShade) * today_.light;

    return CoverAt(worldX) * held * day;
}

float Sky::RainAt(float worldX) const {
    (void)worldX;

    // The weather's, everywhere. Kept taking a position so the caller does not have
    // to know that, and so a future local squall has somewhere to go.
    return now_.rain;
}

// How unsettled the weather is, in [0,1]. Cover on its own would have an overcast
// afternoon blowing as hard as a storm; rain on its own would leave a dry gale
// still. The two together are what "blustery" means.
float Sky::Bluster() const { return std::clamp(now_.cover * 0.45f + now_.rain * 0.55f, 0.0f, 1.0f); }

float Sky::WindAt(float worldX) const {
    const Settings::Gust &gust = settings_.gust;

    // Sampled at a position that slides with time, so what the field describes is
    // a shape crossing the world rather than a value changing where it stands.
    const float along = worldX / std::max(gust.wavelength, 1.0f) - time_ * gust.speed / std::max(gust.wavelength, 1.0f);

    // The shape's own seed, the way every other field in this module is read.
    const float wave = terrain::Signed({along * terrain::kFeatureSpan, 0.0f}, gust.shape);

    // A calm day still has its breeze; a storm has the mean and a gust half again
    // as strong on top of it.
    const float weight = 0.35f + 0.65f * Bluster();

    return settings_.wind * (1.0f + wave * gust.strength * weight);
}

float Sky::WindReach() const { return std::fabs(settings_.wind) * (1.0f + settings_.gust.strength); }

float Sky::PushAt(float worldX) const {
    return std::clamp(WindAt(worldX) / std::max(WindReach(), 1e-3f), -1.0f, 1.0f);
}

Sky::Season Sky::Turn() const {
    // Held, for looking at one season rather than waiting for it.
    if (forcedSeason_ >= 0) return {forcedSeason_ % 4, 0.0f};

    const float year = settings_.day.yearDays * SecondsPerDay();

    // No calendar yet. Spring, and no blend — see the declaration.
    if (year <= 0.0f) return {};

    const float through = std::fmod((time_ + dayOffset_) / year, 1.0f) * 4.0f;

    const int index = std::clamp(static_cast<int>(through), 0, 3);

    return {index, through - static_cast<float>(index)};
}

float Sky::MeanDaylight() const {
    // Walked rather than integrated. The curve is a smoothstep of a sine and its
    // mean has no useful closed form, and this is asked once.
    constexpr int kSamples = 96;

    const float day = SecondsPerDay();

    float total = 0.0f;

    for (int i = 0; i < kSamples; i++) {
        total += DaylightAt(static_cast<float>(i) / kSamples * day).light;
    }

    return total / static_cast<float>(kSamples);
}

float Sky::MeanRain() const {
    float rain   = 0.0f;
    float weight = 0.0f;

    for (const MoodDef &mood : settings_.moods) {
        rain += mood.rain * mood.likelihood;
        weight += mood.likelihood;
    }

    return (weight > 0.0f) ? rain / weight : 0.0f;
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

    // What colour the light arriving here already is, weighted by how much of this
    // line of sight is deep air.
    //
    // The sun's own journey, not this one. Overhead it has crossed almost nothing
    // and is white; along the horizon it has crossed the most air there is and has
    // had the blue taken out of it. Weighting by the view's own airmass is what
    // keeps the zenith blue while the horizon burns — the reddening shows up where
    // the light had furthest to come.
    const float depth = airmass / std::max(air.thickness, 1e-3f);

    const Vector3 beam = {std::pow(std::max(today_.beam.x, 1e-4f), depth),
                          std::pow(std::max(today_.beam.y, 1e-4f), depth),
                          std::pow(std::max(today_.beam.z, 1e-4f), depth)};

    // What that air scatters, channel by channel. Blue scatters some five times
    // more strongly than red, so thin air passes blue alone and thick air scatters
    // everything until it is white. The pale horizon, the blue overhead and the
    // dark above it are all this expression — none of the three is written down
    // anywhere.
    //
    // Colour only. How *bright* the sky is belongs to the light layer the whole
    // scene is multiplied by, and it is already carrying the day: dimming here as
    // well would square it, and dusk would come out black.
    const Color lit = {ToByte(beam.x * (1.0f - std::exp(-air.rayleigh.x * airmass))),
                       ToByte(beam.y * (1.0f - std::exp(-air.rayleigh.y * airmass))),
                       ToByte(beam.z * (1.0f - std::exp(-air.rayleigh.z * airmass))), 255};

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

void Sky::DrawStars(Rectangle view, const Ground &ground) const {
    const Stars &stars    = settings_.stars;
    const Atmosphere &air = settings_.air;

    // Three reasons there is nothing to draw, all of them decided before a single
    // star is placed: the sun is up, the deck is closed over them, or the view is
    // under the ground.
    //
    // The cover is read through a curve rather than straight. A cloud that is
    // actually in front of a star hides it below, one star at a time; this is only
    // for the sky a closed deck seals over, and it has to stay out of the way until
    // the sky really is closed or a fair night comes out as an overcast one.
    const float dark  = 1.0f - today_.light;
    const float open  = 1.0f - SmoothStep(stars.hideFrom, stars.hideAt, now_.cover);
    const float shown = dark * open;

    if (shown <= 0.004f) return;

    const float parallax = std::clamp(stars.parallax, 0.01f, 1.0f);
    const float step     = std::max(stars.spacing, 4.0f) * parallax;
    const float grain    = std::max(stars.size, 1.0f);

    // The field's own frame: the world scaled about the horizon, so the horizon
    // stays put and everything above it draws in towards it. That is what distance
    // does, and it is the whole of the parallax.
    const float highest = air.horizon + (view.y - air.horizon) * parallax;
    const float lowest  = air.horizon;

    // Nothing above the horizon is in view at all, which underground is always.
    if (lowest <= highest) return;

    const int i0 = static_cast<int>(std::floor(view.x * parallax / step));
    const int i1 = static_cast<int>(std::ceil((view.x + view.width) * parallax / step));
    const int j0 = static_cast<int>(std::floor(highest / step));
    const int j1 = static_cast<int>(std::ceil(lowest / step));

    // The band a cloud can stand in. A star outside it has nothing to be behind, and
    // most of them are: the deck is a ribbon near the top of the sky and the sky is
    // everything above the ground.
    const float ceiling = settings_.ceiling;
    const float base    = settings_.base + settings_.rainDrop * now_.rain;

    for (int i = i0; i <= i1; i++) {
        // The weather over this stretch, read once for the column of stars rather
        // than once for each. It is the expensive half of asking about a cloud, and
        // it is the same answer all the way up.
        bool asked = false;
        Column column;

        for (int j = j0; j <= j1; j++) {
            // One to a cell, placed inside it by the cell's own hash. The same trick
            // the rain uses on its drops, and for the same reason: there are a great
            // many of them and nothing about one is worth remembering.
            const int cell = i * 73856093 ^ j * 19349663;

            const Vector2 at = {(static_cast<float>(i) + Hash(cell)) * step,
                                (static_cast<float>(j) + Hash(cell + 1)) * step};

            // Back out to the world, to be drawn there.
            const Vector2 world = {at.x / parallax, air.horizon + (at.y - air.horizon) / parallax};

            if (world.y > lowest || world.y < view.y || world.y > view.y + view.height) continue;

            // Behind the land. Being drawn after the light, a star is no longer
            // covered by the ground simply because the ground was drawn later, so
            // the ground has to be asked. The world's own surface, edits and all —
            // the same one the rain lands on.
            if (world.y > GroundAt(ground, world.x, terrain_)) continue;

            // And behind a cloud, read out of the field at the star's own position,
            // which is exactly what being covered by the cloud would have meant.
            //
            // Faded across the outline rather than cut at it. The cloud on screen is
            // rasterised from a lattice twelve pixels across and interpolated
            // between, so its drawn edge and the field's exact edge disagree over
            // about one cell in fifty — and every one of those is a star left
            // burning on the rim of a cloud, which is precisely where it is noticed.
            // Fading over a band swallows the disagreement, and it is what a star
            // going behind the thin edge of a cloud does in any case.
            float behind = 0.0f;

            if (world.y > ceiling && world.y < base) {
                if (!asked) {
                    column = ColumnAt(world.x);
                    asked  = true;
                }

                behind = SmoothStep(-stars.cloudEdge, stars.cloudEdge, MarginAt(world, column));
                if (behind >= 0.996f) continue;
            }

            // Put out by the air it has to shine through, which near the ground is
            // all of it. Nothing at the horizon, rising to full over `rise`, so the
            // field ends well clear of the land instead of running down into it.
            const float altitude = std::max(air.horizon - world.y, 0.0f);

            const float through = SmoothStep(0.0f, std::max(stars.rise, 1.0f), altitude);

            // Its own brightness, and the waver of the air in front of it.
            //
            // A narrow range on purpose, and the floor is high. Given the full range
            // the field reads as noise rather than as a sky — the eye finds the
            // scatter before it finds the pattern — and a mark this small at a low
            // alpha is the sky with a slightly paler square in it, which carries no
            // colour at all. The variety is in the colour instead.
            const float magnitude = 1.0f - stars.spread * Hash(cell + 2);

            const float waver = 1.0f - stars.twinkle * 0.5f *
                                           (1.0f + std::sin(time_ * stars.twinkleRate + Hash(cell + 3) * 2.0f * kPi));

            const float alpha = shown * through * magnitude * waver * (1.0f - behind);
            if (alpha <= 0.01f) continue;

            // Its temperature, which is the whole of its colour: blue-white at one
            // end, amber at the other, and `tint` deciding how far the two ends
            // actually run apart.
            const float heat = 0.5f + (Hash(cell + 4) - 0.5f) * std::clamp(stars.tint, 0.0f, 1.0f);

            const Color colour = Mix(stars.cool, stars.hot, heat);

            // Snapped to its own grain rather than the world's, since it is smaller
            // than one of the world's pixels and has to sit still as the view moves.
            const auto m = static_cast<int>(std::floor(world.x / grain));
            const auto n = static_cast<int>(std::floor(world.y / grain));

            DrawRectangleRec({static_cast<float>(m) * grain, static_cast<float>(n) * grain, grain, grain},
                             Fade(colour, alpha));
        }
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
    // The mood's sunlight, tinted by the colour the sun has left — so a cloud goes
    // gold at the same moment the sky behind it does, off the same one number.
    const Color sun = Scale(now_.sunlight, today_.beam);

    // Then run back towards the shadow as the light goes. Not dimmed: dimming is the
    // light layer's job and this is already inside it. What happens instead is that
    // the two lights collapse together, so a cloud at night has no lit side left and
    // reads as a flat silhouette, which is what a cloud at night looks like. At noon
    // this is `sun` exactly and nothing about a daytime sky moved.
    const Color day = Mix(now_.ambient, sun, today_.light);

    return Mix(now_.ambient, day, std::clamp(lit, 0.0f, 1.0f));
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
    // Where the sun is now, not where the settings said it was. Read from the day
    // rather than written into `settings_`, which only Configure may touch: that
    // measures the field's distribution over thirty thousand samples, and calling it
    // to move the sun would cost more than the rest of the frame put together.
    //
    // It points below the horizon once the sun has set, so the probe reads the cloud
    // *beneath* a cell and the deck lights from underneath at dusk. That falls out;
    // nothing asked for it.
    const Vector2 sun     = Unit(today_.sun, {0.8f, -0.6f});
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
