#include "config.h"
#include "grid.h"
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

static Vector2 Midpoint(Vector2 a, Vector2 b) {
    return {(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
}

// Every cell has four corners that are either solid or empty, giving 16
// possible configurations. The state index is built by reading the corners
// clockwise from the top left (a=8, b=4, c=2, d=1), and each case selects the
// edges the contour crosses. Corner values are binary, so the crossing always
// falls at the edge midpoint and no interpolation is involved.
static void DrawCell(Vector2 a, Vector2 b, Vector2 c, Vector2 d, bool va, bool vb, bool vc, bool vd, Color color) {
    const int state = (va << 3) | (vb << 2) | (vc << 1) | (vd);
    if (state == 0 || state == 15) return; // Cell entirely outside or inside.

    const Vector2 top    = Midpoint(a, b);
    const Vector2 right  = Midpoint(b, c);
    const Vector2 bottom = Midpoint(d, c);
    const Vector2 left   = Midpoint(a, d);

    const float thickness = 2.0f;

    switch (state) {
    case 1:
    case 14: DrawLineEx(left, bottom, thickness, color); break;
    case 2:
    case 13: DrawLineEx(bottom, right, thickness, color); break;
    case 3:
    case 12: DrawLineEx(left, right, thickness, color); break;
    case 4:
    case 11: DrawLineEx(top, right, thickness, color); break;
    case 6:
    case 9: DrawLineEx(top, bottom, thickness, color); break;
    case 7:
    case 8: DrawLineEx(left, top, thickness, color); break;

    // Ambiguous cases: the two solid corners are diagonal, so two different
    // connections are possible. The convention here is to always separate the
    // diagonals.
    case 5:
        DrawLineEx(left, top, thickness, color);
        DrawLineEx(bottom, right, thickness, color);
        break;
    case 10:
        DrawLineEx(left, bottom, thickness, color);
        DrawLineEx(top, right, thickness, color);
        break;
    }
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

        const Vector2 origin = {config::kVertexSize / 2.0f, config::kVertexSize / 2.0f};
        for (int i = 0; i < grid.Cols(); i++) {
            for (int j = 0; j < grid.Rows(); j++) {
                const Vector2 p      = grid.PointAt(i, j);
                const Rectangle rect = {p.x, p.y, config::kVertexSize, config::kVertexSize};

                DrawRectanglePro(rect, origin, 0.0f, grid.IsSolid(i, j) ? RED : LIGHTGRAY);
            }
        }

        // One cell per pair of neighbouring columns and rows, hence the -1.
        for (int i = 0; i < grid.Cols() - 1; i++) {
            for (int j = 0; j < grid.Rows() - 1; j++) {
                DrawCell(grid.PointAt(i, j), grid.PointAt(i + 1, j), grid.PointAt(i + 1, j + 1),
                         grid.PointAt(i, j + 1), grid.IsSolid(i, j), grid.IsSolid(i + 1, j), grid.IsSolid(i + 1, j + 1),
                         grid.IsSolid(i, j + 1), DARKBLUE);
            }
        }

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
