#include "config.h"
#include "grid.h"
#include "marching_squares.h"
#include "raylib.h"
// stb_perlin ships inside raylib. Without STB_PERLIN_IMPLEMENTATION this header
// contributes declarations only; the implementation is already compiled into
// libraylib. The include path is set in CMakeLists.txt.
#include "stb_perlin.h"

Texture2D noiseTexture;
Shader shader;

void LoadNoiseShader() {
    shader = LoadShader(nullptr, config::kNoiseShaderPath);

    const int edge0Loc = GetShaderLocation(shader, "edge0");
    const int edge1Loc = GetShaderLocation(shader, "edge1");

    float edge0Val = 0.2f;
    float edge1Val = 0.8f;

    SetShaderValue(shader, edge0Loc, &edge0Val, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, edge1Loc, &edge1Val, SHADER_UNIFORM_FLOAT);
}

// Tunable parameters of the noise generator.
struct NoiseSettings {
    float frequency;  // Number of large features across the map width.
    int octaves;      // Noise layers summed together.
    float lacunarity; // Frequency multiplier per octave.
    float gain;       // Amplitude multiplier per octave.
    float offsetX;    // Sampling offset, scrolls the map without changing the
    float offsetY;    // shape of the terrain.
    int seed;         // Equal seeds yield identical terrain.
    float threshold;  // Value in [0,1] above which a vertex counts as ground.
    int skyRows;      // Number of rows at the top forced to stay empty.
};

// Fractal Brownian motion: sums several octaves of Perlin noise, each with its
// frequency multiplied by `lacunarity` and its amplitude by `gain`. The result
// is renormalised by the accumulated amplitude so that contrast stays
// independent of the octave count.
static float FbmNoise(float x, float y, const NoiseSettings &s) {
    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < s.octaves; o++) {
        // Perturbing the seed per octave decorrelates the layers.
        sum += stb_perlin_noise3_seed(x * frequency, y * frequency, 0.0f, 0, 0, 0, s.seed + o) * amplitude;

        maxAmplitude += amplitude;
        frequency *= s.lacunarity;
        amplitude *= s.gain;
    }

    return (maxAmplitude > 0.0f) ? (sum / maxAmplitude) : 0.0f;
}

// Samples the noise once per grid vertex and thresholds it into the inside or
// outside state. Also builds the greyscale debug texture from the continuous
// field, before the threshold is applied.
static void GenerateTerrain(Grid &grid, const NoiseSettings &s) {
    Image noise = GenImageColor(grid.Cols(), grid.Rows(), BLACK);

    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            // Both axes are divided by the column count so that noise cells
            // stay square.
            const float nx = (i + s.offsetX) * s.frequency / grid.Cols();
            const float ny = (j + s.offsetY) * s.frequency / grid.Cols();

            const float n = (FbmNoise(nx, ny, s) + 1.0f) * 0.5f; // [-1,1] -> [0,1]

            grid.SetSolid(i, j, n > s.threshold && j > s.skyRows);

            const auto v = static_cast<unsigned char>(n * 255.0f);
            ImageDrawPixel(&noise, i, j, {v, v, v, 255});
        }
    }

    if (noiseTexture.id != 0) UnloadTexture(noiseTexture);

    noiseTexture = LoadTextureFromImage(noise);
    UnloadImage(noise);
}

int main() {
    InitWindow(config::kScreenWidth, config::kScreenHeight, "marching squares");
    SetTargetFPS(config::kTargetFps);

    Grid grid(config::kCols, config::kRows, config::kResolution);

    const NoiseSettings settings = {
        .frequency  = 4.0f,
        .octaves    = 4,
        .lacunarity = 2.0f,
        .gain       = 0.5f,
        .offsetX    = 0.0f,
        .offsetY    = 0.0f,
        .seed       = 1337,
        .threshold  = 0.45f,
        .skyRows    = 18,
    };

    GenerateTerrain(grid, settings);
    LoadNoiseShader();

    while (!WindowShouldClose()) {
        // Left mouse button paints vertices solid, right button clears them.
        // Holding a button paints continuously.
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            const Vector2 mouse = GetMousePosition();
            const bool solid    = IsMouseButtonDown(MOUSE_BUTTON_LEFT);

            int i = 0;
            int j = 0;
            if (grid.PickVertex(mouse, config::kPickRadius, i, j)) {
                grid.SetSolid(i, j, solid);
            }
        }

        if (IsKeyPressed(KEY_C)) grid.Clear();

        BeginDrawing();
        ClearBackground(RAYWHITE);

        marching_squares::DrawVertices(grid, config::kVertexSize, RED, LIGHTGRAY);
        marching_squares::DrawContour(grid, DARKBLUE);

        BeginShaderMode(shader);
        DrawTextureEx(noiseTexture, {static_cast<float>(config::kCols), static_cast<float>(config::kRows)}, 0.0f, 5.0f,
                      WHITE);
        EndShaderMode();

        DrawText("left: fill  |  right: erase  |  C: clear", 10, config::kScreenHeight - 24, 14, GRAY);

        EndDrawing();
    }

    UnloadTexture(noiseTexture);
    UnloadShader(shader);
    CloseWindow();

    return 0;
}
