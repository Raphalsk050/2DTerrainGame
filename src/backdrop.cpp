#include "backdrop.h"

#include "config.h"

namespace {

// Reading a render texture means reading it upside down: raylib stores one
// bottom-up, and a negative source height is how that is undone. Every read of
// one goes through this, including the read of the intermediate — two flips over
// two passes leave the image the way round it started.
Rectangle Flipped(const RenderTexture2D &target) {
    return {0.0f, 0.0f, static_cast<float>(target.texture.width), -static_cast<float>(target.texture.height)};
}

} // namespace

void Backdrop::Create() {
    blur_ = LoadShader(nullptr, config::kBlurShaderPath);

    texelLocation_     = GetShaderLocation(blur_, "texelSize");
    directionLocation_ = GetShaderLocation(blur_, "direction");
    dimLocation_       = GetShaderLocation(blur_, "dim");
}

void Backdrop::Fit(int width, int height) {
    if (width == width_ && height == height_) return;

    // Only the targets. The shader outlives a resize — nothing about it is sized
    // to the frame — and reloading it from disk every time the window is dragged
    // would be a file read per frame of the drag.
    if (scene_.id != 0) UnloadRenderTexture(scene_);
    if (across_.id != 0) UnloadRenderTexture(across_);

    scene_  = LoadRenderTexture(width, height);
    across_ = LoadRenderTexture(width, height);

    width_  = width;
    height_ = height;
}

void Backdrop::Unload() {
    if (scene_.id != 0) UnloadRenderTexture(scene_);
    if (across_.id != 0) UnloadRenderTexture(across_);
    if (blur_.id != 0) UnloadShader(blur_);

    scene_  = {};
    across_ = {};
    blur_   = {};

    width_  = 0;
    height_ = 0;
}

void Backdrop::Capture() {
    BeginTextureMode(scene_);
    ClearBackground(BLANK);
}

void Backdrop::Finish() {
    EndTextureMode();

    // The across pass runs here rather than in Compose because it writes into a
    // target, and a texture mode cannot be opened once the frame has been. Both
    // halves of the capture therefore have to happen on this side of it, and
    // only the pass that writes to the screen is left for later.
    const float texel[2]     = {1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)};
    const float across[2]    = {1.0f, 0.0f};
    const float undimmed     = 1.0f;

    SetShaderValue(blur_, texelLocation_, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(blur_, directionLocation_, across, SHADER_UNIFORM_VEC2);

    // The darkening belongs to the second pass alone. Applied to both it would
    // be squared, and the frame would go to a quarter where a half was asked
    // for.
    SetShaderValue(blur_, dimLocation_, &undimmed, SHADER_UNIFORM_FLOAT);

    BeginTextureMode(across_);
    ClearBackground(BLANK);
    BeginShaderMode(blur_);
    DrawTextureRec(scene_.texture, Flipped(scene_), {0.0f, 0.0f}, WHITE);
    EndShaderMode();
    EndTextureMode();
}

void Backdrop::Compose(float dim) const {
    const float texel[2] = {1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)};
    const float down[2]  = {0.0f, 1.0f};

    SetShaderValue(blur_, texelLocation_, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(blur_, directionLocation_, down, SHADER_UNIFORM_VEC2);
    SetShaderValue(blur_, dimLocation_, &dim, SHADER_UNIFORM_FLOAT);

    BeginShaderMode(blur_);
    DrawTextureRec(across_.texture, Flipped(across_), {0.0f, 0.0f}, WHITE);
    EndShaderMode();
}
