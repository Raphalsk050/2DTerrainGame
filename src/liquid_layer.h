#pragma once

#include "raylib.h"
#include "world.h"

// Off-screen buffer the world's liquids are drawn into before reaching the
// frame.
//
// Liquids are translucent, and drawing them straight to the frame blends every
// piece separately. Wherever two pieces meet, the overlap is blended twice and
// a gap is not blended at all, so a single body of liquid comes out crossed by
// seams that shift as the camera moves by fractions of a pixel. Collecting the
// whole layer opaquely and compositing it once removes the class of artefact
// rather than any particular instance of it.
class LiquidLayer {
public:
    // Sizes the buffer to the frame, reallocating only when the frame has actually
    // changed size. Safe to call every frame, which is what makes it the right shape
    // for a resizable window: nothing has to notice the resize and react to it.
    void Fit(int width, int height);

    void Unload();

    // Draws the liquids of `world` into the buffer, in world space.
    void Capture(const World &world, Rectangle view, const Camera2D &camera);

    // Composites the buffer over the current frame in one blend.
    void Compose(unsigned char alpha) const;

private:
    RenderTexture2D target_ = {};
};
