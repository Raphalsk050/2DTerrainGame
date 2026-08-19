#pragma once

#include "raylib.h"
#include "world/terrain.h"

// Debug view of the noise field: the greyscale field texture drawn through a
// smoothstep fragment shader.
//
// Owns GPU resources, so Unload() must be called while the window is still
// open. There is no destructor doing it, because object lifetime does not
// naturally end before CloseWindow().
class NoiseView {
public:
    // Compiles the fragment shader and sets its smoothstep edges. Falls back to
    // raylib's default shader if the file cannot be read.
    void Load(const char *fragmentShaderPath, float edge0, float edge1);

    void Unload();

    // Replaces the texture with the current contents of the field.
    void SetField(const terrain::Field &field);

    void Draw(Vector2 position, float scale) const;

private:
    Texture2D texture_ = {};
    Shader shader_     = {};
};
