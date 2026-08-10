#include "editor.h"

#include <algorithm>

namespace {

// Colour of the cursor while digging. Distinct from every material in the
// table, so the ring never reads as a preview of what is about to be placed.
constexpr Color kDigColor = {235, 84, 84, 255};

} // namespace

void Editor::Update(World &world, const Hotbar &hotbar, const Camera2D &camera) {
    if (IsKeyPressed(KEY_X)) mode_ = (mode_ == Mode::Place) ? Mode::Dig : Mode::Place;

    // Sized with keys rather than the wheel, which already steps through the
    // hotbar. Two meanings on one control is exactly the kind of thing the mode
    // switch is here to avoid.
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
    if (hotbar.Contains(mouse)) {
        under_.reset();
        return;
    }

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    under_ = world.OccupantAt(target);

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) return;

    if (mode_ == Mode::Place) {
        world.Place(hotbar.Selected(), target, radius_);
        return;
    }

    const World::Yield yield = world.Excavate(target, radius_);
    for (std::size_t e = 0; e < kElementCount; e++) {
        collected_[e] += yield[e];
    }
}

void Editor::DrawCursor(const Hotbar &hotbar, const Camera2D &camera) const {
    const Vector2 mouse = GetMousePosition();
    if (hotbar.Contains(mouse)) return;

    const Vector2 target = GetScreenToWorld2D(mouse, camera);

    // The ring carries the mode: the material's own colour where it is about to
    // be placed, one colour of its own where the brush removes instead. Reading
    // the mode off the cursor is what makes a modal brush workable, since the
    // eye is already there.
    const ElementDef &style = StyleOf(hotbar.Selected());
    const Color color       = (CurrentMode() == Mode::Place) ? style.contour : kDigColor;

    DrawCircleLinesV(target, radius_, color);
    DrawCircleLinesV(target, radius_ - 1.0f, Fade(color, 0.5f));

    // A cross rather than a filled disc, so the brush never hides the contour
    // it is aimed at.
    DrawLineV({target.x - 4.0f, target.y}, {target.x + 4.0f, target.y}, color);
    DrawLineV({target.x, target.y - 4.0f}, {target.x, target.y + 4.0f}, color);
}
