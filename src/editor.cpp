#include "editor.h"

#include "hotbar.h"

#include <iterator>

#include <algorithm>
#include <cmath>

namespace {

// Colour of the cursor over ground the left hand can take apart. Distinct from
// every material in the table, so the ring never reads as a preview of what is
// about to be placed.
constexpr Color kDigColor = {235, 84, 84, 255};

// And over wood, where the left hand becomes an axe. Warm rather than red, so the
// two read as different tools at a glance and not as two states of one.
constexpr Color kChopColor = {242, 196, 106, 255};

// And over ground that is out of reach. Grey and faint, because what it is
// saying is that neither hand can act here — a coloured ring out there would be
// a promise about a click that is going to do nothing.
constexpr Color kFarColor = {150, 156, 168, 255};

// The line a ghost stands on. Pale green, which is the one colour on the bar that
// already means something growing.
constexpr Color kPlantColor = {150, 214, 120, 255};

} // namespace

void Editor::Bank(const World::Yield &freed, Inventory &inventory, Drops &drops, Vector2 at, float away, float now) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (freed[e] <= 0) continue;

        owed_[e] += static_cast<float>(freed[e]);

        const int blocks = static_cast<int>(std::floor(owed_[e] / kVerticesPerBlock));
        if (blocks <= 0) continue;

        owed_[e] -= static_cast<float>(blocks) * kVerticesPerBlock;

        const Stack dug = BlocksOf(static_cast<Element>(e), blocks);

        const int refused = inventory.Add(dug);
        if (refused <= 0) continue;

        // A full bag does not stop the pick. What there was no room for lands at
        // the cursor and waits, which is what Minecraft does and the only answer
        // that does not either destroy the material or refuse the swing.
        drops.Scatter({.holds = dug.holds, .what = dug.what, .count = refused}, at, away, now);
    }
}

const char *Editor::Lay(World &world, Inventory &inventory, Drops &drops, Vector2 target, Rectangle body, float away,
                        float now) {
    const Stack &held = inventory.Held();

    const Element element = held.AsElement();
    const std::size_t e   = ElementIndex(element);

    // Everything the player has of it, counted in the unit the brush spends. The
    // blocks in the slot plus whatever fraction of one was left over from digging
    // it up, which is the same store read from the other end.
    const float have = static_cast<float>(held.count) * kVerticesPerBlock + owed_[e];
    const int budget = static_cast<int>(std::floor(have));

    if (budget <= 0) return nullptr;

    const World::Stroke stroke = world.Place(element, target, radius_, budget, body);

    // What is left is what was there minus what went into the ground, and the
    // slot is then set to however many whole blocks that comes to. Recomputing
    // the count rather than decrementing it is what keeps the fraction and the
    // slot from ever disagreeing about the same material.
    const float left = have - static_cast<float>(stroke.filled);
    const int blocks = static_cast<int>(std::floor(left / kVerticesPerBlock));

    owed_[e] = left - static_cast<float>(blocks) * kVerticesPerBlock;

    if (held.count > blocks) inventory.Take(inventory.Selected(), held.count - blocks);

    // Placing is a replacement, so a brush pressed into a seam of ore hands the
    // ore back rather than destroying it.
    Bank(stroke.freed, inventory, drops, target, away, now);

    return nullptr;
}


