#include "flora/canopy.h"

#include "core/config.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>

namespace canopy {
namespace {

// Field value foliage counts as present at. The masses are summed as a soft
// falloff and cut here, which is the same arrangement the terrain uses and for
// the same reason: a threshold over a continuous field gives an outline that
// steps along the pixels rather than one drawn on them.
constexpr float kLeafThreshold = 0.5f;

// The direction the form shading is worked out against, in texels, Y down. Up
// and a little to the left, which is where every drawn tree of this kind is lit
// from — and fixed, for the reason in the header.
//
// Only a little to the left. The reference art reads as top-lit and very nearly
// symmetric, and every degree off vertical is a degree by which a mirrored copy
// of a tree would be lit from the wrong side. Mirroring has been dropped for
// exactly that reason, but a near-overhead key is what the references show
// anyway.
constexpr float kKeyX = -0.20f;
constexpr float kKeyY = -0.98f;

// Lattice periods, in texels, of the noises that break a canopy up.
//
// The first has to land near two texels or the notches it cuts are larger than
// the masses they are cut out of; the second is the scale of the holes a crown
// is seen through, so it is near a third of a mass.
constexpr float kRaggedPeriod = 2.6f;
constexpr float kGapPeriod    = 7.5f;
constexpr float kBarkPeriod   = 2.2f;

// How far apart the bunches of leaves sit, in texels.
//
// Small, because this is the size a drawn leaf cluster is: three texels across
// is two or three pixels of green with a dark pixel beside it, which is exactly
// what the reference art resolves to. Much larger and the crown goes back to
// reading as paint.
constexpr float kLeafCell = 3.1f;

// How close to halfway between two bunches a texel has to be before it is drawn
// as the seam between them rather than as part of either.
//
// The single most important number here. At zero there are no seams and the
// crown is a smooth field again; too high and the bunches are lost in a net of
// dark lines.
constexpr float kLeafEdge = 0.040f;

// Steps in the ramp a canopy is painted from.
//
// More than the table authors, and interpolated between the four it does. The
// table's four are anchors — the darkest green, two between, and the highlight —
// and four steps is not enough to paint foliage with: what the reference art has
// is six or eight greens that are barely apart from one another, sprinkled
// through the whole crown, and that is what reads as leaves. Four made a dark
// mass with a lit rim.
//
// Interpolating rather than authoring seven per species per season keeps the
// table something a person can read and change.
constexpr int kLeafTones = 7;

// Where the middle of the ramp sits before anything moves it, and how far each
// term may move it. One step of the ramp is 1/kLeafTones = 0.143.
//
// The base sits at the centre of a bucket rather than near its edge. At 0.56 it
// was eleven thousandths below the boundary between tones three and four, so any
// term with a mean of zero split the base half and half between two greens and
// added a step of noise to everything.
//
// The weights are the answer to the fault this whole arrangement was rebuilt for.
// Measured over a real oak crown, the old set put 65% of the tone variance at the
// three-texel bunch scale and only 9% anywhere organised — which is precisely
// what "the light and shadow look disorganised" describes. The rule now is that
// the two terms which vary smoothly across the crown carry the form, and the two
// that vary per bunch may not move a texel a whole step on their own.
constexpr float kLeafBase = 0.50f;

// How much shelter from the key drops a texel. The largest of them: this is the
// form, and it has to carry light, mid and dark on its own.
constexpr float kLeafSun = 0.46f;

// The dome across one mass of foliage, from its lit crest to its shaded belly.
constexpr float kLeafForm = 0.20f;

// Sitting low in the crown, as a residual. Small, because the shelter term
// already falls with height — the two would otherwise count the same thing twice.
constexpr float kLeafDepth = 0.10f;

// One mass's own luck, so two side by side are not the same green. Held under a
// quarter of a step: at half a step it is banding, at more it is the noise this
// replaced.
constexpr float kLeafTint = 0.06f;

// The turn within one bunch of leaves. Half a step — enough to stipple the
// boundary between two tones, which is the thing a pixel artist does by hand, and
// not enough to decide which tone a region is.
constexpr float kLeafDither = 0.13f;

// How far the two darks reach, in ramp steps, as a subtraction rather than a
// colour. Written in steps because that is how they were chosen.
constexpr float kSeamDrop      = 1.5f;
constexpr float kUndersideDrop = 2.4f;

// How deep the snow lies on a crown, in texels.
//
// Three, and it is bounded from both sides by the size of the thing it is lying
// on. A mass of foliage is about nine texels tall — enough to carry a crest, a
// middle and a shaded belly — so a cap of four or more would be most of a mass and
// the tree would come out as a white blob with green under it. One texel is a line
// drawn along the top, which is the same failure the grass band had at one texel
// and for the same reason: a single row cannot carry a lit face and a shaded edge,
// so it reads as an outline rather than as a layer.
constexpr int kSnowDeep = 3;

// The sheet, in texels. One slot holds the largest plant the table can produce
// at the largest size it rolls.
constexpr int kSlotW = 96;
constexpr int kSlotH = 136;

constexpr int kColumns = 10;
constexpr int kRows    = 10;

// Plants drawn per frame.
//
// Raised once the undergrowth arrived: a screen holds forty-odd plants now
// rather than a dozen, and at six a frame the first second of a new view was
// spent with most of the wood missing while its shade was already being cast.
// Walking into a wood brings a few plants into view a second, so this only binds
// when the view jumps, and then it spreads the cost over the next two frames
// rather than one.
constexpr int kDrawBudget = 24;

// Texels of clear space left around a plant inside its slot. Declared in the
// header, since --sprites has to size a plant exactly the way Render does.
constexpr int kPad = kSpritePad;

// Smallest half-height a mass of foliage may have and still be drawn, in texels.
constexpr float kLeastMass = 1.8f;

std::uint32_t Bits(int x, int y, int seed) {
    auto bits = static_cast<std::uint32_t>(x) * 2654435761u;
    bits ^= static_cast<std::uint32_t>(y) * 2246822519u;
    bits ^= static_cast<std::uint32_t>(seed) * 3266489917u;
    bits ^= bits >> 15;
    bits *= 2246822519u;
    bits ^= bits >> 13;

    return bits;
}

float Corner(int x, int y, int seed) {
    return static_cast<float>(Bits(x, y, seed) & 0xffffffu) / static_cast<float>(0x1000000u);
}

// Value noise in [0,1] on a unit lattice, smoothly interpolated.
//
// Smooth rather than nearest, because what it is used for is moving a threshold:
// a blocky field would move the outline in whole blocks and put square bites out
// of a canopy instead of the notches it is meant to cut.
float Value(float x, float y, int seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);

    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);

