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

    if (held.holds == Holds::Item && held.AsItem() == Item::Sapling) {
        // The species is the one the climate there would have grown anyway.
        // Choosing it for the player rather than offering a menu: what a place
        // will support is a property of the place, and planting a pine in a swamp
        // is not a decision worth surfacing before there is a reason to make it.
        if (!grove.Plant(grove.Suited(target.x), target, now)) {
            return "no room here — something is already growing";
        }

        inventory.Take(inventory.Selected(), 1);

        return nullptr;
    }

    return "that is not something to put down";
}

void Editor::DrawCursor(const Inventory &inventory, const Camera2D &camera) const {
    const Vector2 mouse = GetMousePosition();
    if (hotbar::Contains(mouse)) return;

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

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
