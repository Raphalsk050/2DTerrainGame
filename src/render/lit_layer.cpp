#include "render/lit_layer.h"

#include "rlgl.h"

void LitLayer::Fit(int width, int height) {
    if (target_.id != 0 && target_.texture.width == width && target_.texture.height == height) return;

    Unload();

    if (width > 0 && height > 0) target_ = LoadRenderTexture(width, height);
}

void LitLayer::Unload() {
    if (target_.id != 0) {
        UnloadRenderTexture(target_);
        target_ = {};
    }
}

void LitLayer::Blend() {
    // Ordinary alpha compositing for the colour, and the *correct* one for the
    // alpha — which raylib's BLEND_ALPHA does not do. It sets one factor pair
    // for both channels, so alpha comes out
    //
    //     dst.a = src.a * src.a + dst.a * (1 - src.a)
    //
    // and is squared on the first thing drawn over an empty target.
    //
    // Over an opaque background nobody could ever see that, and the two layers
    // this codebase already had are both drawn over one. Here the background is
    // the sky and the sky is deliberately not in the layer, so the alpha *is*
    // half the picture: it is what says how much cloud, rain or fog stands in
    // front of the blue. Squared, a cloud drawn at a half would let through
    // three quarters of the sky rather than a half, and the thinner the thing
    // the further out it would be.
    //
    // What this leaves in the target is premultiplied — every colour already
    // scaled by its own coverage — which is why Compose blends it as such.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,  // colour: over
                              RL_ONE,       RL_ONE_MINUS_SRC_ALPHA,  // alpha:  over, properly
                              RL_FUNC_ADD,  RL_FUNC_ADD);

    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
}

void LitLayer::Capture() {
    if (target_.id == 0) return;

    BeginTextureMode(target_);
    ClearBackground(BLANK);

    Blend();
}

void LitLayer::Finish() {
    if (target_.id == 0) return;

    EndBlendMode();
    EndTextureMode();
}

void LitLayer::Compose() const {
    if (target_.id == 0) return;

    // A render texture is filled bottom row first, so it is read back with a
    // negative height to stand it upright.
    const Rectangle source = {0.0f, 0.0f, static_cast<float>(target_.texture.width),
                              -static_cast<float>(target_.texture.height)};

    // Premultiplied, because that is what Capture's blend left in the target.
    // The ordinary alpha blend would scale every partly covered pixel by its own
    // coverage a second time, and put a cloud, a shower and a fog bank all at
    // the square of the strength they were drawn at.
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    DrawTextureRec(target_.texture, source, {0.0f, 0.0f}, WHITE);
    EndBlendMode();
}
