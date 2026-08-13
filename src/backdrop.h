#pragma once

#include "raylib.h"

// The world behind an open panel: captured, blurred and darkened.
//
// It exists because there was nowhere to blur *from*. The frame is drawn
// straight to the screen, and a blur has to read the finished image back — so
// the scene is rendered into a target of its own first, which is the same shape
// LiquidLayer already uses and for the same reason.
//
// Two targets and two passes. A Gaussian is separable, so blurring across into
// the second target and then down out of it costs eighteen samples a pixel where
// the square kernel would cost eighty-one, and the second pass writes straight
// to the screen rather than to a third target.
class Backdrop {
public:
    // Loads the shader. Called once a window exists.
    void Create();

    // Sizes the targets to the frame. Reallocates only when the size changes, so
    // it is safe to call every frame and nothing has to notice a resize.
    void Fit(int width, int height);

    void Unload();

    // Opens the capture target. Whatever is drawn between this and Finish is
    // what gets blurred.
    //
    // Both have to run before BeginDrawing: a texture mode cannot be opened
    // inside a frame, which is the same constraint that puts LiquidLayer::Capture
    // outside the frame in the loop.
    void Capture();
    void Finish();

    // Draws the captured scene over the frame, blurred and dimmed. Called inside
    // the frame, and it is what replaces clearing the screen.
    void Compose(float dim) const;

private:
    // Standard deviation of the blur, in pixels, and how far the taps reach.
    //
    // The kernel in the shader is written for this figure — nine taps at sigma
    // three, which is the point past which the outermost weight stops being
    // worth a sample. Changing it here alone would leave the weights describing
    // a different curve than the one being sampled.
    static constexpr float kSigma = 3.0f;

    RenderTexture2D scene_   = {};
    RenderTexture2D across_  = {};
    Shader blur_             = {};

    int texelLocation_     = 0;
    int directionLocation_ = 0;
    int dimLocation_       = 0;

    int width_  = 0;
    int height_ = 0;
};
