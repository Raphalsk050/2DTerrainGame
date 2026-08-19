#include "ui/hotbar.h"

#include "core/picture.h"
#include "ui/bottom.h"

#include <cmath>

namespace {

constexpr int kSlots = Inventory::kOnHand;

// Colours of the bar, its slots and the ring round the one in hand.
constexpr Color kBar     = {30, 34, 42, 255};
constexpr Color kSlot    = {60, 66, 78, 255};
constexpr Color kCount   = {255, 214, 110, 255};
constexpr Color kKeycap  = {150, 158, 172, 255};
constexpr Color kOutline = {255, 255, 255, 255};

} // namespace

Rectangle hotbar::Bounds() {
    const float width  = kSlots * kSlotSide + (kSlots + 1) * kPadding;
    const float height = kSlotSide + 2.0f * kPadding;

    // Centred on the frame as it is now, so the bar stays under the middle of a
    // window that has been resized. Floored, because a frame of odd width would
    // otherwise put the whole bar on a half pixel and blur every picture in it.
    return {std::floor((GetScreenWidth() - width) / 2.0f), std::floor(GetScreenHeight() - height - kMargin), width,
            height};
}

Rectangle hotbar::SlotBounds(int slot) {
    const Rectangle bar = Bounds();

    return {bar.x + kPadding + slot * (kSlotSide + kPadding), bar.y + kPadding, kSlotSide, kSlotSide};
}

bool hotbar::Contains(Vector2 screen) {
    return CheckCollisionPointRec(screen, Bounds());
}

void hotbar::Update(Inventory &inventory, bool wheelTaken) {
    // One through nine, which is exactly the bar now that it is nine slots
    // wide. The palette needed a tenth key for a tenth material and a rule about
    // what the ones past that did; there is nothing past this.
    for (int slot = 0; slot < kSlots; slot++) {
        if (IsKeyPressed(KEY_ONE + slot)) inventory.Select(slot);
    }

    const float wheel = wheelTaken ? 0.0f : GetMouseWheelMove();
    if (wheel != 0.0f) inventory.Select(inventory.Selected() - static_cast<int>(wheel));

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const Vector2 mouse = GetMousePosition();

        for (int slot = 0; slot < kSlots; slot++) {
            if (CheckCollisionPointRec(mouse, SlotBounds(slot))) inventory.Select(slot);
        }
    }
}

void hotbar::DrawSlot(const Stack &stack, Rectangle bounds, bool active) {
    DrawRectangleRec(bounds, kSlot);

    if (!stack.Empty()) {
        const float drawn = kIconPixel * kPictureSide;

        const Picture picture = PictureOf(stack);

        DrawPicture(picture,
                    {std::floor(bounds.x + (bounds.width - drawn) / 2.0f),
                     std::floor(bounds.y + (bounds.height - drawn) / 2.0f)},
                    kIconPixel);

        // Only past one. A count on every slot turns the bar into a spreadsheet,
        // and a lone item is already drawn as one thing.
        if (stack.count > 1) {
            const char *amount = TextFormat("%d", stack.count);
            const int width    = MeasureText(amount, 10);

            const int x = static_cast<int>(bounds.x + bounds.width - width - 4.0f);
            const int y = static_cast<int>(bounds.y + bounds.height - 12.0f);

            // Shadowed rather than plain. The count sits over the picture, and
            // over a pale material — sand, snow — white on white is not a
            // number at all.
            DrawText(amount, x + 1, y + 1, 10, {12, 14, 18, 220});
            DrawText(amount, x, y, 10, kCount);
        }
    }

    DrawRectangleLinesEx(bounds, active ? 3.0f : 1.0f, active ? kOutline : Fade(kOutline, 0.4f));
}

void hotbar::Draw(const Inventory &inventory) {
    const Rectangle bar = Bounds();

    // Opaque, because the world keeps scrolling behind the bar and a translucent
    // panel makes that movement read as the bar itself glitching.
    DrawRectangleRec(bar, kBar);

    for (int slot = 0; slot < kSlots; slot++) {
        const Rectangle bounds = SlotBounds(slot);

        DrawSlot(inventory.At(slot), bounds, slot == inventory.Selected());

        // Shadowed like the count, and for the same reason: the key sits over
        // the picture, and over sand or snow or gold a grey number on a pale
        // face is not a number.
        const char *key = TextFormat("%d", slot + 1);

        const int x = static_cast<int>(bounds.x + 4.0f);
        const int y = static_cast<int>(bounds.y + 2.0f);

        DrawText(key, x + 1, y + 1, 10, {12, 14, 18, 220});
        DrawText(key, x, y, 10, kKeycap);
    }

    // The name of what is in hand, on its own row above the strip.
    //
    // The palette printed a name on every slot, which it could because a slot was
    // always the same material. A slot now holds whatever was put in it, and nine names
    // under nine pictures is a wall of text under the one thing the eye actually reads.
    // So it is said once, for the one that matters, and only when there is something to
    // say.
    //
    // Where it goes is `bottom::Of`'s to decide and no longer a constant here. It was
    // twenty pixels over the bar, which is where the brush badge and the health bar
    // also were — three files each certain they had that space to themselves.
    //
    // Minecraft fades this after a couple of seconds and this does not, deliberately:
    // there it is a reminder while you scroll, and the bar is otherwise stable. Here a
    // slot can hold a material, an item or a fixture and they are not all obvious from
    // the picture, so the name earns its line permanently.
    const Stack &held = inventory.Held();
    if (held.Empty()) return;

    const Rectangle where = bottom::Of().name;

    const int width = MeasureText(held.Name(), 14);
    const int x     = static_cast<int>(where.x + (where.width - width) / 2.0f);
    const int y     = static_cast<int>(where.y + (where.height - 14.0f) / 2.0f);

    DrawText(held.Name(), x + 1, y + 1, 14, {12, 14, 18, 200});
    DrawText(held.Name(), x, y, 14, kOutline);
}