    const float sx = (x - fx) * (x - fx) * (3.0f - 2.0f * (x - fx));
    const float sy = (y - fy) * (y - fy) * (3.0f - 2.0f * (y - fy));

    const float a = Corner(ix, iy, seed);
    const float b = Corner(ix + 1, iy, seed);
    const float c = Corner(ix, iy + 1, seed);
    const float d = Corner(ix + 1, iy + 1, seed);

    const float top    = a + (b - a) * sx;
    const float bottom = c + (d - c) * sx;

    return top + (bottom - top) * sy;
}

// One bunch of leaves, as the texel standing in it sees it.
struct Leaf {
    Vector2 at{};      // Centre of the bunch, in texels.
    float tint = 0.0f; // The bunch's own roll, so two neighbours differ.
    float edge = 1.0f; // How far from the seam with the next bunch, in cells.
    float away = 0.0f; // How far round the bunch this texel is from the key side.
};

// The bunch a texel belongs to.
//
// Cellular rather than smooth: what is wanted is a hard division of the crown
// into small pieces, each shaded as a unit, which is how the reference art is
// drawn and is not something a value noise can be coaxed into. The two nearest
// centres are both kept because the seam between two bunches is the difference
// between them — a texel equally far from both is on the line, whatever the
// absolute distance happens to be there.
Leaf LeafAt(float x, float y, int seed) {
    const float gx = x / kLeafCell;
    const float gy = y / kLeafCell;

    const int ix = static_cast<int>(std::floor(gx));
    const int iy = static_cast<int>(std::floor(gy));

    Leaf leaf;

    float nearest = 1e9f;
    float second  = 1e9f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int cx = ix + dx;
            const int cy = iy + dy;

            const float jx = static_cast<float>(cx) + Corner(cx, cy, seed);
            const float jy = static_cast<float>(cy) + Corner(cx, cy, seed + 977);

            const float ox = jx - gx;
            const float oy = jy - gy;

            const float d = std::sqrt(ox * ox + oy * oy);

            if (d < nearest) {
                second  = nearest;
                nearest = d;

                leaf.at   = {jx * kLeafCell, jy * kLeafCell};
                leaf.tint = Corner(cx, cy, seed + 613);

                // Which way round the bunch this texel lies, against the key. One
                // where it faces away from the light, zero where it faces into
                // it. The offset is normalised, so it says direction and not
                // distance, and a bunch is therefore lit the same however big it
                // came out.
                const float length = std::max(d, 1e-4f);
                leaf.away          = 0.5f + 0.5f * ((-ox / length) * kKeyX + (-oy / length) * kKeyY);
            } else if (d < second) {
                second = d;
            }
        }
    }

    leaf.edge = second - nearest;

    return leaf;
}

Color Shade(Color colour, float factor) {
    const auto channel = [factor](unsigned char v) {
        return static_cast<unsigned char>(std::clamp(v * factor, 0.0f, 255.0f));
    };

    return {channel(colour.r), channel(colour.g), channel(colour.b), colour.a};
}

Color Blend(Color a, Color b, float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);

    const auto channel = [s](unsigned char from, unsigned char to) {
        return static_cast<unsigned char>(from + (to - from) * s + 0.5f);
    };

    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255};
}

// The ramp a crown is painted from, drawn out of the four the table authors.
//
// Every step is a blend of two neighbouring anchors, so the whole ramp stays
// inside the hue the species was given and no step is far from the one beside
// it. That is the property being bought: several tones of nearly the same green
// rather than four that are visibly different colours.
void BuildRamp(const flora::SpeciesPalette &palette, Color ramp[kLeafTones]) {
    for (int i = 0; i < kLeafTones; i++) {
        const float along = static_cast<float>(i) / static_cast<float>(kLeafTones - 1) * 3.0f;

        const int step = std::clamp(static_cast<int>(along), 0, 2);

        ramp[i] = Blend(palette.leaf[step], palette.leaf[step + 1], along - static_cast<float>(step));
    }
}

