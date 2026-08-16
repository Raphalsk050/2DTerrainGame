#pragma once

#include "raylib.h"

// The world, drawn into a target of its own so that the light multiplies the
// world and nothing else.
//
// The sky is not something lit — it is where the light comes from. Multiplying
// the solved field over the drawn atmosphere is therefore a category error, and
// it was invisible only for as long as nothing cast a shadow through open air.
// Clouds do. The deck hangs above the band of sky the player can actually see,
// so its shadow fell across the whole backdrop and dimmed it, flickering as the
// player walked from under one cloud to under the next. Under a cloud you see
// the cloud grey and, beside it, ordinary blue; you never see darkened blue.
//
// There was nowhere to put that distinction while the frame was drawn in one
// pass: the atmosphere is the first thing down, so the multiply landed on it
// along with everything else. So the world goes in here, the multiply happens
// inside, and the atmosphere is drawn straight to the frame with this composited
// over it.
//
// The clouds stay *inside* the layer on purpose. They are objects standing in
// the world and should darken at dusk and glow at noon like anything else. Only
// the blue behind them comes out.
class LitLayer {
public:
    // Sizes the target to the frame, reallocating only when the frame has
    // actually changed size — the same shape LiquidLayer and Backdrop use, and
    // for the same reason: nothing has to notice a resize.
    void Fit(int width, int height);

    void Unload();

    // Whether there is a target to draw into. False only where the allocation
    // failed or the frame has no size at all, and the answer to both is to draw
    // no world — the same answer LiquidLayer gives to the same failure, and the
    // window is not on screen in either case.
    bool Ready() const { return target_.id != 0; }

    // The blend everything drawn into the layer has to use. Set by Capture, and
    // public because a pass that needs a blend of its own has to put this back
    // afterwards: EndBlendMode returns to BLEND_ALPHA, which is not it.
    static void Blend();

    // Opens the target and sets that blend. Whatever is drawn between this and
    // Finish is the world.
    //
    // Both run before BeginDrawing, for the reason Backdrop gives: a texture
    // mode cannot be opened once the frame has been.
    void Capture();
    void Finish();

    // The layer over the frame, in one blend.
    void Compose() const;

private:
    RenderTexture2D target_ = {};
};
