#include "liquid_layer.h"

void LiquidLayer::Fit(int width, int height) {
    if (target_.id != 0 && target_.texture.width == width && target_.texture.height == height) return;

    Unload();

    if (width > 0 && height > 0) target_ = LoadRenderTexture(width, height);
}

void LiquidLayer::Unload() {
    if (target_.id != 0) {
        UnloadRenderTexture(target_);
        target_ = {};
    }
}

void LiquidLayer::Capture(const World &world, Rectangle view, const Camera2D &camera) {
    if (target_.id == 0) return;

    BeginTextureMode(target_);
    ClearBackground(BLANK);

    BeginMode2D(camera);
    world.DrawLiquids(view);
    EndMode2D();

    EndTextureMode();
}

void LiquidLayer::Compose(unsigned char alpha) const {
    if (target_.id == 0) return;

    // Render textures come back with their origin at the bottom, so the source
    // rectangle is given a negative height to flip it upright.
    const Rectangle source = {0.0f, 0.0f, static_cast<float>(target_.texture.width),
                              -static_cast<float>(target_.texture.height)};

    DrawTextureRec(target_.texture, source, {0.0f, 0.0f}, {255, 255, 255, alpha});
}