// Distance from a point to a segment, and how far along it that fell.
float ToSegment(float px, float py, Vector2 a, Vector2 b, float &along) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;

    const float length = dx * dx + dy * dy;

    along = (length > 1e-6f) ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / length, 0.0f, 1.0f) : 0.0f;

    const float cx = a.x + dx * along - px;
    const float cy = a.y + dy * along - py;

    return std::sqrt(cx * cx + cy * cy);
}

// One plant's worth of buffers, in texels. Reused between plants.
struct Canvas {
    int w = 0;
    int h = 0;

    std::vector<float> leaf;  // Foliage field, cut at kLeafThreshold.
    std::vector<float> depth; // How far down the crown the mass here sits.
    std::vector<float> form;  // The dome across the mass that owns this texel.
    std::vector<float> sun;   // How much of the key reaches here, in [0,1].
    std::vector<float> wood;  // Trunk and branch coverage.
    std::vector<float> grain; // Signed offset across the trunk, for its two sides.

    void Fit(int width, int height) {
        w = std::clamp(width, 1, kSlotW);
        h = std::clamp(height, 1, kSlotH);

        const auto count = static_cast<std::size_t>(w) * h;

        leaf.assign(count, 0.0f);
        depth.assign(count, 0.0f);
        form.assign(count, 0.0f);
        sun.assign(count, 0.0f);
        wood.assign(count, 0.0f);
        grain.assign(count, 0.0f);
    }

    std::size_t Index(int x, int y) const { return static_cast<std::size_t>(y) * w + x; }
    bool Holds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    float LeafAt(float x, float y) const {
        const int ix = static_cast<int>(std::lround(x));
        const int iy = static_cast<int>(std::lround(y));

        return Holds(ix, iy) ? leaf[Index(ix, iy)] : 0.0f;
    }
};

// Where a plant's own frame lands in its canvas.
struct Frame {
    float left  = 0.0f; // Local X at texel 0.
    float top   = 0.0f; // Local Y at texel 0, measured upward from the trunk foot.
    float pixel = 1.0f;

    float ToX(float localX) const { return (localX - left) / pixel; }
    float ToY(float localY) const { return (top - localY) / pixel; }
};

// The masses of foliage, summed and then torn.
void LayFoliage(const flora::Skeleton &skeleton, const flora::SpeciesShape &art, const Frame &frame, int seed,
                Canvas &canvas) {
    for (int i = 0; i < skeleton.lobeCount; i++) {
        const flora::Lobe &lobe = skeleton.lobes[i];

        const float cx = frame.ToX(lobe.at.x);
        const float cy = frame.ToY(lobe.at.y);

        const float radius = lobe.radius / frame.pixel;
        const float squash = std::max(lobe.flatten, 0.05f);

        // Masses smaller than this have no inside: every texel of one has empty
        // space under it, so the underside accent claims all of them and what
        // reaches the screen is a detached speck of near-black above the crown.
        // The tapering top tier of a conifer produces one of these every time.
        if (radius * squash < kLeastMass) continue;

        // Walked over the mass's own box rather than over the canvas. Per mass
        // over the whole canvas would be twenty times the work for the same
        // answer.
        const int x0 = std::max(static_cast<int>(std::floor(cx - radius)) - 1, 0);
        const int x1 = std::min(static_cast<int>(std::ceil(cx + radius)) + 1, canvas.w - 1);
        const int y0 = std::max(static_cast<int>(std::floor(cy - radius * squash)) - 1, 0);
        const int y1 = std::min(static_cast<int>(std::ceil(cy + radius * squash)) + 1, canvas.h - 1);

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const float dx = (static_cast<float>(x) + 0.5f - cx) / std::max(radius, 0.5f);
                const float dy = (static_cast<float>(y) + 0.5f - cy) / std::max(radius * squash, 0.5f);

                const float d = dx * dx + dy * dy;
                if (d >= 1.0f) continue;

                // A soft cap rather than a disc, so masses that overlap swell
                // into one another instead of showing the seam between two
                // circles.
                const float value = 1.0f - d;

                const std::size_t at = canvas.Index(x, y);

                // The mass that covers a texel most owns its shading. Otherwise a
                // mass at the top of the crown lends its brightness to
                // everything hanging under it.
                if (value > canvas.leaf[at]) {
                    canvas.depth[at] = lobe.depth;

                    // The dome across this mass: where the texel sits on it,
                    // against the key. Positive on the crest, negative on the
                    // belly — which is the light-over-dark the reference art
                    // gives every mass of foliage it draws.
                    //
                    // Taken from the offsets already computed, so it costs
                    // nothing, and in the mass's own squashed space so that the
                    // lit band follows a flattened frond out to its tips instead
                    // of sitting on it as a circular cap.
                    //
                    // Divided by the visible radius rather than the nominal one.
                    // The field is 1 − d² cut at a half, so foliage only reaches
                    // d = 1/√2 and anything scaled "per radius" is out by a
                    // factor of 1.41.
                    constexpr float kVisible = 0.7071f;

                    // A texel above the centre has dy negative and the key points
                    // up, so the two multiply to a positive: the crest is lit
                    // without the expression needing a sign put on it.
                    canvas.form[at] = (dx * kKeyX + dy * kKeyY) / kVisible +
                                      (Corner(i, 0, 7717) - 0.5f) * 2.0f * (kLeafTint / std::max(kLeafForm, 1e-3f));
                }

                canvas.leaf[at] = std::max(canvas.leaf[at], value);
            }
        }
    }

    // Then the two things that stop it reading as a blob: an edge broken into
    // notches, and holes torn through the middle.
    //
    // Done over the finished sum rather than per mass. Per mass the tears would
    // stop at every seam between two of them, and the seams are exactly what the
    // sum was for.
    for (int y = 0; y < canvas.h; y++) {
        for (int x = 0; x < canvas.w; x++) {
            const std::size_t at = canvas.Index(x, y);
            if (canvas.leaf[at] <= 0.0f) continue;

            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);

            canvas.leaf[at] += (Value(fx / kRaggedPeriod, fy / kRaggedPeriod, seed + 11) - 0.5f) * 2.0f * art.ragged;

            // Only the top of the noise tears, and not far. A crown is a dense
            // mass with a few holes punched through it; letting this reach into
            // the middle of the range dissolved it into a spray of separate
            // leaves with a bare pole up the middle.
            const float hole = Value(fx / kGapPeriod, fy / kGapPeriod, seed + 23);
            canvas.leaf[at] -= std::max(hole - 0.60f, 0.0f) * 2.0f * art.gaps;
        }
    }
}

