#include "hotbar.h"

#include "config.h"

#include <cmath>

namespace {

constexpr int kSlots = static_cast<int>(kElementCount);

} // namespace

Rectangle Hotbar::BarBounds() const {
    const float width  = kSlots * Hotbar::kSlotSize + (kSlots + 1) * Hotbar::kPadding;
    const float height = Hotbar::kSlotSize + 2.0f * Hotbar::kPadding;

    return {(config::kScreenWidth - width) / 2.0f, config::kScreenHeight - height - Hotbar::kMargin, width, height};
}

Rectangle Hotbar::SlotBounds(int slot) const {
    const Rectangle bar = BarBounds();

    return {bar.x + kPadding + slot * (kSlotSize + kPadding), bar.y + kPadding, kSlotSize, kSlotSize};
}

void Hotbar::Update() {
    for (int slot = 0; slot < kSlots; slot++) {
        if (IsKeyPressed(KEY_ONE + slot)) selected_ = slot;
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

void Hotbar::Draw() const {
    const Rectangle bar = BarBounds();

    // Opaque, because the world keeps scrolling behind the bar and a
    // translucent panel makes that movement read as the bar itself glitching.
    DrawRectangleRec(bar, {30, 34, 42, 255});

    for (int slot = 0; slot < kSlots; slot++) {
        const Rectangle bounds    = SlotBounds(slot);
        const ElementStyle &style = kElementStyles[slot];
        const bool active         = (slot == selected_);

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

        DrawText(TextFormat("%d", slot + 1), static_cast<int>(bounds.x + 4.0f), static_cast<int>(bounds.y + 2.0f), 10,
                 RAYWHITE);

        const int nameWidth = MeasureText(style.name, 10);
        DrawText(style.name, static_cast<int>(bounds.x + (bounds.width - nameWidth) / 2.0f),
                 static_cast<int>(bounds.y + bounds.height - 12.0f), 10, RAYWHITE);

        DrawRectangleLinesEx(bounds, active ? 3.0f : 1.0f, active ? RAYWHITE : Fade(RAYWHITE, 0.4f));
    }
}
