#pragma once

#include "raylib.h"

// A strip of animation frames, authored rather than generated.
//
// The first art in this project that came out of a file. Everything else is drawn from
// a table — `Picture` for an icon, `figure::Figure` for a creature, `canopy` for a
// tree — and that was a deliberate choice each time: procedural paint has no asset to
// keep in step and no size to get wrong.
//
// It is also the case CLAUDE.md §12 explicitly leaves the door open for. Every material
// in the world is drawn at `config::kPixelSize` because varying it opened a pale rind
// down the side of every block (§12.1), and that argument is about *materials sharing a
// contour and a union*. A sprite drawn over the ground takes part in neither. §12 ends
// by saying a finer texel is "worth it for authored art; not worth it for procedural
// paint", and this is the authored art.
//
// So a strip's pixels are world pixels, one for one. A 28-wide boar is 28 world pixels
// across, which is the same size the six-texel drawing it replaces came out at — see
// `mob::Def::art`.
namespace sheet {

struct Strip {
    Texture2D texture{};

    // How many frames the strip holds, and how big one is. Read from the image: the
    // frames are square-packed left to right, so the count is the width over the
    // height of a frame — which is why the frame size has to be given rather than
    // guessed.
    int frames = 0;
    int wide   = 0;
    int tall   = 0;

    // The part of a frame that is actually drawn on, in texels, measured from the
    // frame's own top left.
    //
    // Worked out at load rather than authored, and it is the **union over every frame**
    // rather than each frame's own — so the frames share one window and the drawing does
    // not jump between them. That is §24.1's rule about one window per creature, met from
    // the other side: there the window was chosen by hand and written into the cutter;
    // here the file may be a canvas with the thing sitting somewhere in it, and the
    // window is what the artist drew inside it.
    //
    // What it buys is that a fixture can be drawn on a 64-square canvas — the same one
    // every tool is drawn on (§29.1) — and still stand on the ground rather than floating
    // above the empty rows under it.
    int solidX    = 0;
    int solidY    = 0;
    int solidWide = 0;
    int solidTall = 0;

    bool Ready() const { return texture.id != 0 && frames > 0; }
};

// Loads a horizontal strip whose frames are `tall` pixels high and as wide as the
// image divided by the frame count.
//
// `frames` is worked out from the image rather than passed in: the cutter writes one
// row of equal cells, so the count is the width over the height. A strip whose width
// is not a whole number of frames is a strip that was not cut by `tools/cut_sprites.py`,
// and it comes back empty rather than drawing a smeared animal.
Strip Load(const char *path, int wide);

// Draws one frame with its bottom-centre at `at`.
//
// The same anchor `figure::Draw` uses, and for the same reason: a body's own position
// is its feet, so a creature drawn from it stands on the ground its collider rests on
// rather than being hung from a corner.
//
// `facing` is +1 right and -1 left, and it mirrors the source rather than needing a
// second strip. `tint` multiplies, which is what the hurt flash is made of.
void Draw(const Strip &strip, int frame, Vector2 at, float pixel, int facing, Color tint);

// The same, but drawing only what is drawn on — see `Strip::solidX`.
//
// Bottom-centre of the *content* rather than of the canvas, so a picture with empty rows
// under it stands on the ground instead of hanging over it. Kept apart from `Draw`
// rather than folded into it, because the two answer different questions: a creature's
// window was cut to the ground line on purpose and its empty margin is part of the
// framing, while a fixture's canvas is a canvas.
//
// No facing. A fixture has none — see `fixture::Def::art`.
void DrawSolid(const Strip &strip, int frame, Vector2 at, float pixel, Color tint);

void Unload(Strip &strip);

} // namespace sheet
