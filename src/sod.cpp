#include "sod.h"

#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sod {
namespace {

// The cover a drought pulls every other one towards, and the season of it.
//
// The steppe in high summer, which is already the colour grass goes when it has
// no water — so a parched meadow is a step along a line that exists rather than a
// fifth palette invented for it.
constexpr std::size_t kParchedCover  = 1;
constexpr std::size_t kParchedSeason = static_cast<std::size_t>(flora::Season::Summer);

// Humidity below which the ground counts as parched, and above which it counts as
// watered.
//
// Read against weather::Sky::HumidityAt, which is the place's climate humidity
// moved by the rain that has actually fallen on it. The band is narrow and sits
// low: what should show is the difference between dry country and country that
// has had a shower, not a lawn that changes colour every time a cloud passes.
constexpr float kDryAt = 0.30f;
constexpr float kWetAt = 0.62f;

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

// A number in [0,1) hashed from a cell and a salt.
//
// Its own copy rather than flora's, for the reason grove.cpp gives for keeping
// its own: the two are read at different scales by different things, and a change
// made for one has no business rearranging the other. The whole horizontal axis
// is a cell index, so the mix has to be sixty-four bits — one that threw away the
// high half would repeat the same field of grass every few million pixels.
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

// Snapped to the plant grid, anchored to the world.
//
// Not optional. Without it a tuft drawn at a fractional offset lands its texels
// on different screen pixels from one frame to the next, and the whole field
// crawls as the view scrolls.
float Snap(float value) {
    return std::floor(value / config::kFloraPixel) * config::kFloraPixel;
}

// Middle of the ramp, and how far the two ends of a blade reach either side of
// it, in tone steps.
//
// The blade's own form, and it is the same idea as the dome on a mass of foliage
// read at the size a blade can carry: the tip catches the light and is what draws
// the silhouette against the sky, the foot sits in the tuft's own shadow.
constexpr float kBladeBase = 0.46f;
constexpr float kBladeTip  = 2.6f;

// How much darker a blade gets towards the edge of its tuft, in tone steps.
//
// The form at the scale above the blade: a tuft is a small mass, and a mass is
// lit across as well as up. Without it a tuft is a row of identical blades, which
// is what a comb looks like and not what grass does.
constexpr float kBladeSide = 1.3f;

// The per-texel stipple on a blade, in tone steps. Under a half for the reason
// every other grain in this project is: see ElementPaint::grain.
constexpr float kBladeGrain = 0.45f;

// Where a tuft's own phase comes from. The same mix the trees use on their cell
// index, so no two tufts beat together and the field does not pulse as one.
float Phase(std::int64_t cell, int blade) {
    const std::int64_t mixed = cell * 2654435761LL + blade * 40503LL;

    return static_cast<float>(mixed & 0xffff) / 65536.0f;
}

// How far the tip of a blade is leaning now, in world pixels.
//
// The tree's own recipe — see Lean in grove.cpp. The wind sets where the blade is
// held and the swing is about that, rather than about upright, which is what
// makes a gust read as a shove followed by a wobble instead of a shake that
// happens to coincide with one.
float Lean(float height, float push, float phase, float now) {
    const float rate  = (1.0f + std::fabs(push) * kBladeUrgency) / kBladePeriod;
    const float swing = std::sin((now * rate + phase) * 2.0f * 3.14159265f);

    return height * kBladeReach * push * (0.66f + 0.34f * swing);
}

// The four tones of one cover at one moment of the year.
//
// Blended between the season and the one after it, so that a year turning is a
// wood and a field changing together over days rather than every blade in the
// world changing colour on one frame.
void SeasonTones(const Cover &cover, flora::Season season, float blend, Color out[kElementTones]) {
    const std::size_t now  = flora::SeasonIndex(season);
    const std::size_t next = (now + 1) % flora::kSeasonCount;

    for (std::size_t i = 0; i < kElementTones; i++) {
        out[i] = soil::Blend(cover.tone[now][i], cover.tone[next][i], std::clamp(blend, 0.0f, 1.0f));
    }
}

} // namespace

