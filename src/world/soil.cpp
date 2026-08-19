#include "world/soil.h"

#include "world/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace soil {
namespace {

// Where the key light comes from.
//
// The same direction the canopy bakes its leaves against, and it has to be the
// same: a tree standing on a hillside is lit from one place and the hillside from
// another only if these two disagree. Very nearly straight down, with just enough
// lean to tell a slope facing left from one facing right.
//
// Never the sun's actual position. What is painted here is form, which is true at
// every hour; the hour arrives later, as one multiply over the whole frame.
constexpr float kKeyX = -0.20f;
constexpr float kKeyY = -0.98f;

// Middle of the ramp, which is what a texel with no form and no texture gets.
constexpr float kBase = 0.50f;

// How far the rim reaches into the material, in world pixels.
//
// Short on purpose, and the reason is a division of labour. The light solve
// already carries daylight into solid rock over `light::Settings::surfaceReach`,
// eighty pixels, falling by a factor of e across it — that long gradient is the
// light's job and it is already done. This is the other thing, the one a pixel
// artist draws by hand: the bright lip along the top of a ledge and the dark
// underside of an overhang, two or three texels of it. Reach much further and it
// becomes a second copy of the light's own gradient, and the rock goes dark
// twice.
// Nine, and the number is not free: the terrain field is clamped into [0,1], so
// it saturates about thirteen pixels inside the rock — `kDensitySpan` times the
// half of the range above the surface level — and past that there is no gradient
// left to measure a depth against. A reach longer than that would spend most of
// itself on a stretch the field cannot describe, and the rim would still be at
// half strength where the readable part ends. This lands it at nothing with room
// to spare.
constexpr float kRimReach = 9.0f;

// How much of a tone step the rim is worth at the very face, at its strongest.
//
// Read against kElementRamp: at two and a half steps, a lit top edge and the
// shaded belly under an overhang are four or five tones apart, which is what
// separates a drawn surface from a filled region.
constexpr float kRimTones = 2.5f;

// Scale of the slow drift and of the bedding, in features per terrain::
// kFeatureSpan pixels.
//
// The drift spans a couple of hundred pixels — a patch of wall rather than a
// speckle. The bedding is finer across and stretched flat by kStrataAspect, so
// what it lays down is layers a few pixels thick running level, which is how
// stone is put down and how it has to read.
//
// The bedding's number is set from the wavelength it wants and not from taste:
// at sixty features per thousand pixels a layer is about sixteen pixels from one
// to the next, which is three terrain texels — thick enough to be a layer and
// thin enough that a wall shows several. At half of that the layers were further
// apart than a screenful of rock is tall, and what they drew was one slow
// gradient nobody would call bedding.
constexpr float kPatchFrequency  = 4.5f;
constexpr float kStrataFrequency = 60.0f;
constexpr float kStrataAspect    = 9.0f;

// Seeds. Each term gets its own so no two of them are one field read twice,
// which would tie the bedding to the drift and make every pale patch a pale
// layer.
constexpr int kPatchSeed  = 7311;
constexpr int kStrataSeed = 7317;

// And one for the blotches a vein is scattered on, kept clear of the three above
// for exactly that reason: a blotch that fell where the grain was already dark
// would put every speck of ore in the shadow of the rock behind it.
constexpr int kVeinSeed = 7331;

// Hash of a pair of integers, in [0,1).
//
// A hash and not a noise field, because this one is read at the texel and a
// smooth field sampled per texel is not a stipple, it is a blur. The same
// arithmetic canopy.cpp uses on its own texels, kept separate for the reason that
// one does: they are read at different scales and there is no reason a change to
// one should move the other.
float Bits(int x, int y, int seed) {
    auto bits = static_cast<std::uint32_t>(x * 374761393 + y * 668265263 + seed * 2147483647);

    bits = (bits ^ (bits >> 13)) * 1274126177u;
    bits ^= bits >> 16;

    return static_cast<float>(bits & 0xffffffu) / static_cast<float>(0x1000000u);
}

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

unsigned char Channel(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
}

} // namespace

Color Blend(Color from, Color to, float t) {
    const float u = std::clamp(t, 0.0f, 1.0f);

    return {Channel(from.r + (to.r - from.r) * u), Channel(from.g + (to.g - from.g) * u),
            Channel(from.b + (to.b - from.b) * u), Channel(from.a + (to.a - from.a) * u)};
}

Ramp Build(const Color authored[kElementTones]) {
    Ramp ramp{};

    // The four written tones land on the ramp evenly and the gaps between them
    // are filled by mixing their neighbours.
    //
    // Interpolated rather than authored outright for the reason the canopy's
    // leaves are: seven tones close enough together to read as one material are
    // very hard to pick by hand and very easy to derive, and what an author
    // actually has an opinion about is the darkest, the lightest and the two in
    // between.
    const float span = static_cast<float>(kElementTones - 1);

    for (int i = 0; i < kElementRamp; i++) {
        const float at = span * static_cast<float>(i) / static_cast<float>(kElementRamp - 1);

        const auto low = static_cast<std::size_t>(at);
        const auto high = std::min(low + 1, kElementTones - 1);

        ramp.tone[i] = Blend(authored[low], authored[high], at - static_cast<float>(low));
    }

    return ramp;
}

