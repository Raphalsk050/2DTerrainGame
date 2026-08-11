#include "light_layer.h"

#include "config.h"

#include <algorithm>

namespace {

// Ordered dither, the usual four by four.
//
// A light gradient across a dark room covers a hundred pixels and perhaps three
// of the 256 values a channel has, so rounding each probe to the nearest of them
// draws the gradient as three flat bands with hard edges between them. Nudging
// the rounding by a fixed pattern spreads the error over neighbouring probes
// instead, and since the texture is interpolated on its way to the screen the
// pattern is averaged back out. What is left is the gradient, without the bands.
constexpr float kDither[4][4] = {
    {0.0f, 8.0f, 2.0f, 10.0f},
    {12.0f, 4.0f, 14.0f, 6.0f},
    {3.0f, 11.0f, 1.0f, 9.0f},
    {15.0f, 7.0f, 13.0f, 5.0f},
};

unsigned char ToByte(float value, int i, int j) {
    // Only worth doing when the texture is blended on its way to the screen.
    // Drawn as flat blocks the pattern is not averaged back out by anything,
    // and a dither meant to be invisible becomes a checkerboard four probes
    // wide laid over the world.
    const float nudge = config::kBlockyLight ? 0.0f : (kDither[i & 3][j & 3] / 16.0f - 0.5f);

    return static_cast<unsigned char>(std::clamp(value * 255.0f + nudge, 0.0f, 255.0f) + 0.5f);
}

} // namespace

void LightLayer::Unload() {
    if (texture_.id != 0) UnloadTexture(texture_);

    texture_ = {};
    cols_    = 0;
    rows_    = 0;
}

void LightLayer::Update(const light::Field &field) {
    const int cols = field.Cols();
    const int rows = field.Rows();

    if (cols <= 0 || rows <= 0) return;

    origin_  = field.Origin();
    spacing_ = field.Spacing();

    if (cols != cols_ || rows != rows_ || texture_.id == 0) {
        Unload();

        Image blank = GenImageColor(cols, rows, BLACK);
        texture_    = LoadTextureFromImage(blank);
        UnloadImage(blank);

        // Clamped, so the last row of probes is not wrapped around to the first
        // along the edge of the region.
        SetTextureFilter(texture_, config::kBlockyLight ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(texture_, TEXTURE_WRAP_CLAMP);

        cols_ = cols;
        rows_ = rows;

        pixels_.assign(static_cast<std::size_t>(cols) * rows, BLACK);
    }

    const float exposure = field.Exposure();

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const light::Radiance value = field.ProbeAt(i, j);

            // Each channel is exposed on its own rather than the brightness as
            // a whole, so a torch stays warm where it is strong and washes
            // towards white only where it is overwhelming, the way a real
            // light does.
            pixels_[static_cast<std::size_t>(j) * cols + i] = {
                ToByte(light::Expose(value.r, exposure), i, j),
                ToByte(light::Expose(value.g, exposure), i, j),
                ToByte(light::Expose(value.b, exposure), i, j),
                255,
            };
        }
    }

    UpdateTexture(texture_, pixels_.data());
}

void LightLayer::Compose() const {
    if (texture_.id == 0) return;

    const Rectangle source = {0.0f, 0.0f, static_cast<float>(cols_), static_cast<float>(rows_)};

    // A probe sits at the centre of its own cell, and so does a texel, so the
    // texture covers the probe grid's whole extent rather than the span between
    // the first and last probe.
    const Rectangle target = {origin_.x, origin_.y, cols_ * spacing_, rows_ * spacing_};

    BeginBlendMode(BLEND_MULTIPLIED);
    DrawTexturePro(texture_, source, target, {0.0f, 0.0f}, 0.0f, WHITE);
    EndBlendMode();
}