soil::Ramp RampAt(const terrain::Climate &climate, flora::Season season, float blend, float wet) {
    // Every cover weighed, and the result mixed by weight rather than the best one
    // taken outright.
    //
    // A hard choice would draw a line across the world where two covers meet, and
    // there is no such line: what separates a meadow from a steppe is a stretch of
    // country that is a little of both. This is the "blend the parameters, not the
    // result" rule the generator follows for terrain, applied to colour — which
    // for something that is only ever drawn *is* the parameter.
    Color mixed[kElementTones]{};

    float total = 0.0f;

    for (const Cover &cover : kCovers) {
        const float bell = ClimateBell(cover.climate, climate.temperature, climate.humidity);
        const float weight =
            ClimateRamp(cover.climate.goneAt, cover.climate.fullAt, bell);

        if (weight <= 0.0f) continue;

        Color tones[kElementTones];
        SeasonTones(cover, season, blend, tones);

        // Accumulated as a running mix rather than as a sum divided at the end,
        // so the intermediate never leaves the range a colour can hold.
        total += weight;

        for (std::size_t i = 0; i < kElementTones; i++) {
            mixed[i] = soil::Blend(mixed[i], tones[i], weight / total);
        }
    }

    // Somewhere between every cover's range — high ground in a cold desert, say.
    // The meadow answers, because ground with soil on it and no better claim on it
    // is ordinary ground.
    if (total <= 0.0f) SeasonTones(kCovers[0], season, blend, mixed);

    // Then the drought, which pulls whatever grew towards dead straw.
    const float parch = kParch * (1.0f - SmoothStep(kDryAt, kWetAt, wet));

    if (parch > 0.0f) {
        for (std::size_t i = 0; i < kElementTones; i++) {
            mixed[i] = soil::Blend(mixed[i], kCovers[kParchedCover].tone[kParchedSeason][i], parch);
        }
    }

    return soil::Build(mixed);
}

namespace {

// What one column of the profile says, from a world position.
struct Reading {
    float top   = 0.0f;
    float cover = 0.0f;
    float push  = 0.0f;
    const soil::Ramp *ramp = nullptr;
};

// How much of one tuft is standing, in [0,1]. One where nothing has happened to
// it, which is the case for every tuft in the world until somebody swings at it.
float Standing(const Blades &ground, std::int64_t cell) {
    if (ground.standing == nullptr || ground.cells <= 0) return 1.0f;

    const std::int64_t at = cell - ground.firstCell;
    if (at < 0 || at >= ground.cells) return 1.0f;

    return ground.standing[static_cast<std::size_t>(at)];
}

Reading Read(const Blades &ground, float worldX) {
    const int column =
        std::clamp(static_cast<int>(std::floor(worldX / ground.spacing)) - ground.firstColumn, 0, ground.count - 1);

    const auto at = static_cast<std::size_t>(column);

    return {ground.top[at], ground.cover[at], ground.push[at], &ground.ramp[at]};
}

// The cell a world position falls in. The whole of a tuft's identity: the same
// number on every frame and in every session, which is what a record of cutting
// is filed under and what makes a field of grass cost nothing to keep.
std::int64_t CellAt(float worldX) {
    return static_cast<std::int64_t>(std::floor(worldX / kTuftSpan));
}

float DrawnTop(float crossing) {
    return marching_squares::DrawnTop(crossing, config::kPixelSize);
}

// Where a tuft stands and how much of it there is, or nothing where the cell is
// bare.
//
// Height is scaled by how established the grass is, so ground that is coming back
// is short before it is tall — the shape regrowth actually has, and cheaper than
// any other way of saying it.
bool Grow(std::int64_t cell, const Blades &ground, int seed, float &outX, Reading &outAt, int &outBlades,
          float &outScale) {
    outX  = (static_cast<float>(cell) + 0.5f) * kTuftSpan;
    outAt = Read(ground, outX);

    if (outAt.cover <= 0.0f) return false;

    // One roll against the cover, so tufts arrive one at a time as the ground
    // recovers rather than the whole field fading up together. A fade reads as a
    // rendering artefact; this reads as spreading.
    if (Roll(cell, 5, seed) >= outAt.cover) return false;

    // As many blades as the tuft has texel columns to stand them in, so the
    // clump is filled rather than sampled. What varies between one tuft and the
    // next is how tall they run, not how many there are.
    outBlades = kBladesPerTuft;

    // Two different things, kept apart on purpose. The cover is how established
    // the grass in this stretch of ground is, and it decides whether a tuft is
    // here at all; the standing is what is left of this one tuft after it was
    // cut, and it decides how tall it is. Folding them together would have a
    // mown tuft roll for its own existence again and come back somewhere else.
    const float left = Standing(ground, cell);
    if (left <= 0.0f) return false;

    // Never the whole way down to nothing: a tuft that has just taken hold, or
    // just been cut, is a short tuft and not an invisible one.
    outScale = (0.55f + 0.45f * outAt.cover) * left;

    return true;
}

} // namespace