// The trunk and the limbs.
void LayWood(const flora::Skeleton &skeleton, const Frame &frame, bool bare, int seed, Canvas &canvas) {
    const auto sweep = [&canvas](Vector2 a, Vector2 b, float halfA, float halfB) {
        const float widest = std::max(halfA, halfB) + 1.0f;

        const int x0 = std::max(static_cast<int>(std::floor(std::min(a.x, b.x) - widest)), 0);
        const int x1 = std::min(static_cast<int>(std::ceil(std::max(a.x, b.x) + widest)), canvas.w - 1);
        const int y0 = std::max(static_cast<int>(std::floor(std::min(a.y, b.y) - widest)), 0);
        const int y1 = std::min(static_cast<int>(std::ceil(std::max(a.y, b.y) + widest)), canvas.h - 1);

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;

                float along      = 0.0f;
                const float d    = ToSegment(px, py, a, b, along);
                const float half = halfA + (halfB - halfA) * along;

                if (d > half) continue;

                const std::size_t at = canvas.Index(x, y);

                canvas.wood[at] = 1.0f;

                // Which side of the axis this fell, across the width. The
                // trunk's tones are read off it, so a trunk is lit down one edge
                // rather than being one flat brown column.
                const float side = (b.y - a.y) * (px - a.x) - (b.x - a.x) * (py - a.y);

                canvas.grain[at] = std::clamp((side >= 0.0f ? d : -d) / std::max(half, 0.5f), -1.0f, 1.0f);
            }
        }
    };

    for (int n = 1; n < flora::kTrunkNodes; n++) {
        const Vector2 a = {frame.ToX(skeleton.trunk[n - 1].x), frame.ToY(skeleton.trunk[n - 1].y)};
        const Vector2 b = {frame.ToX(skeleton.trunk[n].x), frame.ToY(skeleton.trunk[n].y)};

        float halfA = skeleton.trunkWidth[n - 1] * 0.5f / frame.pixel;
        const float halfB = skeleton.trunkWidth[n] * 0.5f / frame.pixel;

        // The foot flares where it meets the ground, which is the one place a
        // drawn tree is never a straight column.
        if (n == 1) halfA *= 1.5f;

        sweep(a, b, std::max(halfA, 0.6f), std::max(halfB, 0.5f));
    }

    // The thinnest a limb may be drawn, in texels of half-width.
    //
    // Not a half, which is what it was. A texel is claimed when the segment passes
    // within `half` of its centre, and the centres of two diagonally neighbouring
    // texels are 1.41 apart — so at a half-width of 0.5 a near-horizontal limb
    // running between two rows of centres claims neither, and comes out as a line of
    // floating dashes with sky between them. Just over the half-diagonal is the
    // figure at which a limb is guaranteed to be one connected thing at any angle.
    constexpr float kThinnestLimb = 0.72f;

    for (int i = 0; i < skeleton.branchCount; i++) {
        const flora::Branch &branch = skeleton.branches[i];

        const Vector2 a = {frame.ToX(branch.from.x), frame.ToY(branch.from.y)};

        const float half = std::max(branch.width * 0.5f / frame.pixel, kThinnestLimb);

        if (!bare) {
            const Vector2 b = {frame.ToX(branch.to.x), frame.ToY(branch.to.y)};

            sweep(a, b, half, std::max(half * 0.55f, kThinnestLimb));
            continue;
        }

        // Bare, the limb is the tree, and a straight spar out to where the leaves
        // would have been is a telephone pole rather than a branch.
        //
        // Three things separate the two, and all of them are here. A limb *rises* as
        // it goes out, so it is walked as an arc rather than a chord — a bare tree
        // is a bundle of upswept lines and a horizontal one reads as a crosstree.
        // It *tapers* the whole way, to nothing rather than to half. And it *forks*:
        // most of what the eye reads as a winter tree is the fine spray at the ends,
        // and a limb that simply stops has no silhouette at all.
        const Vector2 end = {frame.ToX(branch.tip.x), frame.ToY(branch.tip.y)};

        const float run  = end.x - a.x;
        const float rise = end.y - a.y;

        // Each limb its own arc, fork angle and fork length, drawn off the tree's
        // seed and the limb's index. Without this every branch is the same curve
        // turned about the trunk, and a wood of them reads as a row of candelabra —
        // the regularity of the tiers underneath is invisible under summer foliage
        // and is the whole silhouette once the leaves are off.
        const auto roll = [&](int salt) { return static_cast<float>(Bits(i, salt, seed) & 0xffffu) / 65536.0f; };

        // How far the arc stands off its own chord, against the reach of the limb.
        // Y grows downward in the canvas, so lifting is subtracting.
        const float lift = std::fabs(run) * (0.18f + 0.24f * roll(11));

        constexpr int kJoints = 4;

        Vector2 walk  = a;
        float carried = half;

        for (int j = 1; j <= kJoints; j++) {
            const float t = static_cast<float>(j) / static_cast<float>(kJoints);

            // A parabola through both ends, at its furthest from the chord halfway
            // along, which is the shape a loaded limb actually takes.
            const Vector2 node = {a.x + run * t, a.y + rise * t - lift * (t - t * t) * 4.0f};

            const float shrink = std::max(half * (1.0f - 0.80f * t), kThinnestLimb);

            sweep(walk, node, carried, shrink);

            walk    = node;
            carried = shrink;
        }

        // And the fork at the end. Two twigs, thrown out at a shallow angle from the
        // limb's own heading and lifted, so the tip breaks into a Y rather than
        // stopping dead. Short — a fifth of the limb — because what is wanted is a
        // broken silhouette, not a second generation of branches.
        const float away = std::sqrt(run * run + rise * rise);

        if (away > 3.0f) {
            const float ux = run / away;
            const float uy = rise / away;

            for (int side = -1; side <= 1; side += 2) {
                // Not every tip forks both ways. A wood in which every limb ends in
                // a perfect Y is as regular as one in which none of them fork.
                if (roll(side * 7 + 23) < 0.18f) continue;

                const float reach = away * (0.16f + 0.16f * roll(side * 7 + 31));
                const float splay = 0.40f + 0.34f * roll(side * 7 + 41);

                // Turned off the heading and lifted, both. The lift is what keeps a
                // fork reading as growth rather than as a dead spike.
                const float spread = std::sqrt(std::max(1.0f - splay * splay, 0.0f));

                const float tx = ux * spread - uy * splay * static_cast<float>(side);
                const float ty = uy * spread + ux * splay * static_cast<float>(side) - 0.30f;

                sweep(walk, {walk.x + tx * reach, walk.y + ty * reach}, carried, kThinnestLimb);
            }
        }
    }
}

