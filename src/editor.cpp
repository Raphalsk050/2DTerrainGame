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

// And the line laid under the cursor's own, one screen pixel outside it.
//
// Near black, because what it is for is being a colour no material is. A single
// hairline in one colour disappears into whatever it is drawn over — the digging
// red over red sandstone, the grey over rock — and a selected block the player
// cannot find is a block they believe is not selected. Two lines cannot both be
// lost, whatever is behind them. The same reasoning as DrawLabel's outline, and
// the same trick.
constexpr Color kEdgeColor = {12, 14, 18, 205};

} // namespace

void Editor::Bank(const World::Yield &freed, Drops &drops, Vector2 at, float away, float now) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (freed[e] <= 0) continue;

        owed_[e] += static_cast<float>(freed[e]);

        const int blocks = static_cast<int>(std::floor(owed_[e] / kVerticesPerBlock));
        if (blocks <= 0) continue;

        owed_[e] -= static_cast<float>(blocks) * kVerticesPerBlock;

        // What the hand gets, which is not always what was in the ground — see
        // ElementDef::yields. The debt above is still kept against the material
        // that was actually dug, so a seam of rock and a wall of cobble do not
        // pour their remainders into one another.
        const Stack dug = BlocksOf(YieldOf(static_cast<Element>(e)), blocks);

        // Straight onto the ground. One object per block, since Scatter divides a
        // stack one piece per unit up to its bound and a cell yields one.
        drops.Scatter(dug, at, away, now);
    }
}

const char *Editor::Spend(World &world, Inventory &inventory, Drops &drops, Rectangle body, float away, float now) {
    const Stack &held = inventory.Held();

    if (held.Empty()) return "nothing in hand";

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!Block(x0, y0, x1, y1)) return nullptr;

    const Element element = held.AsElement();

    // The rule about *where*, asked of the cell under the cursor rather than of
    // every cell in the block. A block of nine laid against a wall has eight cells
    // touching nothing but each other, and asking each of them separately would
    // make a wide brush refuse everything but its own edge — so the aim is what is
    // judged, which is also what the player is looking at.
    if (building_) {
        if (!roomy_) return "no room — you are standing there";
        if (!founded_) return "nothing here to fix it to";
    }

    // Asked of every material and not only of a built one, and after the two above:
    // a player standing inside the cell is told to move before they are told to dig,
    // because moving is what they have to do either way.
    if (!vacant_) return "something is already there — dig it out first";

    World::Yield freed{};

    const int budget = held.count;

    int spent    = 0;
    bool refused = false;

    for (int cx = x0; cx <= x1 && spent < budget; cx++) {
        for (int cy = y0; cy <= y1 && spent < budget; cy++) {
            // Asked before rather than inferred after, because a cell that filled
            // nothing filled nothing for two quite different reasons — the body is
            // in it, or it already held the material — and only the first is worth
            // saying out loud.
            if (Def(element).rules.blocksBodies && !world.CellClear(cx, cy, body)) {
                refused = true;
                continue;
            }

            const World::Stroke stroke = world.PlaceCell(element, cx, cy, body);

            // Already this material. Not charged and not remarked on: this is the
            // ordinary state of a held button whose cursor has not moved on yet.
            if (stroke.filled <= 0) continue;

            // One block per cell, whatever fraction of the cell was already full.
            // The cell is the unit now, so there is no fraction to carry: what was
            // already there was paid for when it was put there.
            spent++;

            for (std::size_t e = 0; e < kElementCount; e++) freed[e] += stroke.freed[e];
        }
    }

    if (spent > 0) inventory.Take(inventory.Selected(), spent);

    // A stroke can still free something even though placing no longer replaces
    // anything: a liquid is displaced by a solid poured into it, and that liquid has
    // to be handed back rather than destroyed.
    const Rectangle whole = World::CellBounds(x0, y0);

    Bank(freed, drops, {whole.x + whole.width * 0.5f, whole.y + whole.height * 0.5f}, away, now);

    if (spent == 0 && refused) return "no room — you are standing there";

    return nullptr;
}