namespace {

// ------------------------------------------------------------------ the icons
//
// The cursor says which of the two tools the click will be, and it says it by
// drawing the tool. This is the house standard for any icon drawn into the world,
// and it is written down here because the next one should match it:
//
//   * Outline only. Nothing is filled — a solid mark at the cursor hides the very
//     thing it is aimed at, and the world behind has to stay readable through it.
//   * One stroke weight for every icon, and it is held in *screen* pixels rather
//     than world ones, so the mark is the same weight at every zoom. That is the
//     one rule a world-space icon gets wrong by default.
//   * Round caps and joins, done by dropping a disc at each joint. Mitred corners
//     at this weight come out as spikes.
//   * Authored as a handful of control points and smoothed, not as line segments.
//     A dozen points through a curve is a shape somebody can retune; forty
//     segments is a shape nobody will touch again.
//   * Drawn in the colour the cursor already uses for that hand, never a colour of
//     the icon's own — the mark and the ring have to agree about what they mean.
//
// The shapes themselves are authored upright in a square running -1 to 1, with Y
// down as everywhere else on screen, and turned when they are drawn.

// Stroke weight, in screen pixels. Taken from the axe, which is the finer of the
// two drawings, and then given to both so they read as one set.
constexpr float kIconStroke = 1.7f;

// How large an icon stands at the cursor, in screen pixels, corner to corner.
constexpr float kIconSize = 26.0f;

// A quarter turn anticlockwise, near enough, so the axe hangs on the diagonal the
// way a tool does in a hand rather than standing to attention.
constexpr float kAxeTurn = -30.0f * 3.14159265f / 180.0f;

// One point along a Catmull-Rom through four controls. It passes through its
// controls, which is what makes a shape authored as points come out as the shape
// that was authored.
Vector2 Curved(Vector2 a, Vector2 b, Vector2 c, Vector2 d, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;

    return {0.5f * (2.0f * b.x + (c.x - a.x) * t + (2.0f * a.x - 5.0f * b.x + 4.0f * c.x - d.x) * t2
                    + (-a.x + 3.0f * b.x - 3.0f * c.x + d.x) * t3),
            0.5f * (2.0f * b.y + (c.y - a.y) * t + (2.0f * a.y - 5.0f * b.y + 4.0f * c.y - d.y) * t2
                    + (-a.y + 3.0f * b.y - 3.0f * c.y + d.y) * t3)};
}

// Draws one closed outline, smoothed, turned, and scaled to world units.
//
// `thick` arrives in world units — the caller divides the screen weight by the
// zoom, once, rather than this having to know about the camera.
void Outline(const Vector2 *points, int count, Vector2 at, float scale, float turn, float thick, Color color) {
    if (count < 3) return;

    const float sine   = std::sin(turn);
    const float cosine = std::cos(turn);

    const auto place = [&](Vector2 p) {
        return Vector2{at.x + (p.x * cosine - p.y * sine) * scale, at.y + (p.x * sine + p.y * cosine) * scale};
    };

    // Samples per span. Four is enough at this size: a span is a few pixels on
    // screen and the eye cannot find the joins.
    constexpr int kSteps = 4;

    Vector2 was = place(points[0]);

    for (int i = 0; i < count; i++) {
        const Vector2 a = points[(i - 1 + count) % count];
        const Vector2 b = points[i];
        const Vector2 c = points[(i + 1) % count];
        const Vector2 d = points[(i + 2) % count];

        for (int step = 1; step <= kSteps; step++) {
            const Vector2 now = place(Curved(a, b, c, d, static_cast<float>(step) / static_cast<float>(kSteps)));

            DrawLineEx(was, now, thick, color);

            // The join, which is also the cap. Without it every bend in the outline
            // shows daylight through its outside corner.
            DrawCircleV(now, thick * 0.5f, color);

            was = now;
        }
    }
}

// The axe: a flared head with a squared poll, and a haft falling from under it that
// tapers and flicks out at the butt. Two outlines, because a haft that ran into the
// head as one silhouette loses the step between them at this size.
constexpr Vector2 kAxeHead[] = {
    {-0.60f, -0.86f}, {-0.16f, -0.92f}, {0.30f, -0.90f}, {0.52f, -0.84f},
    {0.56f, -0.62f},  {0.54f, -0.44f},  {0.44f, -0.38f}, {0.10f, -0.36f},
    {-0.24f, -0.42f}, {-0.52f, -0.54f}, {-0.70f, -0.70f},
};

constexpr Vector2 kAxeHaft[] = {
    {0.12f, -0.34f}, {0.34f, -0.34f}, {0.36f, 0.14f}, {0.32f, 0.62f},
    {0.36f, 0.86f},  {0.20f, 0.94f},  {0.10f, 0.72f}, {0.14f, 0.24f},
};

// The shovel: a spade blade, a shaft on the diagonal, and a D-grip at the top with
// its hole left open. Four outlines — the grip's hole is the reason this cannot be
// one, and having it is most of what says shovel rather than paddle.
constexpr Vector2 kSpadeBlade[] = {
    {-0.86f, 0.14f}, {-0.96f, 0.44f}, {-0.86f, 0.76f}, {-0.60f, 0.92f},
    {-0.34f, 0.78f}, {-0.24f, 0.48f}, {-0.34f, 0.20f}, {-0.60f, 0.08f},
};

constexpr Vector2 kSpadeShaft[] = {
    {-0.50f, 0.18f}, {0.30f, -0.62f}, {0.44f, -0.48f}, {-0.36f, 0.32f},
};

constexpr Vector2 kSpadeGrip[] = {
    {0.26f, -0.66f}, {0.44f, -0.92f}, {0.68f, -0.94f},
    {0.80f, -0.74f}, {0.70f, -0.52f}, {0.44f, -0.50f},
};

constexpr Vector2 kSpadeHole[] = {
    {0.44f, -0.70f},
    {0.54f, -0.82f},
    {0.66f, -0.76f},
    {0.58f, -0.62f},
};

// The two tools, each as the outlines it is made of, so a caller draws one by name
// rather than by remembering which pieces belong to which.
void DrawAxe(Vector2 at, float scale, float thick, Color color) {
    Outline(kAxeHead, static_cast<int>(std::size(kAxeHead)), at, scale, kAxeTurn, thick, color);
    Outline(kAxeHaft, static_cast<int>(std::size(kAxeHaft)), at, scale, kAxeTurn, thick, color);
}

void DrawSpade(Vector2 at, float scale, float thick, Color color) {
    Outline(kSpadeBlade, static_cast<int>(std::size(kSpadeBlade)), at, scale, 0.0f, thick, color);
    Outline(kSpadeShaft, static_cast<int>(std::size(kSpadeShaft)), at, scale, 0.0f, thick, color);
    Outline(kSpadeGrip, static_cast<int>(std::size(kSpadeGrip)), at, scale, 0.0f, thick, color);
    Outline(kSpadeHole, static_cast<int>(std::size(kSpadeHole)), at, scale, 0.0f, thick, color);
}

} // namespace