// How much of the key reaches every texel, as one sweep down the canvas.
//
// This replaced three samples taken along the key and tested against the
// threshold, and the replacement is the single change that made a canopy read as
// lit. What was wrong with the samples was not that there were too few: measured
// over a real oak crown, **seventy per cent of foliage texels saturated** with
// all three buried, so for seven texels in ten the term was a constant carrying
// no information at all. Its reach was eight texels into a crown forty-five
// texels tall — a rim light, not a form.
//
// A leaky accumulator run down the key has no such ceiling. Each row takes what
// the row above it had, shifted along the key, plus whatever foliage stands
// there, and lets a fixed share of it go. So shelter accumulates without bound
// through a deep crown and decays through a shallow one, which is the difference
// between "is anything above me" and "how much".
//
// Measured against what it replaced, on the same crown: nothing saturates, 93% of
// its variance survives a seven-texel blur, and it correlates −0.77 with height in
// the crown. It does four jobs at once — the dome over every bump of the
// silhouette, the shadow an upper mass throws on the one beneath it, the crown's
// own top-to-bottom gradient, and the dapple where the tears let light through.
void LaySun(Canvas &canvas) {
    // Fraction kept from one row to the next. Set from the size of a mass, so
    // shelter fades over about the depth of one and a lower mass is shaded by the
    // one directly above it but not by the whole crown.
    constexpr float kDecay = 0.90f;

    // Steps sideways per row, so the sweep runs along the key rather than
    // straight down.
    const float slide = kKeyX / std::max(-kKeyY, 1e-3f);

    for (int y = 0; y < canvas.h; y++) {
        for (int x = 0; x < canvas.w; x++) {
            const std::size_t at = canvas.Index(x, y);

            float above = 0.0f;

            if (y > 0) {
                // Where this texel's column was on the row above, interpolated,
                // so the sweep does not stagger sideways in whole texels.
                const float sx = static_cast<float>(x) - slide;

                const int ix   = static_cast<int>(std::floor(sx));
                const float ft = sx - static_cast<float>(ix);

                const auto sample = [&canvas, y](int column) {
                    return canvas.Holds(column, y - 1) ? canvas.sun[canvas.Index(column, y - 1)] : 0.0f;
                };

                above = sample(ix) * (1.0f - ft) + sample(ix + 1) * ft;
            }

            // Softened rather than tested, so a texel just inside the foliage
            // does not shade the one below it as hard as one deep in a mass.
            const float cover = std::clamp((canvas.leaf[at] - (kLeafThreshold - 0.12f)) / 0.24f, 0.0f, 1.0f);

            canvas.sun[at] = (above + cover) * kDecay;
        }
    }

    // Turned from shelter into light, and normalised so the constant that scales
    // it means the same thing whatever the decay is set to.
    const float reach = 1.0f / (1.0f - kDecay);

    for (float &value : canvas.sun) value = std::clamp(1.0f - value / reach, 0.0f, 1.0f);
}

