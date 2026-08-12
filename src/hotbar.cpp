#include "hotbar.h"

#include "config.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSlots = static_cast<int>(kElementCount);

// How many slots the number row can reach: one through nine, then zero for the
// tenth, which is how the keys are laid out rather than how they are numbered.
// Anything past that is reached by the wheel or by clicking the slot.
constexpr int kKeyedSlots = 10;

} // namespace

float Hotbar::SlotSize() const {
    const float room = static_cast<float>(GetScreenWidth()) - 2.0f * kMargin - (kSlots + 1) * kPadding;

    return std::min(kSlotSize, std::max(room / kSlots, kMinSlotSize));
}

Rectangle Hotbar::BarBounds() const {
    const float slot = SlotSize();

    const float width  = kSlots * slot + (kSlots + 1) * Hotbar::kPadding;
    const float height = slot + 2.0f * Hotbar::kPadding;

    // Centred on the frame as it is now, so the bar stays under the middle of a
    // window that has been resized.
    return {(GetScreenWidth() - width) / 2.0f, GetScreenHeight() - height - Hotbar::kMargin, width, height};
}

Rectangle Hotbar::SlotBounds(int slot) const {
    const Rectangle bar = BarBounds();
    const float side    = SlotSize();

    return {bar.x + kPadding + slot * (side + kPadding), bar.y + kPadding, side, side};
}

void Hotbar::Update() {
    for (int slot = 0; slot < kSlots && slot < kKeyedSlots; slot++) {
        const int key = (slot == kKeyedSlots - 1) ? KEY_ZERO : (KEY_ONE + slot);

        if (IsKeyPressed(key)) selected_ = slot;
    }

    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        // Wrapped rather than clamped, so scrolling past either end continues
        // into the other, the way an inventory bar is expected to behave.
        selected_ = (selected_ - static_cast<int>(wheel) % kSlots + kSlots) % kSlots;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const Vector2 mouse = GetMousePosition();

        for (int slot = 0; slot < kSlots; slot++) {
            if (CheckCollisionPointRec(mouse, SlotBounds(slot))) selected_ = slot;
        }
    }
}

bool Hotbar::Contains(Vector2 screen) const {
    return CheckCollisionPointRec(screen, BarBounds());
}

void Hotbar::Draw(const std::array<int, kElementCount> &collected) const {
    const Rectangle bar = BarBounds();

    // Opaque, because the world keeps scrolling behind the bar and a
    // translucent panel makes that movement read as the bar itself glitching.
    DrawRectangleRec(bar, {30, 34, 42, 255});

    for (int slot = 0; slot < kSlots; slot++) {
        const Rectangle bounds  = SlotBounds(slot);
        const ElementDef &style = kElements[slot];
        const bool active       = (slot == selected_);

        DrawRectangleRec(bounds, {60, 66, 78, 255});

        // Swatch showing what the element looks like in the world: its own
        // interior colour where it has one, its outline colour otherwise, so a
        // material drawn only as a contour is still recognisable here.
        const Rectangle swatch = {bounds.x + 8.0f, bounds.y + 8.0f, bounds.width - 16.0f, bounds.height - 24.0f};

        // Backed with the world's background colour first, so a translucent
        // element shows the same shade here as it does out in the world.
        DrawRectangleRec(swatch, RAYWHITE);
        DrawRectangleRec(swatch, (style.fill.a > 0) ? style.fill : style.contour);
        DrawRectangleLinesEx(swatch, 1.0f, style.contour);

        // The key that selects it, not its position in the row. The last slots
        // have no key at all, and printing an eleven on one would be an
        // instruction that does nothing.
        if (slot < kKeyedSlots) {
            DrawText(TextFormat("%d", (slot + 1) % 10), static_cast<int>(bounds.x + 4.0f),
                     static_cast<int>(bounds.y + 2.0f), 10, RAYWHITE);
        }

        // Only once there is something to count. A row of zeroes says nothing
        // and competes with the swatch for the eye.
        if (collected[slot] > 0) {
            const char *amount = TextFormat("%d", collected[slot]);
            const int width    = MeasureText(amount, 10);

            DrawText(amount, static_cast<int>(bounds.x + bounds.width - width - 4.0f),
                     static_cast<int>(bounds.y + 2.0f), 10, {255, 214, 110, 255});
        }

        const int nameWidth = MeasureText(style.name, 10);
        DrawText(style.name, static_cast<int>(bounds.x + (bounds.width - nameWidth) / 2.0f),
                 static_cast<int>(bounds.y + bounds.height - 12.0f), 10, RAYWHITE);

        DrawRectangleLinesEx(bounds, active ? 3.0f : 1.0f, active ? RAYWHITE : Fade(RAYWHITE, 0.4f));
    }
}