void DrawTufts(const Blades &ground, Rectangle view, float now, int seed) {
    if (ground.Empty()) return;

    const float pixel = config::kFloraPixel;

    // A margin, because a blade leans out of the cell it grew in.
    const float reach = static_cast<float>(kBladeTall) * pixel * (kBladeReach + kBladeCurve) + kTuftSpan;

    const std::int64_t from = CellAt(view.x - reach);
    const std::int64_t to   = CellAt(view.x + view.width + reach);

    for (std::int64_t cell = from; cell <= to; cell++) {
        float baseX      = 0.0f;
        Reading at       = {};
        int blades       = 0;
        float scale      = 1.0f;

        if (!Grow(cell, ground, seed, baseX, at, blades, scale)) continue;

        const soil::Ramp &ramp = *at.ramp;

        // How tall this tuft runs.
        //
        // Biased short, so a field is mostly cover with the occasional stand of
        // taller grass in it rather than one even height everywhere — but only
        // mildly. Squaring the roll put the median crest under four texels and
        // what that drew was a mown lawn: the taper and the per-blade nick take
        // two or three texels out of every blade, so a crest has to start well
        // clear of them or there is nothing left to be ragged.
        const float reach = Roll(cell, 7, seed);

        const int crest = 3 + static_cast<int>(std::pow(reach, 1.3f) * static_cast<float>(kBladeTall - 2));

        for (int blade = 0; blade < blades; blade++) {
            const int salt = 11 + blade * 7;

            // One blade per texel column of the tuft, in order, rather than a
            // handful thrown anywhere inside it.
            //
            // This was a scatter, and what it drew was a spray of separate
            // uprights with air between them — a comb, not grass. Grass is a mass
            // with a ragged top: dense where it meets the ground and open where it
            // does not, which is what filling every column and varying only the
            // height gives. It is also what the reference art does, and the same
            // rule the canopy is built on — dense masses with a few holes, never a
            // loose spray.
            const float across = (static_cast<float>(blade) + 0.5f) / static_cast<float>(blades);

            const float side = across * 2.0f - 1.0f;
            const float dx   = side * kTuftSpan * 0.5f;

            // The ground under *this* blade, not under the middle of the tuft it
            // belongs to.
            //
            // A tuft is ten pixels across and the surface is terraced into risers
            // a good deal taller than that, so a clump straddling a step had half
            // its blades planted on the height of the other half — which is most
            // of what was seen floating. It costs one lookup per blade and it is
            // the only reading that is true of where the blade actually is.
            const Reading under = Read(ground, baseX + dx);

            if (under.cover <= 0.0f) continue;

            const float foot = DrawnTop(under.top);

            // The tuft's own crest, tapered towards its edges and then nicked a
            // texel or two per blade.
            //
            // Correlated on purpose. Heights rolled independently give a clump
            // with no shape of its own — five blades that happen to stand
            // together — and what makes grass read as grass is that a clump has a
            // crest and a ragged edge, in that order.
            const float taper = 1.0f - std::fabs(side) * kTuftTaper;

            const int nick = static_cast<int>(Roll(cell, salt + 1, seed) * 2.5f);

            int texels = static_cast<int>(std::lround(static_cast<float>(crest) * taper)) - nick;
            texels     = std::max(static_cast<int>(std::lround(std::min(texels, kBladeTall) * scale)), 1);

            const float height = static_cast<float>(texels) * pixel;

            const float curve = (Roll(cell, salt + 2, seed) - 0.5f) * 2.0f * kBladeCurve * height;
            const float lean  = Lean(height, at.push, Phase(cell, blade), now);

            float previousX = Snap(baseX + dx);

            for (int k = 0; k < texels; k++) {
                // How far up the blade this texel sits, in [0,1] from the foot.
                const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(texels);

                // Squared, so the foot barely moves and the tip carries the bend.
                // A linear ramp reads as the whole blade sliding sideways, which
                // is the same fault the trees were drawn with before their bands
                // were weighted this way.
                const float bend = (lean + curve) * t * t;

                const float x = Snap(baseX + dx + bend);
                const float y = foot - static_cast<float>(k + 1) * pixel;

                float lit = kBladeBase + t * kBladeTip * soil::kStep;

                lit -= std::fabs(side) * kBladeSide * soil::kStep;

                lit += (Roll(cell, 41 + blade * 13 + k, seed) - 0.5f) * 2.0f * kBladeGrain * soil::kStep;

                const int index =
                    std::clamp(static_cast<int>(lit * static_cast<float>(kElementRamp)), 0, kElementRamp - 1);

                const Color tone = ramp.tone[index];

                // The whole run this row crosses, and not just the texel it ends
                // on.
                //
                // A blade is a line and not a column of samples. The bend grows as
                // the square of the height, so near the tip one row carries the
                // blade two or three texels sideways — and one square per row
                // leaves the top of a blade as loose specks with sky between them
                // and the rest of it. Measured over twelve hundred columns of
                // ordinary ground, that broke the tip off nearly half of them.
                //
                // Filled from where the row below ended to where this one does,
                // *inclusive of both*: consecutive rows then share a column and
                // the blade is connected by construction. Drawn along this row
                // rather than the one under it, so nothing is painted into the
                // ground the blade is standing on.
                const float from = std::min(previousX, x);
                const float to   = std::max(previousX, x);

                for (float fill = from; fill <= to + pixel * 0.5f; fill += pixel) {
                    DrawRectangleV({fill, y}, {pixel, pixel}, tone);
                }

                previousX = x;
            }
        }
    }
}