Paint For(const ElementDef &def, int seed) {
    // The fringe, from a share of the vein to the pixels the painter measures a
    // texel's depth in. The half-width is what the share is of, because the depth
    // runs from nothing at the face to the half-width at the middle.
    //
    // Floored at half a texel rather than allowed to reach zero. A material with no
    // vein generator at all — an inclusion only ever placed by hand — would otherwise
    // get a fringe of nothing and a hard edge, and a hard edge is the one thing this
    // whole arrangement exists to remove.
    const float half = def.spawn.veinCells * static_cast<float>(config::kResolution) * 0.5f;

    return {.ramp   = Build(def.paint.tone),
            .grain  = def.paint.grain,
            .patch  = def.paint.patch,
            .strata = def.paint.strata,
            .vein   = {.share  = def.paint.vein.share,
                       .blotch = def.paint.vein.blotch,
                       .rim    = std::max(def.paint.vein.fringe * half, config::kPixelSize * 0.5f)},
            .seed   = seed};
}

Shade Shading(const Paint &paint, const marching_squares::Texel &texel) {
    Shade shade;
    shade.base = kBase;

    // The form, and it is the only term allowed to move a texel more than a step.
    //
    // How much of a face there is, times which way it points. Deep inside the
    // material there is no face and the product is nothing, which is right: what
    // makes the inside of a rock read as the inside of a rock is that it has no
    // lighting of its own at all.
    const float rim  = 1.0f - SmoothStep(0.0f, kRimReach, texel.depth);
    const float face = texel.normal.x * kKeyX + texel.normal.y * kKeyY;

    shade.form = rim * (paint.bedded ? std::max(face, 0.0f) : face) * kRimTones * kStep;

    // Then the texture, at three scales. None of them is allowed to decide
    // anything: the drift moves a patch of wall a tone, the bedding lays that
    // down in layers, and the grain stipples the boundary between two tones the
    // way a hand would.
    if (paint.patch > 0.0f) {
        const float drift =
            terrain::Sample(texel.at, {.frequency = kPatchFrequency, .octaves = 2, .seed = kPatchSeed + paint.seed});

        shade.patch = (drift - 0.5f) * 2.0f * paint.patch * kStep;
    }

    if (paint.strata > 0.0f) {
        const float bedding = terrain::Sample(texel.at, {.frequency = kStrataFrequency,
                                                         .octaves  = 2,
                                                         .aspect   = kStrataAspect,
                                                         .seed     = kStrataSeed + paint.seed});

        shade.strata = (bedding - 0.5f) * 2.0f * paint.strata * kStep;
    }

    if (paint.grain > 0.0f) {
        const int gx = static_cast<int>(std::floor(texel.at.x / config::kPixelSize));
        const int gy = static_cast<int>(std::floor(texel.at.y / config::kPixelSize));

        shade.grain = (Bits(gx, gy, paint.seed) - 0.5f) * 2.0f * paint.grain * kStep;
    }

    return shade;
}

bool Shows(const Paint &paint, const marching_squares::Texel &texel) {
    if (paint.vein.Solid()) return true;

    // Dense at the core and thinning to nothing at the face, which is the term
    // that dissolves the outline: blotches that stopped along the very curve the
    // solid disc was drawn to would let the eye put the disc back together.
    //
    // `rim` is already in the pixels the depth is measured in — For converted it
    // from the share of the vein the row is written with, so this arithmetic knows
    // nothing about how wide a seam of this material is.
    const float dense = SmoothStep(0.0f, paint.vein.rim, texel.depth) * paint.vein.share;

    // Then the blotch the texel falls in, hashed against that share.
    //
    // A hash and not a noise field, and the reason is that the share has to *be*
    // the share: a hash is uniform, so a cut at 0.45 keeps 45% of the blotches.
    // Perlin is crowded hard around its own midpoint — §17.5's trap, met from the
    // other side — so the same cut on a field would keep some quite different
    // fraction, and the one number the row is written in would mean nothing.
    //
    // Quantised to the blotch and anchored to the world, so the pattern belongs to
    // the rock and does not crawl as the view scrolls.
    const int bx = static_cast<int>(std::floor(texel.at.x / paint.vein.blotch));
    const int by = static_cast<int>(std::floor(texel.at.y / paint.vein.blotch));

    return Bits(bx, by, paint.seed + kVeinSeed) < dense;
}

Color Paint::operator()(const marching_squares::Texel &texel) const {
    // Nothing at all where the vein is not, rather than a colour: a texel this
    // painter declines is one DrawPainted does not submit, so what stays on the
    // screen is what the pass underneath put there. That is also why this is the
    // one place in the ground allowed to answer with an alpha of zero — see §5.5,
    // which needs every colour the ground *is* drawn in to be opaque, and this is
    // not a colour.
    if (!Shows(*this, texel)) return BLANK;

    const float lit = Shading(*this, texel).Lit();

    const int index = std::clamp(static_cast<int>(lit * static_cast<float>(kElementRamp)), 0, kElementRamp - 1);

    return ramp.tone[index];
}

} // namespace soil
