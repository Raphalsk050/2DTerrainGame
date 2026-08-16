#include "scuff.h"

#include "config.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace scuff {
namespace {

// How long one puff lasts, in seconds.
//
// Short. Dust off a foot is gone before the next foot lands, and anything longer
// leaves a trail hanging in the air behind a runner like a vapour contrail — which
// reads as smoke rather than as ground being disturbed.
constexpr float kLife = 0.42f;

// Specks in one puff at full force, and the fewest a footfall draws at all.
constexpr int kMostSpecks  = 7;
constexpr int kLeastSpecks = 2;

// How far apart feet come down, in world pixels.
//
// A stride rather than an interval — see Trail::strode_. About two thirds of the
// character's height, which is what a walk looks like.
constexpr float kStride = 22.0f;

// Speeds, in world pixels per second: below the first nothing is kicked up at all,
// and at the second the ground is being torn at.
constexpr float kIdleSpeed = 26.0f;
constexpr float kHardSpeed = 190.0f;

// Falling speed at which a landing throws everything it has.
constexpr float kHardFall = 460.0f;

// How fast a speck leaves the foot, in world pixels per second, and how hard
// gravity pulls it back. Light: this is dust, not gravel.
//
// The upward figure is set against the grass rather than by eye, and that is the
// only way to set it: a blade stands kBladeTall texels high — some sixteen world
// pixels — and dust that peaks below that is dust nobody ever sees, because it
// spends its whole life inside the sod. A throw of `u` against a settle of `g`
// peaks at `u² / 2g`, so clearing the grass with room to spare is what fixes this
// number. It was a third of what it is and the puffs were vanishing into the field.
constexpr float kThrowBack = 54.0f;
constexpr float kThrowUp   = 92.0f;
constexpr float kSettle    = 150.0f;

// The burst a landing throws, out to both sides instead of behind.
//
// Several times the stride's throw, and it has to be. A landing is one event that
// happens once and is over, where a run lays a puff every stride and is read as the
// sum of them — so the landing has only its own moment to be seen in, and at a
// walking puff's size it simply is not. What reads is a pair of lobes that leave
// the feet and travel: this carries a speck the better part of a body's width.
constexpr float kBurstOut = 165.0f;

// How much of the upward throw a landing keeps. Under one, because what says
// *landing* is dust running out along the ground where what says *running* is dust
// lifting behind — but not far under, or the lobes never clear the grass.
constexpr float kBurstUp = 1.05f;

// Specks in a landing burst at full force. Well over a stride's — two lobes have to
// read as two, and half a handful each reads as a scatter.
constexpr int kBurstSpecks = 24;

// How much longer a landing hangs than a footfall.
//
// A stride's puff has to be gone before the next foot lands or a run leaves a
// contrail; a landing has nothing following it, so it can take its time and be
// watched. This is most of what makes the two read as different events.
constexpr float kBurstLinger = 1.7f;

// How far a puff spreads along the ground over its life, as a share of the throw.
// Dust billows outward as it slows, which is the one thing that separates it from a
// spray of chips.
constexpr float kBillow = 0.55f;

// What each material is worth underfoot, as a share of a full puff.
//
// Written per material rather than derived, for the reason the mood table is: how
// much a ground throws up when it is trodden on is a fact about that ground, and
// nothing else in the element table implies it. Rock is nearly silent, the loose
// grounds are the whole point of the feature, and an ore seam is rock with
// something in it.
constexpr float Loose(Element element) {
    switch (element) {
    case Element::Sand: return 1.00f;
    case Element::Snow: return 0.95f;
    case Element::Soil: return 0.70f;
    case Element::Rock: return 0.22f;

    // Cobble is the rock it came out of, and gives up as little underfoot.
    case Element::Cobblestone: return 0.22f;

    // A plank floor is swept, and water is dealt with by the caller — a foot in
    // it is a splash and this is dust. Both are silent here for the same reason:
    // a puff of dust off a board would say the player had trodden in something,
    // and the whole of a built floor is that they have not.
    case Element::WoodPlank:
    case Element::Water: return 0.0f;

    default: return 0.28f;
    }
}

// The two tones a speck is drawn in: the material's own, one light and one dark.
//
// Off the element's paint rather than a colour of this module's, so a ground
// retuned in the table takes its dust with it — the same contract the leaves keep
// with their species palette.
Color Speck(Element element, bool lit) {
    const ElementPaint &paint = StyleOf(element).paint;

    return lit ? paint.tone[kElementTones - 1] : paint.tone[1];
}

std::uint32_t Bits(int index, int salt) {
    auto bits = static_cast<std::uint32_t>(index * 73856093 + salt * 19349663);

    bits ^= bits >> 15;
    bits *= 2246822519u;
    bits ^= bits >> 13;

    return bits;
}

float Roll(int index, int salt) { return static_cast<float>(Bits(index, salt) & 0xffffu) / 65536.0f; }

// Snapped to the world's own pixel grid, so a speck is a square on the same lattice
// everything else is drawn on rather than a smaller mark floating between them.
float Snap(float value) { return std::floor(value / config::kFloraPixel) * config::kFloraPixel; }

} // namespace

