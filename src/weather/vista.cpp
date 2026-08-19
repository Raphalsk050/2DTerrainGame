#include "weather/vista.h"

#include "core/config.h"
#include "flora/flora.h"
#include "core/pool.h"
#include "core/profile.h"
#include "world/sod.h"

#include <algorithm>
#include <cmath>

namespace {

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

terrain::NoiseShape Reseed(terrain::NoiseShape shape, int seed) {
    shape.seed += seed;

    return shape;
}

// The threshold a texel is dithered against, in [0,1): the ordered 4x4 matrix,
// read in **the row's own texel frame**.
//
// Both halves of that were arrived at by getting one of them wrong. The matrix was
// swapped for a hash first, on the theory that an ordered pattern is what magnifies
// into a screen door at three screen pixels per texel — ranges seen through
// sandblasted glass, which is what it was called and what it looked like. The hash
// did take the lattice out and it took the drawing with it: an ordered dither is
// what carries a value cleanly between two tones, and scattered at random the same
// proportion of texels reads as dirt on the picture rather than as a shade.
//
// The lattice was never the fault. **The frame was.** Keyed on the world's own
// texel indices, the pattern sat where it was while the range slid along behind it,
// so what the eye had to explain was a fixed speckled pane in front of moving
// scenery — and a *regular* pane is simply a more legible one than a random pane,
// which is why swapping the pattern seemed to help. A row's frame is the world's
// shifted by the part of the camera that row does not take; in it, the matrix
// travels with the silhouette and reads as what it is.
//
// The shift is rounded to whole texels because the pattern is a lattice of them. A
// fractional one would resample it every frame, which is a shimmer — a different
// fault, and a worse one than the drift.
//
// Written out rather than derived: the bit-interleaving that generates a Bayer
// matrix is three lines of shifts nobody can read, and the whole of what the table
// has to be is *this* permutation — sixteen thresholds arranged so that no two near
// values are near each other on the grid.
constexpr int kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float Threshold(int m, int n) {
    return (static_cast<float>(kBayer[(m & 3) + (n & 3) * 4]) + 0.5f) / 16.0f;
}

// A value quantised to `steps` levels with the dither carried across the step.
//
// The dither is added *inside* the rounding rather than to the result, which is
// the whole trick: what it perturbs is which side of a boundary a texel falls on,
// so a run of values sitting between two levels comes out as a mix of the two in
// the pattern's own proportion. Added afterwards it would only be noise on a
// banded picture.
float Quantise(float v, float steps, float grain, float dither) {
    const float levels = std::max(steps, 1.0f);

    return std::clamp(std::floor(v * levels + (grain - 0.5f) * dither + 0.5f) / levels, 0.0f, 1.0f);
}

Color Dim(Color colour, float by) {
    const auto scale = [&](unsigned char channel) {
        return static_cast<unsigned char>(std::clamp(static_cast<float>(channel) * by, 0.0f, 255.0f));
    };

    return {scale(colour.r), scale(colour.g), scale(colour.b), colour.a};
}

// How far a column's whole lift is brought down by being a desert. One where
// nothing is sand, `duneRise` where everything is.
//
// Its own function because the shading needs the same number: the volume under a
// crest is measured over the row's amplitude, and a row scaled to two fifths of
// its height with the gradient still spread over the whole of it is a dune with no
// shape on it at all.
float DuneScale(float dunes, const vista::Settings &settings) {
    return 1.0f + (settings.duneRise - 1.0f) * std::clamp(dunes, 0.0f, 1.0f);
}

bool Same(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

} // namespace

void vista::Range::Configure(const Settings &settings, const terrain::Settings &terrain) {
    settings_ = settings;
    terrain_  = terrain;

    // The same line the air is measured from, so a world with its ground raised
    // gets its horizon raised and its ranges with it. See Sky::Configure, which
    // takes the identical number for the identical reason.
    horizon_ = terrain.surface.level;

    // The two materials whose colour a range does not get from the country it
    // stands in. Rock is rock everywhere, and snow is the snow row's own paint —
    // what the *climate* decides is whether there is any of it, not what colour it
    // is when there is.
    rock_ = soil::Build(Def(Element::Rock).paint.tone);
    snow_ = soil::Build(Def(Element::Snow).paint.tone);
}

float vista::Range::Sample(float worldX, Rectangle view, const LayerDef &def) const {
    const float camX = view.x + view.width * 0.5f;

    // What is drawn at `worldX` is the content at `worldX` shifted back by the part
    // of the camera's travel this layer does not take. At a distance of one the
    // shift is nothing and the layer is pinned to the world; at zero it is the
    // whole of it and the layer is pinned to the frame.
    return worldX - (1.0f - std::clamp(def.distance, 0.0f, 1.0f)) * camX;
}

float vista::Range::Slide(Rectangle view, const LayerDef &def) const {
    const float camY = view.y + view.height * 0.5f;

    // The same argument on the other axis, and measured from the horizon so that
    // the horizon is what stays put. A far layer's own horizon tracks the camera,
    // which is what keeps it in the same part of the frame however far the player
    // climbs — which is what distance does.
    return (1.0f - std::clamp(def.distance, 0.0f, 1.0f)) * (camY - horizon_);
}

float vista::Range::Fold(float sample, const LayerDef &def) const {
    const Settings &s = settings_;

    const float span = std::max(def.span, 1.0f);

    float frequency = terrain::kFeatureSpan / span;
    float amplitude = 0.5f;

    float sum  = 0.0f;
    float norm = 0.0f;

    // How much of the octave under this one survives. One at the top, and then
    // whatever the octave above left standing — see Settings::sharp.
    float above = 1.0f;

    for (int octave = 0; octave < std::max(s.octaves, 1); octave++) {
        // Stop before the fold starts describing something too narrow to be a
        // mountain. The first octave always runs, whatever the row's span, so a row
        // is never left with nothing.
        if (octave > 0 && terrain::kFeatureSpan / frequency < s.finest) break;

        const terrain::NoiseShape shape = {.frequency = frequency,
                                           .octaves   = 1,
                                           .seed      = terrain_.seed + def.seed + octave * 977};

        // The crease. `1 - |signed|` folds the field about its own zero set, so
        // what comes out is a run of sharp ridges with broad valleys between them
        // rather than the smooth swell a summed field gives at any amplitude. It is
        // the same fold `terrain::Mountains` uses on the ground, and deliberately:
        // the range on the skyline and the one the player is standing on should be
        // made of the same shape.
        float value = 1.0f - std::fabs(terrain::Signed({sample, 0.0f}, shape));

        value *= above;
        above = std::clamp(value * s.sharp, 0.0f, 1.0f);

        sum += amplitude * value;
        norm += amplitude;

        frequency *= s.lacunarity;
        amplitude *= s.gain;
    }

    if (norm <= 0.0f) return 0.0f;

    return std::pow(std::clamp(sum / norm, 0.0f, 1.0f), std::max(s.crestRound, 0.05f));
}

// The other profile a range can have, and the whole of what makes a desert a
// desert from a distance.
//
// A plain summed field and not a folded one. Everything about `Fold` is built to
// put a crease in the outline; a dune has no crease anywhere on it, and no amount
// of amplitude or sharpness applied to a ridged multifractal will take one out.
// The exponent is under one for the same reason the ground's `ridgeSharp` is —
// it lifts the mid-range, so what stands between two hollows is a long curve with
// a top rather than a bump.
float vista::Range::Dune(float sample, const LayerDef &def) const {
    const Settings &s = settings_;

    const terrain::NoiseShape shape = {.frequency = terrain::kFeatureSpan /
                                                    std::max(def.span * s.duneSpan, 1.0f),
                                       .octaves = 2,
                                       .seed    = terrain_.seed + def.seed + 313};

    return std::pow(std::clamp(terrain::Sample({sample, 0.0f}, shape), 0.0f, 1.0f), std::max(s.duneRound, 0.05f));
}

float vista::Range::Country(float sample, const LayerDef &def) const {
    return sample / std::max(std::clamp(def.distance, 0.0f, 1.0f), 0.02f);
}

terrain::Climate vista::Range::Regional(float country) const {
    const terrain::ClimateSettings &c = terrain_.climate;

    const Vector2 at = {country, 0.0f};

    return {.temperature = std::clamp(terrain::Sample(at, Reseed(c.temperature, terrain_.seed)), 0.0f, 1.0f),
            .humidity    = std::clamp(terrain::Sample(at, Reseed(c.humidity, terrain_.seed)), 0.0f, 1.0f)};
}

float vista::Range::Dunes(const terrain::Climate &climate) const {
    // The sand row's own bell, ramped by the sand row's own pair of edges. Not a
    // second opinion about where the deserts are: it is the identical expression
    // that decides whether there is sand on the ground here, so the horizon changes
    // shape over exactly the stretch of world the ground changes material over. A
    // desert widened in `kElements` widens here with nothing edited.
    const ElementSpawn &wants = Def(Element::Sand).spawn;

    const float edge = std::max(settings_.duneEdge, 0.0f);

    return ClimateRamp(wants.climate.goneAt - edge, wants.climate.fullAt + edge,
                       ClimateBell(wants.climate, climate.temperature, climate.humidity));
}

float vista::Range::Lift(float sample, const LayerDef &def, float dunes) const {
    const Settings &s = settings_;

    const terrain::NoiseShape swell = {.frequency = std::max(terrain::kFeatureSpan / std::max(def.span, 1.0f), 1e-3f) *
                                                   s.swellOf,
                                       .octaves = 1,
                                       .seed    = terrain_.seed + def.seed + 51};

    // The wide swell over the crests, which is what turns an even hedge of teeth
    // into a run of massifs with low country between them. A range is not evenly
    // high along its length and nothing in the fold above says so. It runs over the
    // dunes too — a sand sea also has high stretches and flat ones.
    const float massif = s.swellFloor + s.swellSwing * std::clamp(terrain::Sample({sample, 0.0f}, swell), 0.0f, 1.0f);

    const float share = std::clamp(dunes, 0.0f, 1.0f);

    const float crest = Fold(sample, def);
    const float shape = crest + (Dune(sample, def) - crest) * share;

    // And the whole lift comes down with it, foot included. A desert whose horizon
    // stood as high as a mountain range's would be a range of dunes, which is a
    // thing nowhere has.
    return (def.foot + def.amp * shape * massif) * DuneScale(share, s);
}

bool vista::Range::Visible(Rectangle view) const {
    if (!settings_.on) return false;

    // Nothing above the deepest the stack is ever drawn to. Underground this is the
    // whole of the early out and it costs two comparisons.
    if (view.y >= horizon_ + settings_.reach) return false;

    // And nothing below the highest crest any row could reach here.
    float highest = horizon_ + settings_.reach;

    for (const LayerDef &def : kRanges) {
        // The bound, and it is a bound rather than a height: a column may be a
        // desert, and a desert only ever brings the lift *down* (see DuneScale), so
        // the undesert case is the highest anything here can ever stand.
        highest = std::min(highest, horizon_ + Slide(view, def) - def.foot - def.amp);
    }

    return view.y + view.height > highest;
}

float vista::Range::CrestAt(float worldX, Rectangle view) const {
    float crest = horizon_ + settings_.reach;

    if (!settings_.on) return crest;

    for (const LayerDef &def : kRanges) {
        const float sample = Sample(worldX, view, def);

        const float dunes = Dunes(Regional(Country(sample, def)));

        crest = std::min(crest, horizon_ + Slide(view, def) - Lift(sample, def, dunes));
    }

    return crest;
}

void vista::Range::Draw(Rectangle view, const weather::Sky &sky) const {
    if (!Visible(view)) return;

    const Settings &s = settings_;

    const float pixel = std::max(config::kPixelSize, 1.0f);

    // The world's own texel lattice, and every value below is read at the centre of
    // one of its squares. That is what makes the silhouette a staircase of real
    // steps rather than an aliased diagonal, and it is the same anchoring the
    // ground, the cloud and the grass are drawn on — a horizon on a grid of its own
    // would be the one thing in the frame not made of the same pixels.
    const int m0 = static_cast<int>(std::floor(view.x / pixel));
    const int m1 = static_cast<int>(std::ceil((view.x + view.width) / pixel));
    const int n0 = static_cast<int>(std::floor(view.y / pixel));
    const int n1 = static_cast<int>(std::ceil((view.y + view.height) / pixel));

    const int wide = m1 - m0;
    const int tall = n1 - n0;

    if (wide <= 0 || tall <= 0) return;

    // The lowest row the stack may reach, whatever the ground under it does.
    const int floorRow = std::min(tall, static_cast<int>(std::ceil((horizon_ + s.reach) / pixel)) - n0);

    if (floorRow <= 0) return;

    const auto layers = static_cast<int>(kRangeCount);

    lifts_.assign(static_cast<std::size_t>(layers) * static_cast<std::size_t>(wide + 2), 0.0f);
    climates_.assign(static_cast<std::size_t>(layers) * static_cast<std::size_t>(wide + 2), terrain::Climate{});
    floors_.assign(static_cast<std::size_t>(wide), 0);
    ceilings_.assign(static_cast<std::size_t>(wide), 0);
    air_.assign(static_cast<std::size_t>(tall), BLANK);
    paint_.assign(static_cast<std::size_t>(wide) * static_cast<std::size_t>(tall), BLANK);

    // Where each layer's own horizon has slid to, once for the view rather than
    // once per column.
    float slid[kRangeCount]{};

    for (int layer = 0; layer < layers; layer++) slid[layer] = Slide(view, kRanges[layer]);

    // ------------------------------------------------------------------- the air
    //
    // The sky exactly as DrawAtmosphere drew it, band for band and scaled by the
    // same day — because what the haze mixes towards has to *be* what is behind the
    // range and not a second opinion about it. Read once per texel row and not once
    // per texel: the air is a function of height alone, and the alternative is a
    // quarter of a million exponentials a frame for three hundred distinct answers.
    {
        const weather::Atmosphere &atmosphere = sky.Config().air;

        const float band = std::max(atmosphere.bandHeight, 1.0f);

        const float cover = sky.Now().cover;

        for (int row = 0; row < tall; row++) {
            const float worldY = (static_cast<float>(n0 + row) + 0.5f) * pixel;

            // Snapped to the band it falls in, so a texel takes the colour of the
            // stripe the sky drew there rather than a sample between two of them.
            const float middle = std::floor(worldY / band) * band + band * 0.5f;

            // Kept at full brightness here and scaled with everything else at the
            // end. Dimming the haze alone is the shape of the bug this had: a far
            // row is mostly haze and went dark on time, a near row is barely any and
            // stayed at noon — so at midnight the foothills were the brightest thing
            // on a black hillside and the mistake read as the near rows glowing
            // rather than as the day being applied in the wrong place.
            air_[static_cast<std::size_t>(row)] = sky.AirAt(middle, cover);
        }
    }

    // How bright the day is, floored at whatever the night is worth.
    //
    // The same two lines DrawAtmosphere uses and deliberately not a variation on
    // them: the ranges stand in the air, so they take the light the air takes.
    // Everything below is worked out at full brightness and scaled by this at the
    // very end, which is also the order the sky does it in.
    const float night = std::clamp(sky.Config().air.night, 0.0f, 1.0f);
    const float day   = night + (1.0f - night) * std::clamp(sky.Today().light, 0.0f, 1.0f);

    // Which way the light is coming from, and how squarely.
    //
    // The day's own sun and not a constant: the ranges light from the left in the
    // morning and from the right in the afternoon, off the same vector the clouds
    // are shaded by, and they go flat at noon and at midnight because the sun is
    // overhead or gone. Nothing here had to be told about the hour.
    const float sun = std::clamp(sky.Today().sun.x, -1.0f, 1.0f);

    const auto turn      = sky.Turn();
    const auto season    = static_cast<flora::Season>(turn.index);
    const float turning  = turn.blend;

    // ------------------------------------------------------- the heightfields
    //
    // Every column of every row, including one either side of the view: the slope a
    // face is shaded by is the difference between its neighbours, and computing it
    // from two extra evaluations per texel would triple the noise this module
    // spends. Across the cores, one column each; a worker writes only its own
    // column and `Lift` is a pure function of the settings and the position.
    {
        PROFILE_ZONE("vista lift");

        pool::For(wide + 2, [&](int k) {
            const float worldX = (static_cast<float>(m0 + k - 1) + 0.5f) * pixel;

            for (int layer = 0; layer < layers; layer++) {
                const LayerDef &def = kRanges[layer];

                const float sample = Sample(worldX, view, def);


                // Read here and kept, because the shading wants it too: the shape
                // asks it whether this is a desert and the colour asks it what the
                // ground is made of and whether anything up there is cold. At the
                // row's own country and never at the drawn position — see Country,
                // which is the whole reason a range stands still.
                const terrain::Climate climate = Regional(Country(sample, def));

                const std::size_t row = static_cast<std::size_t>(layer) * static_cast<std::size_t>(wide + 2);

                climates_[row + static_cast<std::size_t>(k)] = climate;

                lifts_[row + static_cast<std::size_t>(k)] = Lift(sample, def, Dunes(climate));
            }
        });
    }

    // ---------------------------------------------------------------- the shading
    //
    // **Near to far, with a ceiling per column.** Every row is opaque, so a texel
    // covered by the range in front of it is a texel the ones behind never have to
    // be asked about: the column is filled from the nearest crest upwards and each
    // layer only shades what is still open above it. Drawn the natural way round —
    // far to near, each row painting over the last — the same texel is shaded five
    // times and four of those are thrown away.
    {
        PROFILE_ZONE("vista shade");

        pool::For(wide, [&](int column) {
            const float worldX = (static_cast<float>(m0 + column) + 0.5f) * pixel;

            // Cut at the ground, and this is what keeps the cost of a horizon to the
            // sky it is actually drawn in. Everything below the generated surface is
            // covered opaquely by the terrain or by World::DrawUnderground behind
            // it, so shading it would be a screenful of work nobody ever sees.
            //
            // The *generated* surface and not the built one, deliberately: a hole
            // somebody dug shows the deep rock the underground fill paints, which is
            // what is behind the world there. A range seen through a doorway would
            // be a mountain inside the hill.
            const float surface = terrain::Height(worldX, terrain_) + s.sink;

            int open = std::min(floorRow, static_cast<int>(std::ceil(surface / pixel)) - n0);
            open     = std::clamp(open, 0, tall);

            floors_[static_cast<std::size_t>(column)]   = open;
            ceilings_[static_cast<std::size_t>(column)] = open;

            if (open <= 0) return;

            for (int layer = layers - 1; layer >= 0; layer--) {
                if (ceilings_[static_cast<std::size_t>(column)] <= 0) break;

                const LayerDef &def = kRanges[layer];

                const std::size_t row = static_cast<std::size_t>(layer) * static_cast<std::size_t>(wide + 2);

                const float lift = lifts_[row + static_cast<std::size_t>(column + 1)];

                const float apex = horizon_ + slid[layer] - lift;

                // The texel row the crest actually landed in, before it is clipped
                // to the view. Everything below is measured in rows from *this*, and
                // it is kept unclipped for that reason alone: a crest above the top
                // of the frame still has to say where its shading is being counted
                // from, or the whole face rearranges itself the moment the summit
                // leaves the screen.
                const int crest = static_cast<int>(std::ceil(apex / pixel - 0.5f)) - n0;

                const int from = std::max(crest, 0);

                const int until = ceilings_[static_cast<std::size_t>(column)];

                if (from >= until) continue;

                const float sample = Sample(worldX, view, def);

                // The row's own texel frame: the world's, shifted by the travel this
                // row does not take. Rounded to whole texels because the grain is a
                // lattice of them — a fractional shift would resample the pattern
                // every frame, which is the shimmer this is avoiding rather than the
                // drift it is fixing.
                const float behind = 1.0f - std::clamp(def.distance, 0.0f, 1.0f);

                const int shiftX = static_cast<int>(std::lround(behind * (view.x + view.width * 0.5f) / pixel));
                const int shiftY = static_cast<int>(std::lround((horizon_ + slid[layer]) / pixel));

                // What the country under *this* row is made of, worked out only once
                // it is known that the row paints anything here at all.
                //
                // Per row and not once for the column, because each row stands in a
                // different country — see Country. The laziness is what keeps that
                // from costing five times over: with the ceiling closing from the
                // front, most columns get past one or two rows before there is
                // nothing left open above them, and a row that paints nothing needs
                // no palette.
                const terrain::Climate climate = climates_[row + static_cast<std::size_t>(column + 1)];

                const float dunes = Dunes(climate);
                const float scale = DuneScale(dunes, s);

                const sod::Look look =
                    sod::LookAt(climate, season, turning, sky.HumidityAt(Country(sample, def)));

                const Color coverDark  = look.ramp.tone[1];
                const Color coverLight = look.ramp.tone[5];

                // And whether it is cold enough up there to hold any snow, off the
                // snow row's own bell. The crest is the layer's business — a foothill
                // is below the line wherever it stands — so what is asked here is
                // only the half a bell can answer. See CLAUDE.md §9.
                const ElementSpawn &wants = Def(Element::Snow).spawn;

                const float cold = ClimateRamp(wants.climate.goneAt, wants.climate.fullAt,
                                               ClimateBell(wants.climate, climate.temperature, climate.humidity));

                // Rise per pixel run, positive going up to the right — so a face
                // rising rightwards is turned to the left and is dark under a sun on
                // the right. Read off the neighbours already computed rather than
                // sampled again.
                const float slope = (lifts_[row + static_cast<std::size_t>(column + 2)] -
                                     lifts_[row + static_cast<std::size_t>(column)]) /
                                    (2.0f * pixel);

                // The two wandering lines, per column and per layer. Both are the
                // same idea: a boundary drawn at a height alone is a horizontal line
                // across a mountain, and there is no such thing.
                // Both read `Signed` and not `Sample`, and that is not a detail. The
                // folded field is crowded hard around its own midpoint — it is a sum
                // of octaves whose peaks rarely line up — so `Sample - 0.5` visits
                // about a seventh of its nominal swing and a line jittered by it
                // comes out very nearly straight. It is the same trap the wind
                // envelope and the cloud cutoffs are measured to avoid, arrived at
                // from the other side: there the fix is to measure the field, here
                // it is to use the one whose zero set is reachable.
                const float wander = terrain::Signed({sample, 0.0f}, Reseed(s.line, terrain_.seed + def.seed));
                const float drift  = terrain::Signed({sample, 0.0f}, Reseed(s.drift, terrain_.seed + def.seed));

                const float snowLine = s.snowLine + drift * s.snowJitter;

                // How much of the snow this face keeps once it is over the line. A
                // cliff sheds it, and this is most of what makes a cap read as snow
                // rather than as a second colour of rock.
                const float holds = 1.0f - SmoothStep(s.snowSheds, s.snowBare, std::fabs(slope));

                const float white = std::clamp(def.snow, 0.0f, 1.0f) * cold * holds;

                for (int texel = from; texel < until; texel++) {
                    const float worldY = (static_cast<float>(n0 + texel) + 0.5f) * pixel;

                    // Above the layer's own horizon, which is the frame everything
                    // about its colour is measured in: a distant range and a near
                    // one both have their rock above their green, and that is what
                    // altitude looks like from here.
                    //
                    // In whole texels too, off the same rounded horizon the grain is
                    // keyed to, and for the reason `depth` is: the row's horizon
                    // slides continuously as the camera rises, so the treeline and
                    // the snow line would sweep through the picture between texels
                    // and in every column at once. Counted in rows they step where
                    // the picture steps.
                    const float above = static_cast<float>(shiftY - (n0 + texel)) * pixel;

                    // And below its own crest, which is what everything about its
                    // light is measured in — **counted in whole texels down from
                    // the row the crest was drawn in**, not as the distance to where
                    // the crest mathematically is.
                    //
                    // This is the fix for a flicker that only showed on *vertical*
                    // movement, and the asymmetry is what named it. `apex` slides
                    // continuously as the camera rises while the texels stand on the
                    // world's grid, so the true distance to it drifts through a
                    // texel's worth — and it drifts by the *same amount in every
                    // column at once*, because every column's apex moves together.
                    // The lit rim and the volume under it are steep functions of it,
                    // so the whole face changed tone in step and the eye read a
                    // flash. Sideways the same drift is spread over the columns at
                    // every phase, which is why it reads as texture there and was
                    // invisible.
                    //
                    // Counted in rows it is an integer, so the shading steps exactly
                    // when the silhouette steps and never between. Which is also the
                    // rule the rest of the picture is drawn by: a lit rim is the top
                    // so many texels of a face, not the top so many pixels of a
                    // curve that happens to be sampled there.
                    const float depth = static_cast<float>(texel - crest) * pixel;

                    const float grain = Threshold(m0 + column - shiftX, n0 + texel - shiftY);

                    // The face turned to the sun, brightened.
                    float tone = s.ambient - std::clamp(slope * sun, -1.5f, 1.5f) * s.slopeLight;

                    // The volume of the mass under the crest: light at the top,
                    // falling away into the valley.
                    tone += (1.0f - SmoothStep(0.0f, def.amp * scale * s.volumeOf, depth)) * s.volume;

                    // The mottle, and it is the only term here that varies across a
                    // face rather than down it. Without it a range is a set of
                    // vertical ramps and the eye finds the ramp first.
                    tone += (terrain::Sample({sample, -above}, Reseed(s.patch, terrain_.seed)) - 0.5f) * s.mottle;

                    // And the lit rim along the very top of the silhouette.
                    tone += (1.0f - SmoothStep(pixel * s.rimFrom, pixel * s.rimTo, depth)) * s.rim;

                    const float lit = Quantise(std::clamp(tone, 0.0f, 1.0f), static_cast<float>(std::max(s.tones - 1, 1)),
                                               grain, s.ditherTone);

                    // Where between the cover and the bare rock this texel stands.
                    float alt = (above - s.ground) / std::max(s.bare - s.ground, 1.0f);

                    alt += wander * s.lineJitter;

                    // And no rock in a dune, however high it stands. The treeline
                    // above is a fact about a mountain; sand has no such line, and a
                    // grey top on a dune would put a crag back in the one place the
                    // shape was changed to take it out of.
                    alt *= 1.0f - dunes;

                    alt = Quantise(std::clamp(alt, 0.0f, 1.0f), static_cast<float>(s.bands), grain, s.ditherGround);

                    Color colour = soil::Blend(soil::Blend(coverDark, rock_.tone[1], alt),
                                               soil::Blend(coverLight, rock_.tone[5], alt), lit);

                    // The cap. Thresholded against the dither rather than blended,
                    // because snow on a mountain has an edge and a fade would draw a
                    // grey band round every peak.
                    const float cap = SmoothStep(snowLine, snowLine + s.snowFade, above) * white;

                    if (grain * s.ditherSnow + (1.0f - s.ditherSnow) * 0.5f < cap) {
                        colour = soil::Blend(snow_.tone[2], snow_.tone[6], lit);
                    }

                    // Then the distance, into the air as it is actually drawn — and
                    // then the day over the whole of it, rock and haze alike.
                    colour = soil::Blend(Dim(colour, def.lum), air_[static_cast<std::size_t>(texel)], def.haze);

                    colour = Dim(colour, day);

                    colour.a = 255;

                    paint_[static_cast<std::size_t>(texel) * static_cast<std::size_t>(wide) +
                           static_cast<std::size_t>(column)] = colour;
                }

                ceilings_[static_cast<std::size_t>(column)] = from;
            }
        });
    }

    // ---------------------------------------------------------------- the drawing
    //
    // One upload and one blit. What used to be here was a walk of every column
    // finding runs of one colour and submitting each as a rectangle, which is how
    // everything else in this project rasterises and is the wrong shape for this
    // one — see `picture_` for the measurement and for why the runs can never be
    // long enough to pay for themselves.
    //
    // Exact, on the same four conditions World::PaintChunks rests on: one texel per
    // square of the grid the colour was worked out on, point sampling, an opaque
    // colour wherever anything was painted and a clear one everywhere else, and a
    // destination rectangle that is the grid's own extent. A screen pixel therefore
    // takes the texel whose square it falls in, which is the square the rectangle
    // would have covered it with.
    {
        PROFILE_ZONE("vista blit");

        if (picture_.id == 0 || width_ != wide || height_ != tall) {
            if (picture_.id != 0) UnloadTexture(picture_);

            const Image blank = {.data    = paint_.data(),
                                 .width   = wide,
                                 .height  = tall,
                                 .mipmaps = 1,
                                 .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

            picture_ = LoadTextureFromImage(blank);

            SetTextureFilter(picture_, TEXTURE_FILTER_POINT);

            width_  = wide;
            height_ = tall;
        } else {
            UpdateTexture(picture_, paint_.data());
        }

        const Rectangle source = {0.0f, 0.0f, static_cast<float>(wide), static_cast<float>(tall)};

        const Rectangle target = {static_cast<float>(m0) * pixel, static_cast<float>(n0) * pixel,
                                  static_cast<float>(wide) * pixel, static_cast<float>(tall) * pixel};

        DrawTexturePro(picture_, source, target, {0.0f, 0.0f}, 0.0f, WHITE);
    }
}

void vista::Range::Unload() {
    if (picture_.id == 0) return;

    UnloadTexture(picture_);

    picture_ = {};
    width_   = 0;
    height_  = 0;
}
