#include "noise_view.h"

void NoiseView::Load(const char *fragmentShaderPath, float edge0, float edge1) {
    shader_ = ::LoadShader(nullptr, fragmentShaderPath);

    const int edge0Loc = GetShaderLocation(shader_, "edge0");
    const int edge1Loc = GetShaderLocation(shader_, "edge1");

    SetShaderValue(shader_, edge0Loc, &edge0, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, edge1Loc, &edge1, SHADER_UNIFORM_FLOAT);
}

void NoiseView::Unload() {
    if (texture_.id != 0) {
        UnloadTexture(texture_);
        texture_ = {};
    }
    if (shader_.id != 0) {
        UnloadShader(shader_);
        shader_ = {};
    }
}

void NoiseView::SetField(const terrain::Field &field) {
    Image image = terrain::ToImage(field);

    // Release the previous texture before replacing it, so repeated calls do
    // not leak VRAM.
    if (texture_.id != 0) UnloadTexture(texture_);

    texture_ = LoadTextureFromImage(image);
    UnloadImage(image);
}

void NoiseView::Draw(Vector2 position, float scale) const {
    if (texture_.id == 0) return;

    BeginShaderMode(shader_);
    DrawTextureEx(texture_, position, 0.0f, scale, WHITE);
    EndShaderMode();
}