void Trail::Update(const World &world, Rectangle body, float speed, float fall, bool grounded, float now) {
    const Vector2 feet = {body.x + body.width * 0.5f, body.y + body.height};

    if (!started_) {
        wasAt_   = feet;
        started_ = true;
    }

    const float step = feet.x - wasAt_.x;
    const float moved = std::fabs(step);

    // Which way the foot was going, taken before the record of where it was is
    // overwritten. Reading it afterwards compares a position with itself and
    // answers "rightwards" for every step in the world.
    const float away = (step >= 0.0f) ? 1.0f : -1.0f;

    wasAt_ = feet;

    const bool landed = grounded && !wasDown_;

    wasDown_ = grounded;

    if (!grounded) {
        // A stride half-walked when the feet left the ground is not owed on the far
        // side of a jump: landing lays its own step, and carrying the remainder
        // would put a second one down half a pace later.
        strode_ = 0.0f;
        return;
    }

    strode_ += moved;

    // Landing throws whatever the drop was worth, and a stride throws whatever the
    // pace is worth. A landing does not wait for the stride to come round, which is
    // the whole of why the two are separate: the interesting puff is the one at the
    // bottom of a fall.
    float force = 0.0f;

    // Floored well up, unlike a stride's. Every landing is an event worth drawing —
    // a hop from a low ledge that puffs like a footstep reads as nothing having
    // happened, and the player did plainly do something.
    if (landed) force = std::clamp(fall / kHardFall, 0.62f, 1.0f);

    if (strode_ >= kStride && speed > kIdleSpeed) {
        strode_ = 0.0f;

        force = std::max(force, std::clamp((speed - kIdleSpeed) / (kHardSpeed - kIdleSpeed), 0.22f, 1.0f));
    }

    if (force <= 0.0f) return;

    // What is underfoot, read half a lattice step below the soles — the same place
    // the grass and the sapling ask about, and for the same reason: on the surface
    // itself the answer is whatever the contour happened to round to.
    const std::optional<Element> under = world.OccupantAt({feet.x, feet.y + static_cast<float>(world.Spacing()) * 0.5f});

    if (!under.has_value() || Loose(*under) <= 0.0f) return;

    steps_[static_cast<std::size_t>(next_)] = {.at    = feet,
                                               .on    = *under,
                                               .when  = now,
                                               .force = force * Loose(*under),
                                               .away  = away,
                                               .ring  = landed};

    next_ = (next_ + 1) % kSteps;
}

void Trail::Draw(float now) const {
    const float pixel = config::kFloraPixel;

    for (int s = 0; s < kSteps; s++) {
        const Step &step = steps_[static_cast<std::size_t>(s)];

        if (step.when < 0.0f) continue;

        const float life = step.ring ? kLife * kBurstLinger : kLife;

        const float age = now - step.when;
        if (age < 0.0f || age >= life) continue;

        const float flown = age / life;

        // Faded out over the whole life rather than the last of it. Dust thins as it
        // spreads, so there is no moment at which a puff should still be at full
        // strength — unlike a leaf, which is a solid thing and is only faded at the
        // end so that it does not blink out.
        const float fade = (1.0f - flown) * (1.0f - flown);

        const int most = step.ring ? kBurstSpecks : kMostSpecks;

        const int specks =
            kLeastSpecks + static_cast<int>(step.force * static_cast<float>(most - kLeastSpecks) + 0.5f);

        for (int i = 0; i < specks; i++) {
            const int salt = s * 977 + i * 31;

            float sideways = 0.0f;
            float up       = 0.0f;

            if (step.ring) {
                // Two lobes, one ahead and one behind, dealt out by index rather
                // than by a roll — a coin flip per speck leaves one side of a small
                // burst empty about as often as not, and a landing that throws dust
                // one way reads as a stumble.
                const float side = (i % 2 == 0) ? 1.0f : -1.0f;

                // Spread across the lobe rather than all at one speed, so it opens
                // into a fan instead of leaving as a wall.
                sideways = side * kBurstOut * (0.40f + 0.60f * Roll(salt, 3)) * step.force;

                // Lower than a stride's, and deliberately: what says *landing* is
                // dust running out along the ground to either side, where what says
                // *running* is dust lifting behind. Still enough to clear the grass,
                // which is the floor under every figure here.
                up = kThrowUp * kBurstUp * (0.55f + 0.45f * Roll(salt, 7)) * step.force;
            } else {
                // Thrown backwards along the travel and up. Both ends of the
                // sideways term matter: near zero the puff is a column of dust, and
                // past one it is a fan of separate specks.
                sideways = -step.away * kThrowBack * (0.25f + 0.75f * Roll(salt, 3)) * step.force;

                // Floored at better than half, not near zero. A speck thrown at a
                // third of the figure peaks two pixels up — inside the sod, drawn
                // and never seen — so the low end of this range decides how much of
                // every puff exists at all.
                up = kThrowUp * (0.55f + 0.45f * Roll(salt, 7)) * step.force;
            }

            const float spread = (Roll(salt, 11) - 0.5f) * 2.0f * kBillow * kThrowBack * flown;

            const float x = step.at.x + sideways * age + spread;

            // Up, and then settling. Never below the ground it came off, so dust
            // lies on the surface at the end of its life instead of sinking through
            // it.
            const float y = std::min(step.at.y - up * age + 0.5f * kSettle * age * age, step.at.y);

            // Staggered a little, so a puff arrives over two or three frames rather
            // than as a ring leaving the foot together.
            if (age < Roll(salt, 13) * 0.06f) continue;

            const Color tone = Fade(Speck(step.on, Roll(salt, 17) > 0.45f), fade);

            DrawRectangleV({Snap(x), Snap(y)}, {pixel, pixel}, tone);
        }
    }
}

int Trail::Live(float now) const {
    int live = 0;

    for (const Step &step : steps_) {
        if (step.when >= 0.0f && now - step.when < kLife) live++;
    }

    return live;
}

} // namespace scuff
