#pragma once

#include "core/picture.h"
#include "core/stack.h"
#include "raylib.h"

// What one stack looks like, wherever it is drawn.
//
// There were four callers drawing a stack — the bar, the panel, the cursor carrying
// one, the recipe card, and a pickup lying in the grass — and every one of them
// reached for `DrawPicture(PictureOf(stack), ...)` directly. That was right while a
// stack had exactly one way of being drawn. It is not any more: a row may now carry an
// authored file instead of a hand-drawn `Picture`, and five call sites each deciding
// which is five places for a new material to be drawn one way in the bar and another
// on the ground. §25.1's complaint about the foot of the screen, one layer in.
//
// So there is one answer to "draw this stack", and it takes the same two arguments
// `DrawPicture` did — the top-left corner and the width of a texel — because the
// callers anchor against different grids and only they know which. A pickup is
// anchored to the world so it does not crawl as the view scrolls; a slot is anchored
// to the frame and moves with it.
//
// ---
//
// **Both kinds fill the same box.** An authored file is 64x64 and a `Picture` is six
// texels, and neither number reaches a caller: what is drawn is always
// `kPictureSide * pixel` on a side. That is what makes the art a drop-in for the
// drawing it replaces — a slot does not change size because somebody drew a pickaxe,
// and fifteen tools drawn on one canvas keep their proportions against each other
// rather than each being blown up to its own bounding box. It is §24.1's argument
// about one window per creature, met from the other side.
//
// **And the file is never resampled to get there.** The asset stays at the size it
// was drawn; only the scale it is *drawn at* moves. What keeps that from turning pixel
// art into a smear is a mipmap chain and trilinear sampling, which is the one place in
// this project that is not point sampled — deliberately, and see `Load` for why.
namespace icon {

// The authored texture for one row, loaded on the first ask and kept, or nothing
// where the row is drawn from its `Picture`.
//
// Needs a window, so it cannot be called before one is open. That is why the load
// happens at draw time rather than at startup, where `content::Open` runs and there is
// no graphics device yet — `mob::Dressed`'s reasoning exactly.
const Texture2D *Art(const ItemDef &def);

// The stack's picture, authored or hand-drawn, filling `kPictureSide * pixel` square
// with its top-left corner at `at`.
void Draw(const Stack &stack, Vector2 at, float pixel);

// How much of the tool is left, as a bar across the foot of that same square.
//
// Nothing at all for anything that does not wear out, which is nearly everything the
// player carries.
//
// **Drawn even at full**, where Minecraft hides it until the tool is damaged. The
// difference is what the two are for: there the bar is a warning, and here it is also
// the answer to "is this thing something that runs out", which a player holding their
// first pickaxe has no other way to find out. It is §25.2's lesson about the hearts —
// a readout that appears for the first time during the emergency is one the player has
// to find while it matters.
void DrawWear(const Stack &stack, Vector2 at, float pixel);

// Gives back every texture. Called beside `mob::Undress` on the way out, and for the
// same reason: a texture outliving the window it was made in is a crash on exit that
// only ever happens on somebody else's machine.
void Discard();

} // namespace icon
