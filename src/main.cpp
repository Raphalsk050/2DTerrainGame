#include "config.h"
#include "grid.h"
#include "marching_squares.h"
#include "raylib.h"
#include "terrain.h"

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

int main() {
    InitWindow(config::kScreenWidth, config::kScreenHeight, "marching squares");
    SetTargetFPS(config::kTargetFps);

    Grid grid(config::kCols, config::kRows, config::kResolution);

    const terrain::Settings settings = {
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

    const terrain::Field field = terrain::Generate(settings, grid.Cols(), grid.Rows());
    terrain::Apply(field, settings, grid);

    Image noiseImage = terrain::ToImage(field);
    noiseTexture     = LoadTextureFromImage(noiseImage);
    UnloadImage(noiseImage);

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