bool Editor::Block(int &outX0, int &outY0, int &outX1, int &outY1) const {
    if (!onCell_) return false;

    // Centred on the cell under the cursor for an odd span, and hung from it for
    // an even one — there is no cell at the middle of an even block to centre on,
    // and picking the one up and to the left keeps the cursor inside the block at
    // every size rather than on its edge at half of them.
    outX0 = cellX_ - (span_ - 1) / 2;
    outY0 = cellY_ - (span_ - 1) / 2;
    outX1 = outX0 + span_ - 1;
    outY1 = outY0 + span_ - 1;

    return true;
}

bool Editor::Built(const World &world, int cx, int cy) {
    // Asked at the middle of the cell rather than under the cursor. On the cursor
    // the answer at a block's edge is whatever the contour rounded to, and the
    // question here is about the cell as a whole.
    const Rectangle at = World::CellBounds(cx, cy);

    const std::optional<Element> what = world.OccupantAt({at.x + at.width * 0.5f, at.y + at.height * 0.5f});

    return what.has_value() && Def(*what).laying == Laying::Cell;
}

bool Editor::Founded(const World &world, int cx, int cy) const {
    // A wall behind this very cell will hold it, which is how a room gets a floor
    // and a shelf without every piece of it having to reach the ground. It is also
    // what makes the wall worth putting up first, exactly as it is in Terraria:
    // back the room, then build into it.
    if (world.WalledAt(cx, cy)) return true;

    // Otherwise one of the four it shares a side with — holding ground a body
    // cannot walk through, or holding a wall of its own so that a wall can be run
    // out from a wall.
    const auto holds = [&](int nx, int ny) {
        return world.OverlapsSolid(World::CellBounds(nx, ny)) || world.WalledAt(nx, ny);
    };

    return holds(cx - 1, cy) || holds(cx + 1, cy) || holds(cx, cy - 1) || holds(cx, cy + 1);
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

// The world rectangle a block of cells covers.
Rectangle BlockBounds(int cx, int cy, int w, int h) {
    const Rectangle from = World::CellBounds(cx, cy);
    const Rectangle to   = World::CellBounds(cx + w - 1, cy + h - 1);

    return {from.x, from.y, to.x + to.width - from.x, to.y + to.height - from.y};
}

} // namespace

void Editor::LetGo() {
    // Nothing was being broken, or nothing had been put into it yet. The second case
    // matters: a click that lands and comes straight off must not flash a bar.
    if (bite_.takes <= 0.0f || bite_.done <= 0.0f) {
        bite_.takes = 0.0f;
        bite_.done  = 0.0f;

        return;
    }

    bite_.letFrom = 1.0f - std::clamp(bite_.done / bite_.takes, 0.0f, 1.0f);
    bite_.let     = kLetGo;

    bite_.letX = bite_.cx;
    bite_.letY = bite_.cy;
    bite_.letW = bite_.w;
    bite_.letH = bite_.h;

    bite_.takes = 0.0f;
    bite_.done  = 0.0f;
}

Editor::Progress Editor::Biting() const {
    Progress out;

    // Being worked on now: the bar is the health left in the block.
    if (bite_.takes > 0.0f && bite_.done > 0.0f) {
        out.showing = true;
        out.health  = 1.0f - std::clamp(bite_.done / bite_.takes, 0.0f, 1.0f);
        out.ink     = 1.0f;
        out.over    = BlockBounds(bite_.cx, bite_.cy, bite_.w, bite_.h);

        return out;
    }

    // Let go of: the bar runs back up to full and fades as it goes. The two together
    // are the whole message — the work is gone, the block is exactly as it was, and
    // nothing was taken from the player for having stopped.
    if (bite_.let > 0.0f) {
        const float gone = 1.0f - bite_.let / kLetGo;

        out.showing = true;
        out.health  = bite_.letFrom + (1.0f - bite_.letFrom) * gone;
        out.ink     = 1.0f - gone;
        out.over    = BlockBounds(bite_.letX, bite_.letY, bite_.letW, bite_.letH);
    }

    return out;
}

