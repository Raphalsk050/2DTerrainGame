#include "editor.h"

#include "hotbar.h"

#include <algorithm>
#include <cmath>

namespace {

// Colour of the cursor over ground the left hand can take apart. Distinct from
// every material in the table, so the ring never reads as a preview of what is
// about to be placed.
constexpr Color kDigColor = {235, 84, 84, 255};

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

    const Vector2 mouse = GetMousePosition();

    // The bar sits over the world it edits, so a click that lands on it belongs
    // to the bar alone. The cursor is dropped as well, otherwise it would hang
    // over the slots as if they were something to dig.
    if (hotbar::Contains(mouse)) {
        under_.reset();
        reachable_ = false;
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

    if (!reachable_) return nullptr;

    const bool digging = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
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

    Color color = kFarColor;

    if (Reachable()) {
        color = (held.holds == Holds::Material) ? StyleOf(held.AsElement()).contour : kDigColor;
    }

    DrawCircleLinesV(target, radius_, color);
    DrawCircleLinesV(target, radius_ - 1.0f, Fade(color, 0.5f));

    // A cross rather than a filled disc, so the brush never hides the contour
    // it is aimed at.
    DrawLineV({target.x - 4.0f, target.y}, {target.x + 4.0f, target.y}, color);
    DrawLineV({target.x, target.y - 4.0f}, {target.x, target.y + 4.0f}, color);
}
