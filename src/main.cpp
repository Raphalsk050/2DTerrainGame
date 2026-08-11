#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "hotbar.h"
#include "light_layer.h"
#include "liquid_layer.h"
#include "player.h"
#include "raylib.h"
#include "terrain.h"
#include "world.h"

#include <algorithm>
#include <cmath>

namespace {

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

// The lantern as linear light, at a given strength.
light::Radiance Lantern(float strength) {
    constexpr float kByte = 1.0f / 255.0f;

    return {config::kLanternGlow.r * kByte * strength, config::kLanternGlow.g * kByte * strength,
            config::kLanternGlow.b * kByte * strength};
}

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

void FollowPlayer(Camera2D &camera, const Player &player, float dt) {
    const Vector2 target = player.Centre();

    // Exponential approach, expressed so that the rate is the same whatever the
    // frame duration.
    const float t = 1.0f - std::exp(-kCameraFollow * dt);

    camera.target.x += (target.x - camera.target.x) * t;
    camera.target.y += (target.y - camera.target.y) * t;
}

// Badge showing what the next click will do, next to the bar that decides what
// it will do it with. The brush is modal, so the mode has to be somewhere the
// eye passes without being sent looking for it.
void DrawBrushMode(const Editor &editor) {
    const bool place  = editor.CurrentMode() == Editor::Mode::Place;
    const Color color = place ? Color{120, 200, 130, 255} : Color{235, 84, 84, 255};

    const char *text = TextFormat("%s  (X)     brush %.0f  (- / +)", editor.ModeName(), editor.Radius());
    const int width  = MeasureText(text, 14);

    // Sat just clear of the bar, which reaches 76 pixels up from the bottom.
    const Rectangle badge = {(config::kScreenWidth - width) / 2.0f - 8.0f, config::kScreenHeight - 104.0f,
                             width + 16.0f, 22.0f};

    DrawRectangleRec(badge, {30, 34, 42, 220});
    DrawRectangleLinesEx(badge, 2.0f, color);
    DrawText(text, static_cast<int>(badge.x + 8.0f), static_cast<int>(badge.y + 4.0f), 14, color);
}

void Draw(const World &world, const Player &player, const Hotbar &hotbar, const Editor &editor,
          const LiquidLayer &liquids, const LightLayer &lights, const Camera2D &camera,
          const debug_view::Toggles &debug, float lantern) {
    const Rectangle view = ViewBounds(camera);

    BeginDrawing();
    ClearBackground(RAYWHITE);

    BeginMode2D(camera);
    world.DrawTerrain(view);
    player.Draw();
    EndMode2D();

    // Composited over the character, so a submerged body is tinted by the
    // liquid it is standing in.
    liquids.Compose(config::kLiquidAlpha);

    BeginMode2D(camera);

    // Then the whole scene is multiplied by the light at once. Everything drawn
    // before this line is lit; everything after it is not, which is exactly the
    // right side of the line for anything meant to be read rather than seen.
    lights.Compose();

    // Drawn over the world rather than under it: the point of an overlay is to
    // check the world against what produced it, which is impossible while the
    // world covers it.
    if (debug.vertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);
    if (debug.layers) debug_view::DrawLayers(world, view);
    if (debug.chunks) debug_view::DrawChunks(world, view);
    if (debug.light) debug_view::DrawLight(world, view);

    editor.DrawCursor(hotbar, camera);

    EndMode2D();

    DrawText("A/D: move  |  space: jump  |  S: crouch  |  J: attack  |  mouse: aim", 10, 10, 14, GRAY);
    DrawText("left: apply brush  |  X: place/dig  |  1-6 or wheel: material  |  - / +: brush size  |  R: regenerate",
             10, 28, 14, GRAY);
    DrawText(TextFormat("V: vertices  |  F3: chunks  |  F4: height grid  |  F5: light probes  |  , . : lantern %.1f",
                        lantern),
             10, 46, 14, GRAY);

    DrawText(TextFormat("chunks: %d (%d pinned)   water in view: %.1f   light rays: %ld", world.ResidentChunks(),
                        world.PinnedChunks(), world.TotalWater(view), world.Light().Rays()),
             10, 70, 14, GRAY);

    const Vector2 centre = player.Centre();
    const auto under     = editor.Under();

    DrawText(TextFormat("y: %d   under cursor: %s   light here: %.2f   light at cursor: %.2f",
                        static_cast<int>(centre.y), under.has_value() ? Def(*under).name : "open",
                        world.LightLevelAt(centre), world.LightLevelAt(editor.Aim())),
             10, 88, 14, GRAY);

    DrawBrushMode(editor);
    hotbar.Draw(editor.Collected());

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
    Editor editor;

    LiquidLayer liquids;
    liquids.Load(config::kScreenWidth, config::kScreenHeight);

    LightLayer lights;

    Camera2D camera = {};
    camera.offset   = {config::kScreenWidth / 2.0f, config::kScreenHeight / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = 1.0f;

    float accumulated = 0.0f;
    float lantern     = config::kLanternStrength;

    debug_view::Toggles debug;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        // Chunks are generated over the simulated band, not merely the visible
        // one. A write-back to a vertex whose chunk is absent is dropped, which
        // would quietly destroy the liquid that flowed there.
        const Rectangle active = Expand(ViewBounds(camera), kSimulationMargin);

        world.Update(active);

        hotbar.Update();
        editor.Update(world, hotbar, camera);

        debug_view::ReadToggles(debug);
        if (IsKeyPressed(KEY_R)) world.Reset();

        // Turned up and down while walking, since how much light the player
        // carries is a balance question and the only way to settle it is to be
        // underground at each setting. Zero is a valid answer: it leaves the
        // dark to torches alone.
        if (IsKeyPressed(KEY_COMMA)) lantern = std::max(lantern - config::kLanternStep, 0.0f);
        if (IsKeyPressed(KEY_PERIOD)) lantern = std::min(lantern + config::kLanternStep, config::kLanternMax);

        accumulated = std::min(accumulated + dt, kMaxAccumulated);
        while (accumulated >= kWaterStep) {
            world.StepWater(active);
            accumulated -= kWaterStep;
        }

        player.Update(ReadPlayerInput(camera), world, dt);
        FollowPlayer(camera, player, dt);

        // Re-offered every frame rather than registered once. A light that has
        // to be renewed to keep burning needs nothing told to it when the thing
        // carrying it moves, and nothing told to it when that thing is gone.
        world.AddLight(player.Centre(), Lantern(lantern), config::kLanternRadius);

        // Solved after the world has finished moving, so the light matches the
        // frame it is about to be drawn over rather than the one before it.
        world.StepLight(active);
        lights.Update(world.Light());

        // Captured before the frame opens, since it renders to its own target.
        liquids.Capture(world, ViewBounds(camera), camera);

        Draw(world, player, hotbar, editor, liquids, lights, camera, debug, lantern);
    }

    lights.Unload();
    liquids.Unload();
    CloseWindow();

    return 0;
}