// Whether a texel is part of the plant at all — foliage, trunk or limb.
//
// One test rather than two, because what snow lands on is the top of the *tree*
// and a bare winter branch catches it exactly as a crown does.
bool Solid(const Canvas &canvas, int x, int y) {
    if (!canvas.Holds(x, y)) return false;

    const std::size_t at = canvas.Index(x, y);

    return canvas.leaf[at] > kLeafThreshold || canvas.wood[at] > 0.0f;
}

// How much snow is lying on a texel, in [0,1].
//
// A count of how far below the exposed top of its own mass the texel sits, which
// is the whole of it: snow lies on what faces the sky and on nothing else, and
// "faces the sky" at this size means "there is nothing directly above it". So the
// first texel under open air is full, the one below it less, and by kSnowDeep
// there is none.
//
// Straight up rather than along the key the shading uses. Snow falls; it does not
// arrive at an angle, and lighting it from the same direction as the leaves is
// what the tone below is for.
//
// The depth wanders a texel either way from the plant's own noise, so a crown does
// not come out with a level white line ruled across it — which is precisely what a
// fixed depth draws, and it reads as a stripe rather than as weather.
float Snowed(const Canvas &canvas, int x, int y, int seed) {
    if (!Solid(canvas, x, y)) return 0.0f;

    const auto fx = static_cast<float>(x);
    const auto fy = static_cast<float>(y);

    const float deep =
        std::max(1.0f, static_cast<float>(kSnowDeep) + (Value(fx / 3.4f, fy / 3.4f, seed + 131) - 0.5f) * 2.4f);

    for (int up = 1; up <= static_cast<int>(deep) + 1; up++) {
        if (!Solid(canvas, x, y - up)) {
            return std::clamp(1.0f - static_cast<float>(up - 1) / deep, 0.0f, 1.0f);
        }
    }

    return 0.0f;
}

