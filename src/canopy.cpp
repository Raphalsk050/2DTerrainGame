#include "canopy.h"

#include "config.h"

#include <algorithm>
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
constexpr float kKeyX = -0.34f;
constexpr float kKeyY = -0.94f;

// How far into the crown the shading looks for something above it, in texels.
// Three samples rather than a march: what is being asked is roughly how buried a
// mass is, and the third sample answers it.
constexpr float kReach[3] = {2.0f, 4.5f, 8.0f};

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
// term may move it.
//
// The base matters more than any of them. Worked out from the exposure alone,
// every texel with foliage over it came out at the bottom of the ramp — which is
// most of a crown — so the inside was one flat dark green and only the rim had
// any other tone. Real foliage does not do that: leaves inside a canopy sit at
// every angle and catch light at all of them. So the ramp starts near its middle
// and the terms below move it a step or two either way, which is what puts every
// green in the palette somewhere in every part of the crown.
constexpr float kLeafBase   = 0.56f;
constexpr float kLeafLight  = 0.34f; // How much being open to the key lifts it.
constexpr float kLeafDepth  = 0.20f; // How much sitting low in the crown drops it.
constexpr float kLeafSpread = 0.42f; // A bunch's own luck, against its neighbours.
constexpr float kLeafRound  = 0.30f; // The turn within one bunch.

// The sheet, in texels. One slot holds the largest plant the table can produce
// at the largest size it rolls.
constexpr int kSlotW = 96;
constexpr int kSlotH = 136;

constexpr int kColumns = 10;
constexpr int kRows    = 10;

// Plants drawn per frame. Walking into a wood brings a few trees into view a
// second; this only binds when the view jumps, and then it spreads the cost over
// the frames after it rather than spending it all in one.
constexpr int kDrawBudget = 6;

// Texels of clear space left around a plant inside its slot.
constexpr int kPad = 2;

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
    std::vector<float> wood;  // Trunk and branch coverage.
    std::vector<float> grain; // Signed offset across the trunk, for its two sides.

    void Fit(int width, int height) {
        w = std::clamp(width, 1, kSlotW);
        h = std::clamp(height, 1, kSlotH);

        const auto count = static_cast<std::size_t>(w) * h;

        leaf.assign(count, 0.0f);
        depth.assign(count, 0.0f);
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

                // The mass that covers a texel most owns its depth. Otherwise a
                // mass at the top of the crown lends its brightness to
                // everything hanging under it.
                if (value > canvas.leaf[at]) canvas.depth[at] = lobe.depth;

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
void LayWood(const flora::Skeleton &skeleton, const Frame &frame, bool bare, Canvas &canvas) {
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

    for (int i = 0; i < skeleton.branchCount; i++) {
        const flora::Branch &branch = skeleton.branches[i];

        // Bare, a limb is drawn the whole way to where its leaves would have
        // been, because in winter the limbs are the tree.
        const Vector2 end = bare ? branch.tip : branch.to;

        const Vector2 a = {frame.ToX(branch.from.x), frame.ToY(branch.from.y)};
        const Vector2 b = {frame.ToX(end.x), frame.ToY(end.y)};

        const float half = std::max(branch.width * 0.5f / frame.pixel, 0.5f);

        sweep(a, b, half, std::max(half * 0.55f, 0.5f));
    }
}

// How exposed a texel is to the direction the form is shaded against.
float Exposure(const Canvas &canvas, int x, int y) {
    float buried = 0.0f;

    for (const float reach : kReach) {
        const float sx = static_cast<float>(x) + kKeyX * reach;
        const float sy = static_cast<float>(y) + kKeyY * reach;

        if (canvas.LeafAt(sx, sy) > kLeafThreshold) buried += 1.0f;
    }

    return 1.0f - buried / static_cast<float>(std::size(kReach));
}

void Paint(const Canvas &canvas, const flora::SpeciesPalette &palette, int seed, std::vector<Color> &out) {
    out.assign(static_cast<std::size_t>(canvas.w) * canvas.h, Color{0, 0, 0, 0});

    Color ramp[kLeafTones];
    BuildRamp(palette, ramp);

    // The accent under a mass of leaves, and along the seam between two of them.
    //
    // The bottom of the ramp itself rather than a colour of its own. A hard drop
    // to near-black reads as an over-sharpened edge rather than as shade: the
    // crown comes out looking like a filter was run over it, and the eye sees the
    // seams instead of the leaves. What separates one bunch from the next only
    // has to be the darkest green there is, not a darker colour than there is.
    const Color accent = ramp[0];

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
                Leaf leaf = LeafAt(fx, fy, seed);

                const bool underside = canvas.LeafAt(fx, fy + 1.6f) <= kLeafThreshold;

                if (underside || leaf.edge < kLeafEdge) {
                    // The bottom of the crown, and the seams between one bunch
                    // and the next. The reference art puts its darkest pixels in
                    // exactly these two places, and between them they are most of
                    // what gives a flat crown its depth.
                    colour = accent;
                } else {
                    // Started near the middle of the ramp rather than at the
                    // bottom of it, so that every term below moves the tone
                    // within the palette instead of pinning it against the dark
                    // end. See kLeafBase.
                    float lit = kLeafBase;

                    // How open the *bunch* is to the key. Read at the bunch's own
                    // centre rather than at this texel, so a bunch is one tone and
                    // not a smear across two.
                    lit += (Exposure(canvas, static_cast<int>(std::lround(leaf.at.x)),
                                     static_cast<int>(std::lround(leaf.at.y))) -
                            0.5f) *
                           kLeafLight;

                    lit -= canvas.depth[at] * kLeafDepth;

                    // Then the bunch's own luck, so neighbouring bunches at the
                    // same depth are not the same green. With a ramp this fine
                    // this is worth a couple of steps, which is the sprinkle of
                    // near-alike greens the whole arrangement is for.
                    lit += (leaf.tint - 0.5f) * kLeafSpread;

                    // And a turn within the bunch: the side facing the key catches
                    // a step more. Two or three texels across, this is what gives
                    // each one its roundness.
                    lit -= (leaf.away - 0.5f) * kLeafRound;

                    colour = ramp[std::clamp(static_cast<int>(lit * kLeafTones), 0, kLeafTones - 1)];
                }
            } else if (canvas.wood[at] > 0.0f) {
                const float across = canvas.grain[at];

                colour = (across < -0.15f) ? palette.barkLight : (across < 0.45f ? palette.bark : palette.barkDark);

                // Bark: a scatter of darker texels and the occasional notch
                // right across. Both are what stops a trunk reading as three
                // flat stripes of brown.
                if (Value(fx / kBarkPeriod, fy / (kBarkPeriod * 2.4f), seed + 53) > 0.66f) colour = Shade(colour, 0.78f);
                if (Value(fx / 9.0f, fy / 1.15f, seed + 71) > 0.80f) colour = Shade(colour, 0.62f);
            }

            out[at] = colour;
        }
    }
}

} // namespace