const char *Editor::Update(World &world, Inventory &inventory, Grove &grove, const Camera2D &camera, Rectangle body,
                           float now) {
    // Where the character is, for the reach and for which side a spilled block
    // is thrown out on. The middle of the body, which is where the arm is.
    const Vector2 player = {body.x + body.width / 2.0f, body.y + body.height / 2.0f};

    // Sized with keys rather than the wheel, which already steps through the
    // hotbar. Two meanings on one control is exactly the kind of thing having a
    // button per hand is here to avoid.
    //
    // Both pairs are bound because raylib names keys by their place on a US
    // layout. The brackets land somewhere else entirely on other layouts, while
    // minus and equals keep their position, so the pair that is advertised is
    // the pair that can be relied on to be under the printed key.
    const bool smaller = IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_KP_SUBTRACT);
    const bool larger  = IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_KP_ADD);

    if (smaller) radius_ = std::max(radius_ - kRadiusStep, kMinRadius);
    if (larger) radius_ = std::min(radius_ + kRadiusStep, kMaxRadius);

    // The latch comes off the moment the button does, wherever this frame returns
    // from. A mode that outlived its press would make the *next* click inherit the
    // last one's tool.
    if (IsMouseButtonUp(MOUSE_BUTTON_LEFT)) left_ = Hand::Idle;

    const Vector2 mouse = GetMousePosition();

    // The bar sits over the world it edits, so a click that lands on it belongs
    // to the bar alone. The cursor is dropped as well, otherwise it would hang
    // over the slots as if they were something to dig.
    if (hotbar::Contains(mouse)) {
        under_.reset();
        reachable_ = false;
        timber_    = false;
        return nullptr;
    }

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    const float dx = target.x - player.x;
    const float dy = target.y - player.y;

    aim_       = target;
    under_     = world.OccupantAt(target);
    reachable_ = (dx * dx + dy * dy) <= kReach * kReach;

    // Where the thing in hand would come to rest, if it is the kind of thing that
    // rests anywhere. Worked out here, once, and used by both the ghost that shows
    // the player where it will go and the click that puts it there — one answer,
    // or the preview is a promise the click does not keep.
    footing_.reset();
    rooted_ = false;

    if (reachable_ && inventory.Held().holds == Holds::Item && inventory.Held().count > 0
        && Def(inventory.Held().AsItem()).placement != Placement::None) {
        float top = 0.0f;

        if (world.FootingUnder(target, kDropReach, top)) {
            footing_ = Vector2{target.x, top};

            // And what that ground is made of, read half a lattice step under the
            // surface — the same place the grass is asked about, and for the same
            // reason: on the surface itself the answer is whatever the contour
            // rounds to.
            const float under = top + static_cast<float>(world.Spacing()) * 0.5f;

            rooted_ = world.OccupantAt({target.x, under}) == Element::Soil;
        }
    }

    // Which tool this press is, decided the once and held — see Editor::Hand.
    //
    // Out of reach counts as ground rather than as nothing, so a click into the
    // distance is a dig that does not reach rather than a hand with no tool: the
    // player who then drags the cursor back into range is digging, which is what
    // they were plainly asking for.
    // Whether the wood is what the cursor is over, asked every frame rather than
    // only on the press — the cursor has to say which tool the click will be
    // *before* it is clicked, or an automatic mode is a mode the player cannot see.
    const Rectangle probe = {target.x - kAimSlack, target.y - kAimSlack, kAimSlack * 2.0f, kAimSlack * 2.0f};

    timber_ = reachable_ && grove.TimberAt(probe, now);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) left_ = timber_ ? Hand::Chop : Hand::Dig;

    if (!reachable_) return nullptr;

    // The left hand digs only where this press was a dig. Where it was a chop the
    // swing belongs to the caller, and this must keep its hands off the ground —
    // otherwise a player felling a tree quietly excavates the hillside behind it.
    const bool digging = left_ == Hand::Dig;
    const bool placing = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

    if (!digging && !placing) return nullptr;

    // Thrown away from the player rather than towards them, the same way the wood
    // off a felled tree goes.
    const float away = (target.x < player.x) ? -1.0f : 1.0f;

    if (digging) {
        Bank(world.Excavate(target, radius_).freed, inventory, grove.Fallen(), target, away, now);
        return nullptr;
    }

    const Stack &held = inventory.Held();

    if (held.holds == Holds::Material) return Lay(world, inventory, grove.Fallen(), target, body, away, now);

    // Everything past here answers the press rather than the hold. A brush lays
    // material for as long as the button is down because a stroke is a continuous
    // thing; a sapling is one sapling, and holding the button over a wood would
    // otherwise plant the whole stack in a second.
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return nullptr;

    if (held.Empty()) return "nothing in hand";

    // Everything that goes into the world whole rather than by the fistful goes
    // in the same way: on the ground the cursor found, which is the ground the
    // ghost has been standing on since the hand came near it.
    if (Def(held.AsItem()).placement == Placement::Plant) {
        if (!footing_.has_value()) return "nothing to plant it on";
        if (!rooted_) return "a sapling needs soil to root in";

        // Whatever tree this seed is, wherever the player is standing.
        //
        // It used to be the species the climate favoured there, which read as the
        // world knowing best and meant the player could not choose: an oak sapling
        // carried into a cold valley came up a pine. What the climate decides is
        // what grows on its own — the scatter's business, and still is. A seed in
        // a hand is one particular tree, and Minecraft plants a jungle sapling in
        // a snowfield without comment.
        const std::optional<flora::Species> seed = flora::SpeciesOf(held.AsItem());
        if (!seed.has_value()) return "that is not something to put down";

        if (!grove.Plant(*seed, *footing_, now)) {
            return "no room here — something is already growing";
        }

        inventory.Take(inventory.Selected(), 1);

        return nullptr;
    }

    return "that is not something to put down";
}

