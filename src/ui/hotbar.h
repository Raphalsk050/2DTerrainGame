#pragma once

#include "item/inventory.h"
#include "raylib.h"

// The row of slots along the bottom of the screen.
//
// A view of the first nine slots of the inventory and nothing more: it holds no
// stacks, and the slot it points at is the inventory's own. Before there was an
// inventory this was a palette with one slot per material and an endless supply
// behind each, which is a different idea wearing the same shape — what a player
// can place is now only what a player has.
//
// A namespace rather than a class because there is no longer any state to keep.
// Everything it draws it is handed, and where it draws is a function of the
// frame as it is this instant, so that the bar stays under the middle of a
// window that has been resized.
namespace hotbar {

// Side of one slot, and the width of one texel of the picture inside it.
//
// Fixed, where the palette's had to shrink to fit twelve materials into the
// narrowest window the game may be dragged to. Nine slots come to 456 pixels
// against a floor of 640, so there is always room — and that is what lets the
// texel be a whole number of screen pixels always. A picture authored on a
// six-texel grid drawn at a fraction of a pixel comes out with its columns
// alternating two pixels wide and three, which is the one thing that must never
// happen to art of this size.
inline constexpr float kSlotSide  = 44.0f;
inline constexpr float kIconPixel = 6.0f;

inline constexpr float kPadding = 6.0f;
inline constexpr float kMargin  = 12.0f;

// Number keys select a slot directly; the wheel steps through them.
//
// `wheelTaken` says the wheel belongs to somebody else this frame — the view,
// while the zoom modifier is held. Told rather than worked out here, so that the
// two readers of one control cannot come to different conclusions about who has
// it.
void Update(Inventory &inventory, bool wheelTaken);

void Draw(const Inventory &inventory);

// Where the bar and its slots are, this frame.
//
// Public because the inventory panel draws the same nine slots as its own
// bottom row and has to line them up with these; two independent layouts of one
// row is two of them disagreeing by a pixel the first time either changes.
Rectangle Bounds();
Rectangle SlotBounds(int slot);

// Draws one slot's worth of stack — the frame, the picture and the count.
//
// Shared with the panel for the same reason the bounds are: a slot in the grid
// and a slot in the bar are the same thing seen twice, and the moment they are
// drawn by two routines they start to look like two different things.
void DrawSlot(const Stack &stack, Rectangle bounds, bool active);

// True when a screen position lies on the bar. Callers test this before acting
// on a click, so selecting a slot does not also paint the world behind it.
bool Contains(Vector2 screen);

} // namespace hotbar
