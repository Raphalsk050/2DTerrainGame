#include "config.h"
#include "hotbar.h"
#include "liquid_layer.h"
#include "player.h"
#include "raylib.h"
#include "terrain.h"
#include "world.h"

#include <algorithm>
#include <cmath>

namespace {

// Radius in pixels of the terrain brush driven by the mouse.
constexpr float kBrushRadius = 16.0f;

// Fraction of the distance to the player the camera closes per second. Framing
// the character with a slight lag reads as smoother than pinning the view to
// the body, which makes every jump shake the whole screen.
constexpr float kCameraFollow = 8.0f;

// The liquid automaton advances in fixed increments. Feeding it the frame time
// would make a long frame move liquid several cells at once, which the flow
// limits are not built to absorb.
constexpr float kWaterStep = 1.0f / 60.0f;

// Upper bound on the time carried into the next frame, so a stall does not
// queue up hundreds of steps and stall the frame after it as well.
constexpr float kMaxAccumulated = 0.25f;

// Liquid is simulated over a band wider than the view, so that what happens
// just off screen has already settled by the time it scrolls in.
constexpr float kSimulationMargin = 128.0f;

Rectangle ViewBounds(const Camera2D &camera) {
    return {camera.target.x - camera.offset.x, camera.target.y - camera.offset.y,
            static_cast<float>(config::kScreenWidth), static_cast<float>(config::kScreenHeight)};
}

Rectangle Expand(Rectangle rect, float margin) {
    return {rect.x - margin, rect.y - margin, rect.width + 2.0f * margin, rect.height + 2.0f * margin};
}

PlayerInput ReadPlayerInput(const Camera2D &camera) {
    PlayerInput input;

    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) input.moveX -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) input.moveX += 1.0f;

    input.jumpPressed   = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W);
    input.jumpHeld      = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W);
    input.crouchHeld    = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
    input.attackPressed = IsKeyPressed(KEY_J);

    input.aimWorld = GetScreenToWorld2D(GetMousePosition(), camera);

    return input;
}

// Left mouse button places the selected element, right button removes it.
void HandleEditing(World &world, const Hotbar &hotbar, const Camera2D &camera) {
    const Vector2 mouse = GetMousePosition();
    if (hotbar.Contains(mouse)) return;

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        world.Paint(hotbar.Selected(), GetScreenToWorld2D(mouse, camera), kBrushRadius,
                    IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    }

    if (IsKeyPressed(KEY_R)) world.Reset();
}

void FollowPlayer(Camera2D &camera, const Player &player, float dt) {
    const Vector2 target = player.Centre();

    // Exponential approach, expressed so that the rate is the same whatever the
    // frame duration.
    const float t = 1.0f - std::exp(-kCameraFollow * dt);

    camera.target.x += (target.x - camera.target.x) * t;
    camera.target.y += (target.y - camera.target.y) * t;
}

void Draw(const World &world, const Player &player, const Hotbar &hotbar, const LiquidLayer &liquids,
          const Camera2D &camera, bool showVertices) {
    const Rectangle view = ViewBounds(camera);

    BeginDrawing();
    ClearBackground(RAYWHITE);

    BeginMode2D(camera);

    world.DrawTerrain(view);

    // Drawn over the world rather than under it: the point of the overlay is to
    // check the fill against the samples that produced it, which is impossible
    // while the fill covers it.
    if (showVertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);

    player.Draw();

    EndMode2D();

    // Composited last and over the character, so a submerged body is tinted by
    // the liquid it is standing in.
    liquids.Compose(config::kLiquidAlpha);

    DrawText("A/D: move  |  space: jump  |  S: crouch  |  J: attack  |  mouse: aim", 10, 10, 14, GRAY);
    DrawText("left: place  |  right: remove  |  1-2 or wheel: select  |  R: regenerate  |  V: vertices", 10, 28, 14,
             GRAY);

    DrawText(TextFormat("chunks: %d   water in view: %.1f", world.ResidentChunks(), world.TotalWater(view)), 10, 46, 14,
             GRAY);

    hotbar.Draw();

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
        .skyDepth   = 144.0f,
        .skyFade    = 96.0f,
    };

    World world(settings, config::kResolution);
    Player player({0.0f, 0.0f});
    Hotbar hotbar;

    LiquidLayer liquids;
    liquids.Load(config::kScreenWidth, config::kScreenHeight);

    Camera2D camera = {};
    camera.offset   = {config::kScreenWidth / 2.0f, config::kScreenHeight / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = 1.0f;

    float accumulated = 0.0f;
    bool showVertices = false;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        // Chunks are generated over the simulated band, not merely the visible
        // one. A write-back to a vertex whose chunk is absent is dropped, which
        // would quietly destroy the liquid that flowed there.
        const Rectangle active = Expand(ViewBounds(camera), kSimulationMargin);

        world.Update(active);

        hotbar.Update();
        HandleEditing(world, hotbar, camera);

        if (IsKeyPressed(KEY_V)) showVertices = !showVertices;

        accumulated = std::min(accumulated + dt, kMaxAccumulated);
        while (accumulated >= kWaterStep) {
            world.StepWater(active);
            accumulated -= kWaterStep;
        }

        player.Update(ReadPlayerInput(camera), world, dt);
        FollowPlayer(camera, player, dt);

        // Captured before the frame opens, since it renders to its own target.
        liquids.Capture(world, ViewBounds(camera), camera);

        Draw(world, player, hotbar, liquids, camera, showVertices);
    }

    liquids.Unload();
    CloseWindow();

    return 0;
}