void Paint(const Canvas &canvas, const flora::SpeciesPalette &palette, bool snowy, int seed, std::vector<Color> &out) {
    out.assign(static_cast<std::size_t>(canvas.w) * canvas.h, Color{0, 0, 0, 0});

    Color ramp[kLeafTones];
    BuildRamp(palette, ramp);

    // The snow, taken from the material's own row rather than from four whites
    // written here.
    //
    // A crown under snow and the ground under the same tree have to be the same
    // white, and the ground's is in the element table. Written down twice, they
    // would agree on the day it was typed and stop agreeing the first time
    // somebody retunes one of them — and a tree wearing a slightly different snow
    // from the field it stands in is exactly the kind of wrongness nobody can name
    // while looking straight at it.
    const Color *lying = Def(Element::Snow).paint.tone;

    for (int y = 0; y < canvas.h; y++) {
        for (int x = 0; x < canvas.w; x++) {
            const std::size_t at = canvas.Index(x, y);

            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);

            Color colour{0, 0, 0, 0};

            if (canvas.leaf[at] > kLeafThreshold) {
                // Which bunch of leaves this texel belongs to.
                //
                // This is the whole of what makes a canopy read as foliage, and
                // it took getting wrong twice to find. Tone taken from smooth
                // noise and quantised gives soft patches — the crown comes out
                // washed, and you cannot feel that it is made of leaves. Drawn
                // leaves are not a gradient: they are many small bunches, each
                // with a hard edge and a dark line separating it from the next.
                // So the texels are handed out to bunches first and shaded per
                // bunch, rather than shaded per texel and hoped over.
                const Leaf leaf = LeafAt(fx, fy, seed);
                const float sun = canvas.sun[at];

                // The form first, and all of it smooth across the crown: how much
                // of the key reaches here, the dome of the mass this texel belongs
                // to, and a residual for sitting low down.
                float lit = kLeafBase;

                lit += (sun - 0.5f) * kLeafSun;
                lit += canvas.form[at] * kLeafForm;
                lit += (0.5f - canvas.depth[at]) * kLeafDepth;

                // Then the two darks, as steps taken off the tone rather than a
                // colour written over it.
                //
                // This was an if/else that set the darkest green in the ramp and
                // threw every term above away. Measured, the seam test alone
                // claimed **one texel in ten of the whole crown** and painted it
                // black, scattered evenly, uncorrelated with the light — a four
                // step drop in the middle of a highlight. It was the salt and
                // pepper, and no amount of tuning the tone equation could have
                // reached it. A crease inside a lit mass is a mid green; only a
                // crease in shadow is the darkest one.
                const float step = 1.0f / static_cast<float>(kLeafTones);

                if (canvas.LeafAt(fx, fy + 1.6f) <= kLeafThreshold) lit -= kUndersideDrop * step;

                // Deepened in shadow and all but gone in the highlight, which is
                // where a drawn crown puts its creases too.
                if (leaf.edge < kLeafEdge) lit -= kSeamDrop * step * (0.35f + 0.65f * (1.0f - sun));

                // And last the stipple, scaled by the light: the reference art is
                // loud with leaf detail on the lit crown and nearly flat in the
                // shaded interior.
                lit -= (leaf.away - 0.5f) * kLeafDither * (0.30f + 0.70f * sun);

                colour = ramp[std::clamp(static_cast<int>(lit * kLeafTones), 0, kLeafTones - 1)];
            } else if (canvas.wood[at] > 0.0f) {
                const float across = canvas.grain[at];

                colour = (across < -0.15f) ? palette.barkLight : (across < 0.45f ? palette.bark : palette.barkDark);

                // Bark: a scatter of darker texels and the occasional notch
                // right across. Both are what stops a trunk reading as three
                // flat stripes of brown.
                if (Value(fx / kBarkPeriod, fy / (kBarkPeriod * 2.4f), seed + 53) > 0.66f) colour = Shade(colour, 0.78f);
                if (Value(fx / 9.0f, fy / 1.15f, seed + 71) > 0.80f) colour = Shade(colour, 0.62f);
            }

            // And then the snow over all of it, where the tree is standing in a
            // snowfield.
            //
            // Laid last, over both the leaves and the wood, because that is what it
            // does: it is not a tone of the foliage, it is a material sitting on
            // top of one. Blended rather than replaced, so the tone underneath
            // still shows through the thinner edge of the cap — a hard white lid
            // reads as paint, and a crown is not a smooth surface for snow to sit
            // flat on.
            if (snowy && colour.a != 0) {
                const float lies = Snowed(canvas, x, y, seed);

                if (lies > 0.0f) {
                    // The lit crest and the body under it, which is as much relief
                    // as three texels of snow can carry. The deepest of the four
                    // tones is left for where the cap runs out, so its edge is a
                    // shadowed rim rather than a cut.
                    const int tone = (lies > 0.72f) ? 3 : (lies > 0.34f ? 2 : 1);

                    colour = Blend(colour, lying[tone], std::min(lies * 1.25f, 1.0f));
                }
            }

            out[at] = colour;
        }
    }
}

} // namespace

std::uint64_t Sheet::Key(std::int64_t cell, flora::Stage stage, flora::Season season, bool snowy) {
    // A plant's id shifted up by five, which is exact for any world anyone can
    // walk across: it leaves fifty-nine bits of id, and an id is a cell index
    // doubled — see flora::PlantId, which is also why a fern and a tree standing
    // in cells that happen to share a number are no longer one drawing.
    //
    // Snow is a bit of the key and not a tint applied at the draw, because it is
    // baked into the sprite: a cap of snow is texels, laid on whichever of them
    // face the sky, and there is no colour multiply over a finished tree that could
    // produce one. It is also the reason it costs nothing per frame — a tree in a
    // snowfield is baked once with its snow on and drawn from the sheet like any
    // other.
    return (static_cast<std::uint64_t>(cell) << 5) | (static_cast<std::uint64_t>(snowy) << 4)
           | (flora::StageIndex(stage) << 2) | flora::SeasonIndex(season);
}

int Sheet::Capacity() const { return kColumns * kRows; }

bool Sheet::Holds(std::int64_t cell) const {
    const auto held = byPlant_.find(cell);
    if (held == byPlant_.end()) return false;

    const Slot &slot = slots_[static_cast<std::size_t>(held->second)];

    return slot.taken && slot.cell == cell;
}

void Sheet::Create() {
    Unload();

    Image image = GenImageColor(kColumns * kSlotW, kRows * kSlotH, BLANK);

    texture_ = LoadTextureFromImage(image);
    UnloadImage(image);

    // Nearest, and it matters more here than anywhere: the whole point of
    // drawing at one texel per square is that a texel arrives on screen as a
    // square, and any filter at all turns the notches this spent its time
    // cutting into a smear.
    SetTextureFilter(texture_, TEXTURE_FILTER_POINT);

    slots_.assign(static_cast<std::size_t>(Capacity()), Slot{});
    lookup_.clear();
    byPlant_.clear();

    frame_ = 0;
}

void Sheet::Unload() {
    if (texture_.id != 0) UnloadTexture(texture_);

    texture_ = {};

    slots_.clear();
    lookup_.clear();
    byPlant_.clear();

    frame_          = 0;
    drawnThisFrame_ = 0;
}

void Sheet::Begin() {
    frame_++;
    drawnThisFrame_ = 0;
}