int Cut(const Blades &ground, Rectangle hitbox, int seed, std::int64_t *into, bool *ripe, int room) {
    if (ground.Empty() || room <= 0) return 0;

    const float pixel = config::kFloraPixel;

    const std::int64_t from = CellAt(hitbox.x - kTuftSpan);
    const std::int64_t to   = CellAt(hitbox.x + hitbox.width + kTuftSpan);

    int taken = 0;

    for (std::int64_t cell = from; cell <= to && taken < room; cell++) {
        float baseX = 0.0f;
        Reading at  = {};
        int blades  = 0;
        float scale = 1.0f;

        if (!Grow(cell, ground, seed, baseX, at, blades, scale)) continue;

        // The tuft as a box: as wide as its own spread and as tall as its tallest
        // blade could be. Tested upright rather than as it is leaning, because a
        // swing that connects or misses depending on where the wind happened to
        // be holding the grass is a swing the player cannot aim.
        const float tall = static_cast<float>(kBladeTall) * pixel * scale;
        const float foot = DrawnTop(at.top);

        const Rectangle tuft = {baseX - kTuftSpan * 0.5f, foot - tall, kTuftSpan, tall};

        if (!CheckCollisionRecs(hitbox, tuft)) continue;

        // Grow has already refused a cell whose tuft is gone, so anything reaching
        // here is grass that is standing — which is what stops one patch of ground
        // being harvested twice.
        into[taken] = cell;
        ripe[taken] = at.cover >= kRipe;

        taken++;
    }

    return taken;
}

} // namespace sod
