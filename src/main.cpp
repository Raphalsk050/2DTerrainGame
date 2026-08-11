#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "hotbar.h"
#include "light_layer.h"
#include "liquid_layer.h"
#include "player.h"
#include "raylib.h"
#include "terrain.h"
#include "weather.h"
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

// The world region the frame covers. Read from the window rather than from the
// configured size, so a resized window shows more of the world instead of the same
// amount of it stretched.
Rectangle ViewBounds(const Camera2D &camera) {
    return {camera.target.x - camera.offset.x, camera.target.y - camera.offset.y, static_cast<float>(GetScreenWidth()),
            static_cast<float>(GetScreenHeight())};
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

    // The vertical axis is the same two keys as jump and crouch. Only flight
    // reads it, and while flying neither of those actions applies, so there is
    // nothing for it to conflict with.
    if (input.jumpHeld) input.moveY -= 1.0f;
    if (input.crouchHeld) input.moveY += 1.0f;

    input.flyToggled = IsKeyPressed(KEY_F);
    input.boostHeld  = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

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
    const Rectangle badge = {(GetScreenWidth() - width) / 2.0f - 8.0f, GetScreenHeight() - 104.0f, width + 16.0f,
                             22.0f};

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

    // The sky first, so the ground is drawn over it. Underground the band is out of
    // view and this returns having done nothing.
    world.Sky().DrawClouds(view, world.Spacing());

    world.DrawTerrain(view);
    player.Draw();

    // Rain in front of the world rather than behind it, so it falls past a cliff
    // face instead of behind one. Still inside the light, because rain in an unlit
    // place should not be the one bright thing on screen.
    world.Sky().DrawRain(view);

    EndMode2D();

    // Composited over the character, so a submerged body is tinted by the
    // liquid it is standing in.
    liquids.Compose(config::kLiquidAlpha);

    BeginMode2D(camera);

    // Then the whole scene is multiplied by the light at once. Everything drawn
    // before this line is lit; everything after it is not, which is exactly the
    // right side of the line for anything meant to be read rather than seen.
    //
    // Skipping the multiply is all it takes to see the world unlit, since the
    // world underneath was already drawn at full brightness.
    if (!debug.unlit) lights.Compose();

    // Drawn over the world rather than under it: the point of an overlay is to
    // check the world against what produced it, which is impossible while the
    // world covers it.
    if (debug.vertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);
    if (debug.layers) debug_view::DrawLayers(world, view);
    if (debug.chunks) debug_view::DrawChunks(world, view);
    if (debug.light) debug_view::DrawLight(world, view);

    editor.DrawCursor(hotbar, camera);

    EndMode2D();

    DrawText("A/D: move  |  space: jump  |  S: crouch  |  J: attack  |  mouse: aim  |  F: fly", 10, 10, 14, GRAY);
    DrawText("left: apply brush  |  X: place/dig  |  1-9 or wheel: material  |  - / +: brush size  |  R: regenerate",
             10, 28, 14, GRAY);
    DrawText(TextFormat("V: vertices  |  F3: chunks  |  F4: height grid  |  F5: light probes  |  F6: unlit %s  |"
                        "  , . : lantern %.1f",
                        debug.unlit ? "on" : "off", lantern),
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

    // The weather over the character, and the two things it is made of. Read
    // together they show the chain working: a wet or high column is cloudier, a
    // cloudier one is shaded and eventually rains.
    const terrain::Climate climate = terrain::ClimateAt(centre.x, world.Settings());
    const weather::Column column   = world.Sky().ColumnAt(centre.x);

    DrawText(TextFormat("climate: %.2f warm  %.2f wet   |   sky: %.0f%% overcast  %.0f%% cloud here  "
                        "%.0f%% shade   |   rain: %.0f%%",
                        climate.temperature, climate.humidity, column.cover * 100.0f,
                        world.Sky().CoverAt(centre.x) * 100.0f, world.Sky().ShadeAt(centre.x) * 100.0f,
                        column.rain * 100.0f),
             10, 106, 14, column.rain > 0.0f ? Color{140, 170, 210, 255} : GRAY);

    // Flight suspends gravity and collision both, which is not something to be
    // left on by accident, so it says so where the eye already is.
    if (player.IsFlying()) {
        const char *text = "flying  (F)     W/S: up and down     shift: boost";
        const int width  = MeasureText(text, 14);

        const Rectangle badge = {GetScreenWidth() - width - 26.0f, 8.0f, width + 16.0f, 22.0f};

        DrawRectangleRec(badge, {30, 34, 42, 220});
        DrawRectangleLinesEx(badge, 2.0f, {170, 130, 220, 255});
        DrawText(text, static_cast<int>(badge.x + 8.0f), static_cast<int>(badge.y + 4.0f), 14, {190, 160, 235, 255});
    }

    DrawBrushMode(editor);
    hotbar.Draw(editor.Collected());

    EndDrawing();
}

} // namespace

