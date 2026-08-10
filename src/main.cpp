#include "config.h"
#include "player.h"
#include "raylib.h"
#include "terrain.h"
#include "world.h"

#include <cmath>

namespace {

// Radius in pixels of the terrain brush driven by the mouse.
constexpr float kBrushRadius = 16.0f;

// Fraction of the distance to the player the camera closes per second. Framing
// the character with a slight lag reads as smoother than pinning the view to
// the body, which makes every jump shake the whole screen.
constexpr float kCameraFollow = 8.0f;

Rectangle ViewBounds(const Camera2D &camera) {
    return {camera.target.x - camera.offset.x, camera.target.y - camera.offset.y,
            static_cast<float>(config::kScreenWidth), static_cast<float>(config::kScreenHeight)};
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

// Left mouse button adds terrain under the cursor, right button carves it away.
void HandleTerrainEditing(World &world, const Camera2D &camera) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        const Vector2 cursor = GetScreenToWorld2D(GetMousePosition(), camera);
        world.Paint(cursor, kBrushRadius, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
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

void Draw(const World &world, const Player &player, const Camera2D &camera) {
    BeginDrawing();
    ClearBackground(RAYWHITE);

    BeginMode2D(camera);

    world.Draw(ViewBounds(camera), config::kVertexSize, RED, LIGHTGRAY, DARKBLUE);
    player.Draw();

    EndMode2D();

    DrawText("A/D: move  |  space: jump  |  S: crouch  |  J: attack  |  mouse: aim", 10, config::kScreenHeight - 40, 14,
             GRAY);
    DrawText("left: add terrain  |  right: carve  |  R: regenerate", 10, config::kScreenHeight - 24, 14, GRAY);

    DrawText(
        TextFormat("chunks: %d   pos: %.0f, %.0f", world.ResidentChunks(), player.Position().x, player.Position().y),
        10, 10, 14, GRAY);

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
        .skyDepth   = 144.0f,
    };

    World world(settings, config::kResolution);
    Player player({0.0f, 0.0f});

    Camera2D camera = {};
    camera.offset   = {config::kScreenWidth / 2.0f, config::kScreenHeight / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = 1.0f;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        // The world is stepped before the player so that the chunks it is about
        // to move through are already resident.
        world.Update(ViewBounds(camera));

        HandleTerrainEditing(world, camera);
        player.Update(ReadPlayerInput(camera), world, dt);
        FollowPlayer(camera, player, dt);

        Draw(world, player, camera);
    }

    CloseWindow();

    return 0;
}
