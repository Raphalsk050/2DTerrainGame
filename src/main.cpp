#include "config.h"
#include "grid.h"
#include "marching_squares.h"
#include "noise_view.h"
#include "raylib.h"
#include "terrain.h"

namespace {

// Left mouse button paints vertices solid, right button clears them. Holding a
// button paints continuously, which draws more fluidly than one click per
// vertex.
void HandleInput(Grid &grid) {
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
}

void Draw(const Grid &grid, const NoiseView &noiseView) {
    BeginDrawing();
    ClearBackground(RAYWHITE);

    marching_squares::DrawVertices(grid, config::kVertexSize, RED, LIGHTGRAY);
    marching_squares::DrawContour(grid, DARKBLUE);

    noiseView.Draw({static_cast<float>(config::kCols), static_cast<float>(config::kRows)}, 5.0f);

    DrawText("left: fill  |  right: erase  |  C: clear", 10, config::kScreenHeight - 24, 14, GRAY);

    EndDrawing();
}

} // namespace

int main() {
    InitWindow(config::kScreenWidth, config::kScreenHeight, "marching squares");
    SetTargetFPS(config::kTargetFps);

    // Assets are opened through paths relative to the executable.
    ChangeDirectory(GetApplicationDirectory());

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

    Grid grid(config::kCols, config::kRows, config::kResolution);

    const terrain::Field field = terrain::Generate(settings, grid.Cols(), grid.Rows());
    terrain::Apply(field, settings, grid);

    NoiseView noiseView;
    noiseView.Load(config::kNoiseShaderPath, 0.2f, 0.8f);
    noiseView.SetField(field);

    while (!WindowShouldClose()) {
        HandleInput(grid);
        Draw(grid, noiseView);
    }

    noiseView.Unload();
    CloseWindow();

    return 0;
}