void Editor::DrawCursor(const Inventory &inventory, const Grove &grove, flora::Season season,
                        const Camera2D &camera) const {
    const Vector2 mouse = GetMousePosition();
    if (hotbar::Contains(mouse)) return;

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    const Stack &carried = inventory.Held();

    // A thing that goes into the world whole gets a ghost of itself standing
    // where it would stand, and no ring: the ring is about an area a brush
    // covers, and there is no area here — there is one spot, and the honest way
    // to point at it is to draw the thing on it.
    if (carried.holds == Holds::Item && carried.count > 0) {
        const Placement placement = Def(carried.AsItem()).placement;

        if (placement != Placement::None) {
            if (footing_.has_value()) {
                // Green where the seed will take and red where the ground will
                // not have it, so the refusal arrives before the click rather than
                // as a line of text after it.
                const Color says = rooted_ ? kPlantColor : kDigColor;

                if (placement == Placement::Plant) {
                    const std::optional<flora::Species> seed = flora::SpeciesOf(carried.AsItem());

                    if (seed.has_value()) grove.DrawGhost(*seed, *footing_, season, says);
                }

                // The line it is standing on, so the spot is unambiguous even
                // where the ghost is a wisp of a thing against a bright hillside.
                DrawLineV({footing_->x - 8.0f, footing_->y}, {footing_->x + 8.0f, footing_->y}, says);
            } else {
                // Nowhere to put it. Said with the same grey the ring uses out of
                // reach, which is the same thing being said: this click will do
                // nothing.
                DrawLineV({target.x - 5.0f, target.y}, {target.x + 5.0f, target.y}, kFarColor);
                DrawLineV({target.x, target.y - 5.0f}, {target.x, target.y + 5.0f}, kFarColor);
            }

            return;
        }
    }

    // The ring carries what the hands can do here. Out of reach it goes grey and
    // says neither of them can; in reach it takes the colour of whatever the
    // right hand would put down, or the digging colour where there is nothing to
    // put down and only the left hand is any use.
    //
    // Reading that off the cursor is what keeps two buttons workable without a
    // badge somewhere else saying which is which, since the eye is already here.
    const Stack &held = inventory.Held();

    // Over wood, the left hand is an axe and the ring would be a lie: the brush
    // radius says an area will come away, and what will actually happen is one blow
    // to one trunk. Drawn as a pair of notches instead — a mark that is plainly not
    // the digging ring, so the player can see the tool change under the cursor
    // before committing to it.
    // Both figures are in screen pixels and both are divided by the zoom, so the
    // mark keeps its size and its weight however far the view is pushed in. A world
    // -sized icon doubles with the zoom and a world-sized stroke doubles with it
    // again, which is how a cursor ends up a blot.
    const float zoom  = std::max(camera.zoom, 0.01f);
    const float thick = kIconStroke / zoom;
    const float scale = kIconSize * 0.5f / zoom;

    if (timber_ && held.holds != Holds::Material) {
        DrawAxe(target, scale, thick, kChopColor);
        return;
    }

    Color color = kFarColor;

    if (Reachable()) {
        color = (held.holds == Holds::Material) ? StyleOf(held.AsElement()).contour : kDigColor;
    }

    DrawCircleLinesV(target, radius_, color);
    DrawCircleLinesV(target, radius_ - 1.0f, Fade(color, 0.5f));

    // The ring says how much comes away and the spade says what the hand is; the
    // two are different questions and the ring cannot answer the second, which is
    // why the mark in the middle is a tool and no longer a cross.
    DrawSpade(target, scale, thick, color);
}