std::uint64_t Sheet::Key(std::int64_t cell, flora::Stage stage, flora::Season season) {
    // A cell index shifted up by four, which is exact for any world anyone can
    // walk across: it leaves sixty bits of cell, and a cell is a hundred pixels.
    return (static_cast<std::uint64_t>(cell) << 4) | (flora::StageIndex(stage) << 2) | flora::SeasonIndex(season);
}

int Sheet::Capacity() const { return kColumns * kRows; }

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

    frame_ = 0;
}

void Sheet::Unload() {
    if (texture_.id != 0) UnloadTexture(texture_);

    texture_ = {};

    slots_.clear();
    lookup_.clear();

    frame_          = 0;
    drawnThisFrame_ = 0;
}

void Sheet::Begin() {
    frame_++;
    drawnThisFrame_ = 0;
}

const Sprite *Sheet::Acquire(const flora::Plant &plant, flora::Stage stage, flora::Season season) {
    if (!Ready()) return nullptr;

    const std::uint64_t key = Key(plant.id, stage, season);

    if (const auto found = lookup_.find(key); found != lookup_.end()) {
        Slot &slot = slots_[static_cast<std::size_t>(found->second)];
        slot.used  = frame_;

        return &slot.sprite;
    }

    if (drawnThisFrame_ >= kDrawBudget) return nullptr;

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

    if (chosen < 0) return nullptr;

    Slot &slot = slots_[static_cast<std::size_t>(chosen)];

    if (slot.taken) lookup_.erase(slot.key);

    Draw(plant, stage, season, slot, chosen % kColumns, chosen / kColumns);

    slot.key   = key;
    slot.used  = frame_;
    slot.taken = true;

    lookup_[key] = chosen;
    drawnThisFrame_++;

    return &slot.sprite;
}

int SlotWidth() { return kSlotW; }
int SlotHeight() { return kSlotH; }

void Render(const flora::Plant &plant, flora::Stage stage, flora::Season season, std::vector<Color> &pixels,
            int &width, int &height, Vector2 &anchor) {
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

    width = std::clamp(static_cast<int>(std::ceil((skeleton.right - skeleton.left) / pixel)) + kPad * 2, 1, kSlotW);
    height = std::clamp(static_cast<int>(std::ceil((top - bottom) / pixel)) + kPad * 2, 1, kSlotH);

    // A seed that separates one tree from the next, so no two of them tear along
    // the same line.
    const auto seed =
        static_cast<int>((plant.id * 97 + flora::StageIndex(stage) * 13 + flora::SeasonIndex(season)) * 31 + 5);

    const bool bare = def.deciduous && season == flora::Season::Winter;

    Canvas canvas;
    canvas.Fit(width, height);

    if (!bare) LayFoliage(skeleton, def.shape, frame, seed, canvas);

    LayWood(skeleton, frame, bare, canvas);
    Paint(canvas, def.palette[flora::SeasonIndex(season)], seed, pixels);

    width  = canvas.w;
    height = canvas.h;
    anchor = {frame.ToX(0.0f), frame.ToY(0.0f)};
}

void Sheet::Draw(const flora::Plant &plant, flora::Stage stage, flora::Season season, Slot &slot, int column,
                 int row) {
    int width  = 0;
    int height = 0;
    Vector2 anchor{};

    Render(plant, stage, season, pixels_, width, height, anchor);

    const Rectangle where = {static_cast<float>(column * kSlotW), static_cast<float>(row * kSlotH),
                             static_cast<float>(width), static_cast<float>(height)};

    UpdateTextureRec(texture_, where, pixels_.data());

    slot.sprite = {.source = where, .anchor = anchor};
}

} // namespace canopy