int main() {
    // Resizable, with a floor under it: below the minimum the hotbar is wider than
    // the frame and the head-up display runs off the side of it.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);

    InitWindow(config::kScreenWidth, config::kScreenHeight, "marching squares");
    SetWindowMinSize(config::kMinScreenWidth, config::kMinScreenHeight);
    SetTargetFPS(config::kTargetFps);

    // Assets are opened through paths relative to the executable.
    ChangeDirectory(GetApplicationDirectory());

    // The world, written one layer at a time. Every number below is in world
    // pixels or in features per terrain::kFeatureSpan pixels, so the settings can
    // be read against the size of the character: it is 26 pixels tall, 12 wide,
    // and jumps 72.
    const terrain::Settings settings =
        {
            .surface =
                {
                    .level = 144.0f,

                    // Where the land is broadly high and where it is broadly low.
                    // One feature spans a couple of screens, so this is what the
                    // player reads as having travelled somewhere.
                    .relief          = {.frequency = 0.45f, .octaves = 2, .seed = 4401},
                    .reliefAmplitude = 150.0f,

                    // The hills actually walked over. Amplitude against frequency is
                    // what sets the slope: this pair averages about a quarter, so a
                    // hillside is climbed at roughly fourteen degrees and its
                    // steepest stretches at forty.
                    .hills         = {.frequency = 2.6f, .octaves = 3, .seed = 4402},
                    .hillAmplitude = 70.0f,

                    // Texture underfoot, kept small. This is the term that turns a
                    // walkable slope into a staircase of one-pixel steps.
                    .detail          = {.frequency = 9.0f, .octaves = 2, .seed = 4403},
                    .detailAmplitude = 12.0f,

                    // Broad enough that a plain is a place rather than a gap
                    // between hills, and a floor low enough that a fully eroded
                    // stretch is genuinely flat ground.
                    .erosion      = {.frequency = 0.55f, .octaves = 1, .seed = 4404},
                    .erosionFloor = 0.22f,

                    // Half the slope is snapped into ledges a quarter of the
                    // character's jump apart, so a hillside is somewhere to stand
                    // rather than a ramp.
                    .terrace     = 0.45f,
                    .terraceStep = 24.0f,

                    // Overhangs, off by default. It is the one layer that can put a
                    // hole back in open ground, so it is turned up by eye and left
                    // alone until then.
                    .warp          = {.frequency = 3.0f, .octaves = 2, .seed = 4405},
                    .warpAmplitude = 0.0f,
                    .warpDepth     = 96.0f,
                },
            .caves =
                {
                    // Two hundred pixels of solid ground over everything below,
                    // which is what keeps the surface a surface.
                    .crust     = 56.0f,
                    .crustFade = 72.0f,

                    // Roughly half the underground is cave country, with a fairly
                    // sharp border, so arriving in it is noticeable.
                    .region         = {.frequency = 0.6f, .octaves = 2, .seed = 4410},
                    .regionCoverage = 0.55f,
                    .regionFade     = 0.12f,

                    // The rooms. A twelfth of the eligible ground, opening to about
                    // four times the character's height at the middle, which is what
                    // gives the corridors a change of scale to lead into.
                    .chamber         = {.frequency = 4.0f, .octaves = 2, .seed = 4411},
                    .chamberCoverage = 0.070f,
                    .chamberDepth    = 58.0f,

                    // The halls: stretched three to one, so they run sideways and can
                    // be walked rather than fallen down. Thirty-four pixels of
                    // headroom where they start, opening out to forty-six well
                    // underground — still comfortably over the character's height,
                    // since a hall that has to be crouched through is a crawlway.
                    .galleries = {.shape        = {.frequency = 2.2f, .octaves = 2, .aspect = 3.0f, .seed = 4412},
                                  .width        = 16.0f,
                                  .widthAtDepth = 21.0f,
                                  .growthDepth  = 1400.0f},

                    // Kept a little under half where the region says solid rock, so
                    // that dead rock is somewhere to squeeze through rather than
                    // somewhere the world ends.
                    .galleryFloor = 0.45f,

                    // The links between the halls, at the height a character has to
                    // crouch to pass. Less stretched, so they cut across the halls
                    // instead of running alongside them.
                    //
                    // Thinned by frequency rather than by width when there are too
                    // many of them: a narrower crawlway stops being passable at all,
                    // and a passage that cannot be used is worse than one that is not
                    // there.
                    .crawlways = {.shape        = {.frequency = 3.0f, .octaves = 2, .aspect = 1.5f, .seed = 4413},
                                  .width        = 10.0f,
                                  .widthAtDepth = 13.0f,
                                  .growthDepth  = 1400.0f},

                    // The way in. Stretched the other way, so it descends, and rare:
                    // roughly one mouth per screen and a half of travel. Rarity comes
                    // from the frequency and the aspect together, since a vertically
                    // stretched field has one curve per band of horizontal distance.
                    // Three times the character's width, so the descent is a passage
                    // and not a squeeze.
                    .shafts = {.shape = {.frequency = 0.18f, .octaves = 2, .aspect = 0.22f, .seed = 4414},
                               .width = 18.0f},

                    // Clears the crust with room to spare, so a mouth at the surface
                    // always reaches the halls rather than stopping in rock.
                    .shaftReach = 340.0f,
                },

            // Nothing about the shape of the world reads these yet. The sky does, and
            // a biome table will. One feature spans four or five screens, so a climate
            // is somewhere arrived in rather than something that changes underfoot.
            .climate =
                {
                    .temperature = {.frequency = 0.22f, .octaves = 2, .seed = 4420},
                    .humidity    = {.frequency = 0.30f, .octaves = 2, .seed = 4421},

                    // Over the hundred and twenty pixels of elevation this relief
                    // reaches, high ground gains a fifth of the humidity range and
                    // loses a seventh of the temperature range. Enough that the peaks
                    // are visibly cloudier than the plains beside them, not enough
                    // that either runs to an extreme on altitude alone.
                    .humidityLift     = 0.0016f,
                    .temperatureLapse = 0.0011f,
                },
            .seed = 1337,
        };

    // The sky. Cloud, and the two things cloud is: shade on the ground below it and
    // rain out of the bottom of it.
    const weather::Settings sky = {
        // Well above the highest ground, which the relief puts at about y 20, so
        // cloud is always sky and never something that can be walked into.
        .ceiling  = -640.0f,
        .base     = -320.0f,
        .rainDrop = 100.0f,

        // Stretched a little over two to one, because a cloud is wider than it is
        // tall. Frequency against aspect is what sets the size: this pair gives one
        // about four hundred and forty pixels across and two hundred deep, so a
        // couple of them are in view at once and each is comfortably shorter than the
        // band it floats in. Stretched much further and one cloud is wider than the
        // screen and taller than the band, and the sky comes out nearly empty however
        // full it was asked to be.
        // Two octaves rather than three: a third layer of detail breaks a cloud into
        // wisps, and what is wanted is a shape with an edge.
        .shape = {.frequency = 4.2f, .octaves = 2, .aspect = 2.2f, .seed = 5501},
        .wind  = 18.0f,

        // A front spans four or five screens and takes a couple of minutes to pass,
        // so a spell of weather is something to shelter from rather than something
        // that flickers.
        .front     = {.frequency = 0.18f, .octaves = 2, .seed = 5502},
        .frontWind = 22.0f,

        // A third of the sky on an average day, and the front pushes that further
        // than the climate does: where you are should colour the weather, not
        // decide it, or half the world would never see rain at all.
        // Just over a quarter of the sky on an average day. A column counts as shaded
        // if cloud stands anywhere in the band above it, so the share of ground in
        // shadow comes out around half again this — a third of it, which is what a
        // partly cloudy day looks like.
        .cover             = 0.28f,
        .frontInfluence    = 0.40f,
        .humidityInfluence = 0.22f,
        .softness          = 0.13f,

        // Rain is not rolled for. It is what a sky this full does.
        .rainAt   = 0.52f,
        .rainFull = 0.82f,

        // Chosen against the exposure curve rather than by the look of the number:
        // this is a quarter darker than open sky on screen, which reads as a shadow
        // sweeping the ground. Half of it would be 4% darker and read as nothing at
        // all, which is what it did.
        .shade = 0.78f,

        // Grey enough to read against a bright sky, and much darker with rain in
        // it. The light pass dims a cloud by its own shade on the way to the
        // screen, which is not how a cloud is lit but is exactly how one looks.
        .tint     = {222, 230, 245, 255},
        .rainTint = {126, 136, 158, 255},
        .rainLine = {168, 190, 222, 255},
    };

    World world(settings, config::kResolution);
    world.SetWeather(sky);

    // Dropped in above the ground at the origin rather than at a fixed height,
    // since the surface there is now wherever the relief put it.
    Player player({0.0f, terrain::Height(0.0f, settings) - 96.0f});
    Hotbar hotbar;
    Editor editor;

    LiquidLayer liquids;

    LightLayer lights;

    Camera2D camera = {};
    camera.offset   = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = 1.0f;

    float accumulated = 0.0f;
    float lantern     = config::kLanternStrength;

    debug_view::Toggles debug;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        // The frame can change size between any two frames, so the two things that
        // are sized to it are set from it every frame rather than when it changes.
        // Nothing then has to notice a resize, and there is no path where something
        // was told about one and something else was not.
        camera.offset = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f + GetScreenHeight() / 4.0f};
        liquids.Fit(GetScreenWidth(), GetScreenHeight());

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

        // Drifted before the light is solved, because the shade the cloud casts is
        // read during the solve. Advancing it afterwards would light every frame by
        // the sky of the frame before it, which nothing would look wrong about and
        // which would be wrong.
        world.StepWeather(dt);

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