const char *Editor::Update(World &world, Inventory &inventory, Grove &grove, fixture::Fixtures &fixtures,
                           const Camera2D &camera, Rectangle body, float now) {
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

    if (smaller) span_ = std::max(span_ - 1, kMinSpan);
    if (larger) span_ = std::min(span_ + 1, kMaxSpan);

    // The latch comes off the moment the button does, wherever this frame returns
    // from. A mode that outlived its press would make the *next* click inherit the
    // last one's tool.
    if (IsMouseButtonUp(MOUSE_BUTTON_LEFT)) {
        left_ = Hand::Idle;

        LetGo();
    }

    // The bar giving the work back, run here rather than in the draw: it has to keep
    // running through every path below that returns early, and half of them are the
    // reasons it started.
    //
    // On the frame clock and not the world's, because it is a message to the player
    // rather than a thing happening in the world — F7 must not make it flash past.
    if (bite_.let > 0.0f) bite_.let = std::max(bite_.let - GetFrameTime(), 0.0f);

    const Vector2 mouse = GetMousePosition();

    // The bar sits over the world it edits, so a click that lands on it belongs
    // to the bar alone. The cursor is dropped as well, otherwise it would hang
    // over the slots as if they were something to dig.
    if (hotbar::Contains(mouse)) {
        LetGo();

        under_.reset();
        reachable_ = false;
        timber_    = false;
        holding_   = false;
        building_  = false;
        buildable_ = false;
        onCell_    = false;
        return nullptr;
    }

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    const float dx = target.x - player.x;
    const float dy = target.y - player.y;

    aim_       = target;
    from_      = player;
    under_     = world.OccupantAt(target);
    reachable_ = (dx * dx + dy * dy) <= kReach * kReach;

    // Which cell the hand is aimed at. Worked out every frame and whatever is in
    // hand, because both hands want it now: the right one sets a piece into it and
    // the left one takes a piece back out of it.
    //
    // Worked out here for the same reason the footing below is: the grid the player
    // sees and the click that follows it have to be one answer.
    World::ToCell(target, cellX_, cellY_);
    onCell_ = true;

    // Whether there is anything in the block to break — see holding_. Worked out
    // before the reach test below returns, because the cursor is drawn out there
    // too and has the same thing to say about it.
    holding_ = false;

    {
        int hx0 = 0;
        int hy0 = 0;
        int hx1 = 0;
        int hy1 = 0;

        if (Block(hx0, hy0, hx1, hy1)) {
            for (int cx = hx0; cx <= hx1 && !holding_; cx++) {
                for (int cy = hy0; cy <= hy1 && !holding_; cy++) {
                    // A torch is something the click takes, and it is the one thing
                    // in a cell that the ground knows nothing about. Asked first and
                    // cheaply, for the same reason the dig takes it first: it is in
                    // front of the wall, so it is what a cursor over the wall means.
                    if (fixtures.At(cx, cy).has_value()) {
                        holding_ = true;
                        break;
                    }

                    World::Yield holds{};

                    world.CellHolds(cx, cy, holds);

                    for (std::size_t e = 0; e < kElementCount; e++) {
                        if (holds[e] <= 0) continue;

                        holding_ = true;
                        break;
                    }
                }
            }
        }
    }

    // `building_` does not ask whether the cursor is in reach. The grid is how the
    // player finds out where the reach ends, so it has to be drawn out there too —
    // greyed, and refusing, but drawn.
    const Stack &hand = inventory.Held();

    building_ = hand.holds == Holds::Material && hand.count > 0 && Def(hand.AsElement()).laying == Laying::Cell;

    // The two ways a cell can refuse, kept apart because they are two different
    // things for the player to do about it: one asks them to build from somewhere
    // that holds, the other to get out of the way.
    founded_ = building_ && Founded(world, cellX_, cellY_);
    roomy_   = building_
           && (!Def(hand.AsElement()).rules.blocksBodies || world.CellClear(cellX_, cellY_, body));

    // Asked a frame ahead so the square goes red before the button is pressed. The
    // world asks it again and is the one that enforces it — see World::CellVacant.
    //
    // Of any material that occupies and not only of a building one: laying soil over
    // a seam of ore was the same hole as setting a block into it, and it has to
    // refuse and say so rather than quietly doing nothing.
    const bool occupying = hand.holds == Holds::Material && hand.count > 0 && Def(hand.AsElement()).rules.occupies;

    vacant_ = !occupying || world.CellVacant(cellX_, cellY_);

    buildable_ = building_ && reachable_ && founded_ && roomy_ && vacant_;


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

            // Against the species' own rule rather than against soil by name, so
            // there is exactly one statement in the world of what a plant will
            // root in and the scatter and the hand cannot drift apart. A seed with
            // no tree behind it — nothing has one yet — is refused rather than
            // allowed anywhere, which is the safe way round.
            const std::optional<flora::Species> seed = flora::SpeciesOf(inventory.Held().AsItem());

            // The world as built, not the generator's cover. This is the one place
            // that distinction goes the other way from flora::Grow: a player who
            // has carried soil into a desert and laid a bed of it has made ground a
            // tree can root in, and the noise underneath knows nothing about it.
            if (seed.has_value()) rooted_ = flora::RootsIn(flora::Def(*seed), world.OccupantAt({target.x, under}));
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

    if (!reachable_) {
        LetGo();

        return nullptr;
    }

    // The left hand digs only where this press was a dig. Where it was a chop the
    // swing belongs to the caller, and this must keep its hands off the ground —
    // otherwise a player felling a tree quietly excavates the hillside behind it.
    const bool digging = left_ == Hand::Dig;
    const bool placing = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);

    if (!digging && !placing) {
        LetGo();

        return nullptr;
    }

    // Thrown away from the player rather than towards them, the same way the wood
    // off a felled tree goes.
    const float away = (target.x < player.x) ? -1.0f : 1.0f;

    if (digging) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        if (!Block(x0, y0, x1, y1)) return nullptr;

        // A fixture standing in the way comes off first, and the stroke stops
        // there. It is in front of the ground in every sense — drawn over it, and
        // the only thing in the cell a player pointing at a lit wall can mean — so
        // taking it and the wall behind it in one click would make a torch
        // impossible to move.
        {
            fixture::Kind what = fixture::Kind::Torch;

            if (fixtures.Remove(cellX_, cellY_, what)) {
                // No resistance and no bar. A torch is hung on a surface rather
                // than being part of one, and Minecraft takes one instantly for the
                // same reason.
                LetGo();

                const Rectangle where = World::CellBounds(cellX_, cellY_);

                // On the ground like everything else taken off the world — see
                // Bank. A torch is the one of these that is an item rather than a
                // material, and there is no reason for it to be the one thing that
                // teleports into the bag.
                grove.Fallen().Scatter(ItemsOf(Item::Torch, 1),
                                       {where.x + where.width * 0.5f, where.y + where.height * 0.5f}, away, now);

                return nullptr;
            }
        }

        // What the block holds, and what steady work against it costs.
        //
        // Asked of the world over the same vertices the spade would take, and not
        // sampled at each cell's middle — see World::CellHolds. The middle is the
        // right place to ask what a cell *is*, and the wrong place to ask whether
        // there is anything in it: at a contour edge the middle is open sky while a
        // corner still holds ground, and that one vertex is invisible, solid to a
        // body, and — if the hand believed the middle — impossible to dig back out.
        //
        // Counted per vertex, so the time is the time to clear what is actually
        // there. A cell standing in a hillside is nine vertices of it and a cell at
        // its edge is two, and the second costs two ninths of the first, which is
        // both fair and what makes a ragged edge come away quickly instead of at the
        // price of solid rock.
        World::Yield holds{};

        for (int cx = x0; cx <= x1; cx++) {
            for (int cy = y0; cy <= y1; cy++) world.CellHolds(cx, cy, holds);
        }

        float takes = 0.0f;
        int filled  = 0;

        for (std::size_t e = 0; e < kElementCount; e++) {
            if (holds[e] <= 0) continue;

            takes += BreakSeconds(static_cast<Element>(e), ToolInHand()) * holds[e] / kVerticesPerBlock;
            filled += holds[e];
        }

        // Aimed at open sky. Not a bite that fails, a bite that never began — the bar
        // must not appear over nothing.
        if (filled == 0) {
            LetGo();

            return nullptr;
        }

        // A different block, or the same one holding something else than when the
        // work started. Either way this is not what was being broken, and the work
        // goes back rather than transferring to whatever is under the cursor now —
        // which is the mechanic: a thing has to be worked *at*.
        const bool same = bite_.takes > 0.0f && bite_.cx == x0 && bite_.cy == y0 && bite_.was == holds;

        if (!same) {
            LetGo();

            bite_.cx   = x0;
            bite_.cy   = y0;
            bite_.w    = x1 - x0 + 1;
            bite_.h    = y1 - y0 + 1;
            bite_.was  = holds;
            bite_.done = 0.0f;
        }

        bite_.takes = takes;

        // The frame clock, not the world's: how long a rock takes to break is a rule
        // of the game and must not run at forty times the rate under F7.
        bite_.done += GetFrameTime();

        // Zero hardness comes away on the first frame, which is what a liquid should
        // do and what any row written at zero is asking for.
        if (bite_.done < bite_.takes) return nullptr;

        World::Yield freed{};

        for (int cx = x0; cx <= x1; cx++) {
            for (int cy = y0; cy <= y1; cy++) {
                const World::Stroke out = world.ExcavateCell(cx, cy);

                for (std::size_t e = 0; e < kElementCount; e++) freed[e] += out.freed[e];
            }
        }

        const Rectangle at = World::CellBounds(x0, y0);

        Bank(freed, grove.Fallen(), {at.x + at.width * 0.5f, at.y + at.height * 0.5f}, away, now);

        // Broken, so there is nothing to give back and no bar to fade: cleared
        // outright rather than through LetGo. The next frame starts a fresh bite
        // against whatever is behind it.
        bite_ = {};

        return nullptr;
    }

    const Stack &held = inventory.Held();

    // Ground and pieces go down the same way now that the brush is square: whole
    // cells, one block of material each. What still separates them is the rule
    // about where, which Spend asks about.
    //
    // Held rather than pressed. A brush stroke is a continuous thing, and so is
    // dragging a wall out along a ledge, which is how walls are built in the game
    // this is modelled on. What stops a held button draining the stack is not a
    // press test but the cell itself: a cell that already holds the material gains
    // nothing, is charged nothing, and a cursor resting on one spends nothing
    // however long the button is down.
    if (held.holds == Holds::Material) return Spend(world, inventory, grove.Fallen(), body, away, now);

    // Everything past here answers the press rather than the hold. A brush lays
    // material for as long as the button is down because a stroke is a continuous
    // thing; a sapling is one sapling, and holding the button over a wood would
    // otherwise plant the whole stack in a second.
    if (!IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return nullptr;

    if (held.Empty()) return "nothing in hand";

    // A fixture goes into the cell the cursor is over, on whatever surface that
    // cell has — which surfaces those are is fixture::Def's to say and not this
    // module's, exactly as what a sapling roots in belongs to flora.
    if (Def(held.AsItem()).placement == Placement::Fixture) {
        if (!onCell_) return nullptr;

        const std::optional<fixture::Kind> kind = fixture::KindOf(held.AsItem());
        if (!kind.has_value()) return "that is not something to put down";

        if (fixtures.At(cellX_, cellY_).has_value()) return "there is already something here";
        if (!fixture::Fixtures::Holds(world, *kind, cellX_, cellY_)) return "nothing here to fix it to";

        if (!fixtures.Place(*kind, cellX_, cellY_)) return "there is already something here";

        inventory.Take(inventory.Selected(), 1);

        return nullptr;
    }

    // Everything that goes into the world whole rather than by the fistful goes
    // in the same way: on the ground the cursor found, which is the ground the
    // ghost has been standing on since the hand came near it.
    if (Def(held.AsItem()).placement == Placement::Plant) {
        if (!footing_.has_value()) return "nothing to plant it on";
        if (!rooted_) return "this will not root in that ground";

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

namespace {

// Two colours mixed, for the bar's ramp. Local because it is three lines and the
// one other Mix in this codebase is a static in weather.cpp.
Color Mix(Color a, Color b, float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);

    return {static_cast<unsigned char>(a.r + (b.r - a.r) * s),
            static_cast<unsigned char>(a.g + (b.g - a.g) * s),
            static_cast<unsigned char>(a.b + (b.b - a.b) * s), a.a};
}

// The block's health, over the block.
//
// Minecraft cracks the block itself, which cannot be done here: a cell is drawn from
// a contour shared with its neighbours and out of a chunk-wide texture, so there is
// no per-cell picture to crack and putting one there would cost the whole cache. A
// bar over it says the same thing and says it in one place the eye is already
// looking, since the outline of the block is drawn there too.
//
// Sized in screen pixels over the zoom, like every other mark the cursor makes: a
// world-sized bar doubles with the view and is a slab at full zoom.
void DrawHealth(const Editor::Progress &work, float zoom) {
    if (!work.showing) return;

    constexpr float kHeight = 5.0f;
    constexpr float kLift   = 7.0f;

    const float high = kHeight / zoom;
    const float over = kLift / zoom;
    const float edge = 1.0f / zoom;

    const Rectangle bar = {work.over.x, work.over.y - over - high, work.over.width, high};

    const auto fade = [&](Color colour) {
        return Color{colour.r, colour.g, colour.b,
                     static_cast<unsigned char>(colour.a * std::clamp(work.ink, 0.0f, 1.0f))};
    };

    // A ground of its own under the fill, so the bar reads against a hillside and
    // against open sky alike. Without it the empty end of the bar is whatever
    // happens to be behind it and the length stops being legible.
    DrawRectangleRec(bar, fade({16, 18, 24, 190}));

    const float left = std::clamp(work.health, 0.0f, 1.0f);

    // Green through amber to red as it goes, which is the one thing about a health
    // bar nobody has to be taught.
    const Color full = {96, 210, 108, 235};
    const Color half = {232, 184, 72, 235};
    const Color gone = {224, 88, 72, 235};

    const Color tone = (left > 0.5f) ? Mix(half, full, (left - 0.5f) * 2.0f) : Mix(gone, half, left * 2.0f);

    if (left > 0.0f) {
        DrawRectangleRec({bar.x + edge, bar.y + edge, (bar.width - edge * 2.0f) * left, bar.height - edge * 2.0f},
                         fade(tone));
    }

    DrawRectangleLinesEx(bar, edge, fade({12, 14, 18, 220}));
}

} // namespace

void Editor::DrawCursor(const Inventory &inventory, const Grove &grove, flora::Season season,
                        const Camera2D &camera) const {
    const Vector2 mouse = GetMousePosition();
    if (hotbar::Contains(mouse)) return;

    // Before every early return below it, and outside the reach test. The bar is
    // about a block that is *being* worked on, and the paths that give up on drawing
    // a cursor — a hand over the axe, a cursor out of range — are exactly the paths
    // where the work has just been let go of and the giving-back is what there is to
    // show.
    DrawHealth(Biting(), std::max(camera.zoom, 0.01f));

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    const Stack &carried = inventory.Held();

    // Both figures are in screen pixels and both are divided by the zoom, so a
    // mark keeps its size and its weight however far the view is pushed in. A
    // world-sized icon doubles with the zoom and a world-sized stroke doubles
    // with it again, which is how a cursor ends up a blot.
    const float zoom  = std::max(camera.zoom, 0.01f);
    const float thick = kIconStroke / zoom;
    const float scale = kIconSize * 0.5f / zoom;

    // Over wood, the left hand is an axe and the mark says so.
    //
    // Before the ghost and not after it, which is the fix. A player with a sapling
    // in hand who points at a trunk is about to chop it — that is what the left
    // button will do, decided by `timber_` and nothing else — and the cursor was
    // answering with a picture of the sapling standing at the foot of the tree,
    // which is a promise about the *other* button and the one that is not going to
    // be pressed. One cursor and two hands: it has to show whichever of them the
    // click will be, and on a trunk that is the axe.
    //
    // A material in hand is the one exception, and it is the same exception it
    // always was: a brush lays ground for as long as the button is held, the ring
    // is how wide the stroke is, and building up against a tree is a thing people
    // do. The ring stays.
    if (timber_ && carried.holds != Holds::Material) {
        DrawAxe(target, scale, thick, kChopColor);
        return;
    }

    // The grid replaces the ring outright, and has to: the ring's whole meaning is
    // "this much comes away under the brush", and a hand that works one square at
    // a time covers no such area. Drawing both would be two different claims about
    // where the next click lands.
    if (building_) {
        DrawGrid(carried, zoom);
        return;
    }

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

    // Nothing in the block to take apart, so nothing is claimed. The mark says what
    // the next click will do, and over open air the honest answer is that it does
    // nothing — see holding_.
    if (!holding_) return;

    // The block carries what the hands can do here, exactly as the ring used to.
    // Out of reach it goes grey and says neither of them can; in reach it takes
    // the colour of whatever the right hand would put down, or the digging colour
    // where there is nothing to put down and only the left hand is any use.
    //
    // Reading that off the cursor is what keeps two buttons workable without a
    // badge somewhere else saying which is which, since the eye is already here.
    const Stack &held = inventory.Held();

    Color color = kFarColor;

    if (Reachable()) {
        color = (held.holds == Holds::Material) ? StyleOf(held.AsElement()).contour : kDigColor;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!Block(x0, y0, x1, y1)) return;

    const Rectangle from = World::CellBounds(x0, y0);
    const Rectangle to   = World::CellBounds(x1, y1);

    const Rectangle at = {from.x, from.y, to.x + to.width - from.x, to.y + to.height - from.y};

    // Two lines, the darker one a screen pixel outside — see kEdgeColor. Both in
    // screen pixels over the zoom, like every other mark the cursor makes, so the
    // outline is the same weight however far the view is pushed in.
    const float hair = 1.0f / zoom;

    DrawRectangleLinesEx({at.x - hair, at.y - hair, at.width + hair * 2.0f, at.height + hair * 2.0f}, hair,
                         kEdgeColor);

    DrawRectangleLinesEx(at, hair, color);

    // The block says how much comes away and the spade says what the hand is; the
    // two are different questions and the outline cannot answer the second, which
    // is why the mark in the middle is a tool and not a cross.
    DrawSpade({at.x + at.width * 0.5f, at.y + at.height * 0.5f}, scale, thick, color);
}

void Editor::DrawGrid(const Stack &held, float zoom) const {
    const auto side = static_cast<float>(config::kBuildCell);

    // Ruled as whole lines across the reach rather than as a rectangle per cell.
    // The reach is six cells, so the square around it is thirteen by thirteen —
    // a hundred and sixty-nine outlines, six hundred and seventy-six quads, every
    // frame the hand holds a plank. Twenty-six lines draw the same grid. It is the
    // same lesson the pickups taught: what costs here is submission, not pixels.
    const int reach = static_cast<int>(std::ceil(kReach / side));

    int cx = 0;
    int cy = 0;
    World::ToCell(from_, cx, cy);

    // Off the cell's own bounds rather than off a multiple of the side, so the
    // rule falls exactly where the edge of a piece will — see kCellOffset. Ruling
    // the grid on one grid and building on another is the bug this whole routine
    // is here to make impossible to have again.
    const Rectangle first = World::CellBounds(cx - reach, cy - reach);
    const Rectangle last  = World::CellBounds(cx + reach, cy + reach);

    const float x0 = first.x;
    const float y0 = first.y;
    const float x1 = last.x + last.width;
    const float y1 = last.y + last.height;

    // Faint, and it has to be. This is drawn over the whole of the ground the
    // player is looking at, every frame, for as long as they are building — at any
    // weight that reads as a fence in front of the world rather than as a
    // guide laid over it.
    const Color rule = Fade(kFarColor, 0.22f);
    const float hair = 1.0f / zoom;

    for (int i = 0; i <= reach * 2 + 1; i++) {
        const float x = x0 + static_cast<float>(i) * side;
        DrawLineEx({x, y0}, {x, y1}, hair, rule);
    }

    for (int j = 0; j <= reach * 2 + 1; j++) {
        const float y = y0 + static_cast<float>(j) * side;
        DrawLineEx({x0, y}, {x1, y}, hair, rule);
    }

    int bx0 = 0;
    int by0 = 0;
    int bx1 = 0;
    int by1 = 0;
    if (!Block(bx0, by0, bx1, by1)) return;

    // The block the click covers, in the material's own colour so the square reads
    // as the thing that is about to be there — and outlined in the answer, green
    // where it will go and red where it will not. The refusal arrives before the
    // click rather than as a line of text after it.
    const Rectangle from = World::CellBounds(bx0, by0);
    const Rectangle to   = World::CellBounds(bx1, by1);

    const Color says = buildable_ ? kPlantColor : (Reachable() ? kDigColor : kFarColor);

    if (buildable_ && held.holds == Holds::Material) {
        const Color fill = Fade(StyleOf(held.AsElement()).contour, 0.45f);

        // Cell by cell rather than as one rectangle, so the grid stays legible
        // under a wide brush and the block reads as the pieces it will become.
        for (int cx = bx0; cx <= bx1; cx++) {
            for (int cy = by0; cy <= by1; cy++) DrawRectangleRec(World::CellBounds(cx, cy), fill);
        }
    }

    const Rectangle at = {from.x, from.y, to.x + to.width - from.x, to.y + to.height - from.y};

    DrawRectangleLinesEx(at, 1.0f / zoom, says);
}