const Sprite *Sheet::Acquire(const flora::Plant &plant, flora::Stage stage, flora::Season season, bool snowy) {
    if (!Ready()) return nullptr;

    const std::uint64_t key = Key(plant.id, stage, season, snowy);

    if (const auto found = lookup_.find(key); found != lookup_.end()) {
        Slot &slot = slots_[static_cast<std::size_t>(found->second)];
        slot.used  = frame_;

        return &slot.sprite;
    }

    // Whatever this plant already has, for the two cases below where it cannot be
    // drawn now. Wrong season for a frame or two beats absent for a frame or two.
    const auto Held = [this](std::int64_t cell) -> Sprite * {
        const auto held = byPlant_.find(cell);
        if (held == byPlant_.end()) return nullptr;

        Slot &slot = slots_[static_cast<std::size_t>(held->second)];
        if (!slot.taken || slot.cell != cell) return nullptr;

        slot.used = frame_;

        return &slot.sprite;
    };

    if (drawnThisFrame_ >= kDrawBudget) return Held(plant.id);

    // A free slot, or the one that has gone longest without being asked for.
    // Never one asked for this frame or the last, which would take the sprite of
    // a tree still on screen.
    int chosen           = -1;
    std::uint64_t oldest = frame_;

    for (int i = 0; i < static_cast<int>(slots_.size()); i++) {
        const Slot &slot = slots_[static_cast<std::size_t>(i)];

        if (!slot.taken) {
            chosen = i;
            break;
        }

        if (slot.used + 1 >= frame_) continue;

        if (chosen < 0 || slot.used < oldest) {
            chosen = i;
            oldest = slot.used;
        }
    }

    if (chosen < 0) return Held(plant.id);

    Slot &slot = slots_[static_cast<std::size_t>(chosen)];

    if (slot.taken) {
        lookup_.erase(slot.key);

        // Only if this slot is still the one that plant is found through. A plant
        // drawn at two stages has two slots, and evicting the older must not
        // unhook the newer.
        if (const auto held = byPlant_.find(slot.cell); held != byPlant_.end() && held->second == chosen) {
            byPlant_.erase(held);
        }
    }

    Draw(plant, stage, season, snowy, slot, chosen % kColumns, chosen / kColumns);

    slot.key   = key;
    slot.cell  = plant.id;
    slot.used  = frame_;
    slot.taken = true;

    lookup_[key]        = chosen;
    byPlant_[plant.id]  = chosen;
    drawnThisFrame_++;

    return &slot.sprite;
}

int SlotWidth() { return kSlotW; }
int SlotHeight() { return kSlotH; }

void Render(const flora::Plant &plant, flora::Stage stage, flora::Season season, bool snowy,
            std::vector<Color> &pixels, int &width, int &height, Vector2 &anchor) {
    const flora::SpeciesDef &def = flora::Def(plant.species);
    const float pixel            = config::kFloraPixel;

    const flora::Skeleton skeleton = flora::Build(plant.species, stage, plant.id, plant.scale);

    float top    = skeleton.height;
    float bottom = 0.0f;

    for (int i = 0; i < skeleton.lobeCount; i++) {
        const flora::Lobe &lobe = skeleton.lobes[i];
        const float reach       = lobe.radius * lobe.flatten;

        top    = std::max(top, lobe.at.y + reach);
        bottom = std::min(bottom, lobe.at.y - reach);
    }

    Frame frame;
    frame.pixel = pixel;
    frame.left  = skeleton.left - kPad * pixel;
    frame.top   = top + kPad * pixel;

    const int wantW = static_cast<int>(std::ceil((skeleton.right - skeleton.left) / pixel)) + kPad * 2;
    const int wantH = static_cast<int>(std::ceil((top - bottom) / pixel)) + kPad * 2;
    width  = std::clamp(wantW, 1, kSlotW);
    height = std::clamp(wantH, 1, kSlotH);

    // A seed that separates one tree from the next, so no two of them tear along
    // the same line.
    const auto seed =
        static_cast<int>((plant.id * 97 + flora::StageIndex(stage) * 13 + flora::SeasonIndex(season)) * 31 + 5);

    const bool bare = def.deciduous && season == flora::Season::Winter;

    Canvas canvas;
    canvas.Fit(width, height);

    if (!bare) LayFoliage(skeleton, def.shape, frame, seed, canvas);

    // After the tears, so what it measures is the shelter the finished crown
    // gives rather than the one it would have given unbroken.
    LaySun(canvas);

    LayWood(skeleton, frame, bare, seed, canvas);
    Paint(canvas, def.palette[flora::SeasonIndex(season)], snowy, seed, pixels);

    width  = canvas.w;
    height = canvas.h;
    anchor = {frame.ToX(0.0f), frame.ToY(0.0f)};
}

void Sheet::Draw(const flora::Plant &plant, flora::Stage stage, flora::Season season, bool snowy, Slot &slot,
                 int column, int row) {
    int width  = 0;
    int height = 0;
    Vector2 anchor{};

    Render(plant, stage, season, snowy, pixels_, width, height, anchor);

    const Rectangle where = {static_cast<float>(column * kSlotW), static_cast<float>(row * kSlotH),
                             static_cast<float>(width), static_cast<float>(height)};

    UpdateTextureRec(texture_, where, pixels_.data());

    slot.sprite = {.source = where, .anchor = anchor};
}

} // namespace canopy
