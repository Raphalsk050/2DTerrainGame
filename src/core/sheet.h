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

void Unload(Strip &strip);

} // namespace sheet
