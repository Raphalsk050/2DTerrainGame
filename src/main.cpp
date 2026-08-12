#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "grove.h"
#include "hotbar.h"
#include "light_layer.h"
#include "liquid_layer.h"
#include "player.h"
#include "sod.h"
#include "soil.h"
#include "raylib.h"
#include "terrain.h"
#include "weather.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr const char *kSeasonNames[] = {"spring", "summer", "autumn", "winter"};

// Seconds a refusal stays on screen.
constexpr float kNoticeTime = 2.2f;

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
//
// Wider than a chunk, which is what the promise above actually requires and what
// a hundred and twenty-eight pixels did not deliver: a chunk is a hundred and
// ninety-two, so at the old figure a chunk could be created and scroll into view
// without a single vertex of it ever having been stepped.
//
// It also has to stay inside the band World::Update generates, or StepWater reads
// through to freshly generated noise for the parts outside it — see the reserve
// there, which is sized against this.
constexpr float kSimulationMargin = 256.0f;

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

// A line of the readout, dark with a pale edge under it.
//
// Grey on its own was unreadable, and measurably so rather than as a matter of
// taste: the daytime sky at the top of the screen sits at very nearly the same
// brightness as GRAY does, so the text and the background were the same value and
// only the letter shapes separated them. Black alone fixes the day and breaks the
// night, which is the same fault the other way up. The shadow is what makes one
// colour work against a bright sky, a dark one, wet rock and open water alike,
// because it supplies the contrast the background refuses to.
//
// Drawn twice. That is the whole cost, on six lines of text.
void DrawLabel(const char *text, int x, int y, Color colour) {
    // Outlined on all four sides rather than given a shadow on one.
    //
    // This was dark ink with a pale offset a pixel down and right, and it failed
    // at both ends of the world it has to be read against. Over white cloud the
    // pale half vanished into the cloud and what was left was thin dark letters;
    // in a cave the dark half vanished and what was left was the offset. And a
    // single offset is not an outline — half of every letter has nothing between
    // it and the background at all.
    //
    // Light letter, dark outline, and the two cover each other's ground: the
    // letter carries a cave and the outline carries a cloud. Five draws on six
    // lines of text, which is nothing next to a screenful of terrain.
    constexpr Color kEdge = {8, 10, 14, 235};

    DrawText(text, x - 1, y, 14, kEdge);
    DrawText(text, x + 1, y, 14, kEdge);
    DrawText(text, x, y - 1, 14, kEdge);
    DrawText(text, x, y + 1, 14, kEdge);

    DrawText(text, x, y, 14, colour);
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

// One strip of the world drawn straight to a file, at one screen pixel per world
// pixel.
//
// The offline probe this project was tuned with lived outside the repository and
// went with it. This is its replacement, and it is inside the game on purpose:
// what it draws is the world these settings describe rather than a copy of them
// kept in step by hand, and a probe that has to be maintained alongside what it
// measures is a probe that will one day be measuring something else.
//
// Drawn unlit, for the same reason F6 exists — what a material's own colour is
// doing and what the light is doing to it are two questions, and answering them
// together is how a palette gets tuned against a time of day.
void DrawProbe(World &world, Grove &grove, Harvest &gathered, Rectangle strip, const char *path, int zoom,
               float seconds, bool plants, int lit) {
    // Run the clock on before drawing, so a still picture can be taken of a world
    // that has been blowing for a while. The sway and the gust are both pure
    // functions of this clock, so two probes a second apart are two frames of the
    // same wind rather than two unrelated pictures.
    // Always at least one step, whatever was asked for: what the sky is giving
    // off is worked out by StepWeather and is nothing until it has run, so a
    // probe that skipped it would draw every lit picture at midnight.
    world.StepWeather(1.0f / 60.0f);

    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) world.StepWeather(1.0f / 60.0f);

    world.Update(strip);

    RenderTexture2D canvas = LoadRenderTexture(static_cast<int>(strip.width), static_cast<int>(strip.height));

    Camera2D camera = {};
    camera.offset   = {0.0f, 0.0f};
    camera.target   = {strip.x, strip.y};
    camera.zoom     = 1.0f;

    BeginTextureMode(canvas);

    // The sky's own colour, so the silhouette of the ground reads against
    // something rather than against black.
    ClearBackground({92, 132, 176, 255});

    // The plants too, and in the order the frame draws them, because half the
    // things worth checking here are about whether two of these agree with each
    // other about where the ground is.
    if (plants) grove.Update(world, strip, {strip.x, strip.y}, world.Sky().Time(), 1.0f / 60.0f, gathered);

    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    BeginMode2D(camera);
    world.DrawTerrain(strip);
    sod::DrawTufts(world.Grass(), strip, world.Sky().Time(), world.Settings().seed);

    // Left out on request, so that a check for something drawn clear of the
    // ground has only the ground and the grass to look at. A fern is full of
    // holes by design, and any test that cannot tell one of those from a tuft
    // hanging in the air will report the wrong thing every time.
    if (plants) grove.Draw(world.Sky(), season, world.Sky().Time());

    world.DrawLiquids(strip);

    // And the light over all of it, which is the other half of every question
    // about how dark something came out: what a material's own tones do and what
    // the multiply does to them are two answers, and tuning either while looking
    // at both together is how a palette ends up fighting a time of day.
    if (lit != 0) {
        // The canopies first, exactly as the frame does it. Shade is re-offered
        // every frame and is gone the moment nobody offers it, so a probe that
        // skipped this would be measuring a world with no woods in it.
        if (plants) grove.Shade(world, world.Sky().Time());

        world.StepLight(strip);

        // One draws the world as it is seen; the other draws the light on its
        // own, one flat block per probe. The second is the one to reach for when
        // the question is where the light is rather than what it is doing to a
        // palette, because a picture of the two multiplied together cannot answer
        // either of them on its own.
        if (lit == 2) {
            debug_view::DrawLight(world, strip);
        } else {
            LightLayer probeLight;
            probeLight.Update(world.Light());
            probeLight.Compose();
        }
    }

    EndMode2D();

    EndTextureMode();

    Image image = LoadImageFromTexture(canvas.texture);

    // A render texture is filled bottom row first, so what comes back off it is
    // upside down.
    ImageFlipVertical(&image);

    // Nearest neighbour, because the whole subject is where one texel ends and
    // the next begins. Anything that filters would answer a question about the
    // tones by inventing colours that are not in the ramp.
    if (zoom > 1) ImageResizeNN(&image, image.width * zoom, image.height * zoom);

    ExportImage(image, path);

    UnloadImage(image);
    UnloadRenderTexture(canvas);
}

// What the covers actually do over a stretch of world, measured rather than
// guessed.
//
// A cover's extent is decided by a climate bell, and a bell is very easy to
// author into a material that is simply never there: nothing errors, nothing is
// drawn, and the only symptom is a world with no deserts in it. This walks the
// columns and reports the share of them each cover claims and how thick it gets,
// which is the same treatment terrain::Calibrate gives cave coverage and for the
// same reason.
void ReportCovers(const terrain::Settings &settings, float fromX, float toX, float step) {
    struct Tally {
        double thickness = 0.0;
        float deepest    = 0.0f;
        float where      = 0.0f;
        int columns      = 0;
    };

    std::array<Tally, kElementCount> tally{};

    float coldest = 1.0f;
    float hottest = 0.0f;
    float driest  = 1.0f;
    float wettest = 0.0f;

    int columns = 0;

    for (float x = fromX; x <= toX; x += step, columns++) {
        const terrain::Climate climate = terrain::ClimateAt(x, settings);

        coldest = std::min(coldest, climate.temperature);
        hottest = std::max(hottest, climate.temperature);
        driest  = std::min(driest, climate.humidity);
        wettest = std::max(wettest, climate.humidity);

        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementSpawn &spawn = kElements[e].spawn;
            if (spawn.generator != Generator::Cover) continue;

            const float thickness = CoverThickness(spawn, x, climate.temperature, climate.humidity);

            // A cover thinner than one terrain pixel is not on the ground, it is
            // a rounding error, and counting it would report a desert nobody can
            // see.
            if (thickness < config::kPixelSize) continue;

            Tally &into = tally[e];

            into.thickness += thickness;
            into.columns++;

            if (thickness > into.deepest) {
                into.deepest = thickness;
                into.where   = x;
            }
        }
    }

    std::printf("%d columns over %.0f px, every %.0f\n", columns, toX - fromX, step);
    std::printf("temperature %.2f..%.2f   humidity %.2f..%.2f\n\n", coldest, hottest, driest, wettest);

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].spawn.generator != Generator::Cover) continue;

        const Tally &t = tally[e];

        if (t.columns == 0) {
            std::printf("%-6s  never\n", kElements[e].name);
            continue;
        }

        std::printf("%-6s  %5.1f%% of columns   mean %4.1f px   deepest %4.1f px at x %.0f\n", kElements[e].name,
                    100.0 * t.columns / std::max(columns, 1), t.thickness / t.columns, t.deepest, t.where);
    }
}

// How each material's paint divides itself, measured rather than argued.
//
// Three numbers per material, all in ramp steps, and each one answers a rule the
// art has to obey:
//
//  - `form` is how far apart a lit top face and a shaded underside come out. This
//    is the term that has to carry the picture, and it is the only one allowed to
//    move a texel more than a step.
//  - `stipple` is how far apart two neighbouring texels come out *inside* the
//    material, where there is no face and the texture is all there is. The rule
//    is that this stays under one step: a stipple breaks the boundary between two
//    tones, and past a step it stops breaking a boundary and starts drawing one.
//  - `jumps` is the share of neighbouring pairs that land two whole tones apart,
//    which is what the eye reads as static rather than as texture. It is the
//    number that caught the first setting here: grain was authored at 0.72 steps,
//    which is under a step and looked reasonable in the table, but two neighbours
//    at opposite ends of it are 1.44 apart and a fifth of them crossed two tones.
void ReportTones() {
    constexpr int kAcross = 200;
    constexpr int kDown   = 60;

    std::printf("%-8s %7s %8s %8s %7s %7s\n", "", "form", "across", "down", "jump-x", "jump-y");

    for (const ElementDef &def : kElements) {
        const soil::Paint paint = soil::For(def, 0);

        // A face at its most extreme either way: the top of a ledge and the belly
        // of an overhang, both hard against the surface.
        const marching_squares::Texel top{{0.0f, 0.0f}, 0.0f, {0.0f, -1.0f}};
        const marching_squares::Texel under{{0.0f, 0.0f}, 0.0f, {0.0f, 1.0f}};

        const float form = (soil::Shading(paint, top).form - soil::Shading(paint, under).form) / soil::kStep;

        // Then the inside, where the form is nothing and the texture is the whole
        // of what is drawn. Sampled on the terrain's own texel grid, because that
        // is the grid the grain is quantised to and any other spacing would
        // measure a different picture from the one drawn.
        //
        // Both axes, and that is not symmetry for its own sake: the bedding term
        // is stretched flat by kStrataAspect, so it barely changes along a row and
        // changes fast down a column. Measured across only, it does not appear at
        // all — which is exactly the reading that would let a material be authored
        // into horizontal stripes and pass.
        std::vector<float> lit(static_cast<std::size_t>(kAcross) * kDown);

        for (int j = 0; j < kDown; j++) {
            for (int i = 0; i < kAcross; i++) {
                const Vector2 at = {static_cast<float>(i) * config::kPixelSize,
                                    static_cast<float>(j) * config::kPixelSize};

                lit[static_cast<std::size_t>(j) * kAcross + i] =
                    soil::Shading(paint, {at, kUnboundedDepth, {0.0f, -1.0f}}).Lit();
            }
        }

        double apart[2] = {0.0, 0.0};
        int pairs[2]    = {0, 0};
        int jumps[2]    = {0, 0};

        const auto compare = [&](float a, float b, int axis) {
            apart[axis] += std::fabs(a - b) / soil::kStep;
            pairs[axis]++;

            if (std::abs(static_cast<int>(a * kElementRamp) - static_cast<int>(b * kElementRamp)) >= 2) jumps[axis]++;
        };

        for (int j = 0; j < kDown; j++) {
            for (int i = 0; i < kAcross; i++) {
                const float here = lit[static_cast<std::size_t>(j) * kAcross + i];

                if (i > 0) compare(here, lit[static_cast<std::size_t>(j) * kAcross + i - 1], 0);
                if (j > 0) compare(here, lit[static_cast<std::size_t>(j - 1) * kAcross + i], 1);
            }
        }

        std::printf("%-8s %7.2f %8.2f %8.2f %6.1f%% %6.1f%%\n", def.name, form, apart[0] / std::max(pairs[0], 1),
                    apart[1] / std::max(pairs[1], 1), 100.0 * jumps[0] / std::max(pairs[0], 1),
                    100.0 * jumps[1] / std::max(pairs[1], 1));
    }
}

// Every material's field down one column, against the surface it is placed
// relative to.
//
// The question this answers is the one that cannot be answered by looking: two
// materials whose contours should meet on the same line, and a picture in which
// they plainly do not. A drawn edge is the field interpolated, thresholded and
// quantised onto texels, and any of those three can be where the discrepancy
// entered — so the field itself has to be readable on its own.
void ReportColumn(const World &world, const terrain::Settings &settings, float worldX, float fromY, float toY) {
    std::printf("x %.0f   surface y %.1f\n\n", worldX, terrain::Height(worldX, settings));

    std::printf("%8s %8s", "y", "depth");
    for (const ElementDef &def : kElements) std::printf(" %8s", def.name);
    std::printf("\n");

    for (float y = fromY; y <= toY; y += static_cast<float>(config::kResolution)) {
        std::printf("%8.0f %8.1f", y, terrain::Depth({worldX, y}, settings));

        for (std::size_t e = 0; e < kElementCount; e++) {
            const float value = world.ValueAt(static_cast<Element>(e), {worldX, y});

            // Printed as the distance from its own threshold, since that is what
            // decides whether the material is there and what the contour
            // interpolates through. Zero is the edge.
            std::printf(" %8.3f", value - kElements[e].threshold);
        }

        std::printf("\n");
    }
}

// The daylight reckoning down a run of columns.
//
// Prints, for each probe column, the first solid probe and what the two terms of
// the spread are giving it. What this is for is telling apart the two things that
// look identical on screen: a column the sun term never reached, and one it
// reached with a dark answer.
void ReportSun(const World &world, Rectangle region) {
    const light::Field &field = world.Light();

    std::printf("%6s %6s %8s %8s %8s\n", "col", "row", "solid", "depth", "sunlit");

    for (int i = 0; i < field.Cols(); i++) {
        int first = -1;

        for (int j = 0; j < field.Rows(); j++) {
            if (field.SolidAt(i, j) > 0.0f) {
                first = j;
                break;
            }
        }

        if (first < 0) continue;

        const Vector2 at = field.ProbePosition(i, first);
        if (at.x < region.x || at.x > region.x + region.width) continue;

        std::printf("%6.0f %6.0f %8.2f %8.1f %8.3f\n", at.x, at.y, field.SolidAt(i, first),
                    field.SunDepthAt(i, first), light::Luminance(field.SunlitAt(i, first)));
    }
}

// The surface, column by column, and how much it moves between one and the next.
//
// What this is looking for is roughness at the scale of a lattice column. The
// light is solved one probe per column, so a surface that jumps between
// neighbouring columns is a surface the light cannot follow smoothly however the
// solve is written — the spikes arrive already in the ground.
void ReportSurface(const terrain::Settings &settings, float fromX, float toX) {
    const float step = static_cast<float>(config::kResolution);

    double total = 0.0;
    int steps    = 0;

    float worst  = 0.0f;
    float wheres = 0.0f;

    int spikes = 0;

    float previous = terrain::Height(fromX, settings);

    for (float x = fromX + step; x <= toX; x += step, steps++) {
        const float here = terrain::Height(x, settings);
        const float move = std::fabs(here - previous);

        total += move;

        if (move > worst) {
            worst  = move;
            wheres = x;
        }

        // A step taller than one terrain texel is one the eye reads as an edge
        // rather than as a slope.
        if (move > config::kPixelSize) spikes++;

        previous = here;
    }

    std::printf("%d columns of %.0f px\n", steps, step);
    std::printf("mean step %.2f px   worst %.1f px at x %.0f   over one texel: %.1f%%\n", total / std::max(steps, 1),
                worst, wheres, 100.0 * spikes / std::max(steps, 1));

    // Then a run of them, so the shape of the roughness can be read rather than
    // summarised.
    std::printf("\n%8s %9s %8s\n", "x", "height", "step");

    previous = terrain::Height(fromX, settings);

    for (float x = fromX + step; x <= fromX + step * 40.0f; x += step) {
        const float here = terrain::Height(x, settings);

        std::printf("%8.0f %9.2f %8.2f\n", x, here, here - previous);

        previous = here;
    }
}

// What the cave settings actually carve, measured rather than argued.
//
// Four questions, and each is a way the underground can be wrong without looking
// wrong in any one picture:
//
//  - **How much of the rock is gone.** Every layer adds into the same union and
//    none of them can see the others, so the total is the one figure no single
//    setting is responsible for and the one easiest to move by accident.
//  - **How much of it can be reached from the sky.** The one that matters most
//    and the one that cannot be seen from inside the game: a sealed pocket looks
//    exactly like a cave right up until there is no way out of it, and the rarer
//    the caves are the more each sealed one costs.
//  - **How tall the passages are.** The character is 26 px standing and 14
//    crouched, so a passage is walked, crawled, or is scenery — and which of the
//    three cannot be read off a width setting, because a corridor's own width is
//    measured across the corridor and not against gravity.
//  - **How often the surface opens.** An entrance nobody finds is the same as no
//    cave at all.
//
// Sampled on the lattice the world is stored on, because that is the grid the
// contour is drawn from: anything finer would report passages the marching
// squares cannot represent, and anything coarser would miss the ones it can.
void ReportCaves(const terrain::Settings &settings, Rectangle region) {
    const float step = static_cast<float>(config::kResolution);

    const int cols = static_cast<int>(region.width / step);
    const int rows = static_cast<int>(region.height / step);

    if (cols < 2 || rows < 2) {
        std::printf("region too small: %d x %d cells\n", cols, rows);
        return;
    }

    const auto count = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    const auto index = [cols](int i, int j) { return static_cast<std::size_t>(j) * cols + i; };

    // The whole region held at once. The flood fill has to see it as one
    // connected thing, and a streaming version would have to keep the frontier
    // anyway, which is most of the memory with none of the simplicity.
    std::vector<unsigned char> solid(count);
    std::vector<float> depth(count);

    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            const Vector2 at = {region.x + static_cast<float>(i) * step, region.y + static_cast<float>(j) * step};

            // Both numbers from one call, since the surface is much the most
            // expensive part of either and SampleGround already pays for it once.
            const terrain::Ground ground = terrain::SampleGround(at, settings);

            depth[index(i, j)] = ground.depth;
            solid[index(i, j)] = (ground.density > terrain::kSurfaceLevel) ? 1 : 0;
        }
    }

    // Volume, in bands measured from the surface rather than from an absolute Y.
    // Depth is what every cave layer is written against, and a band of absolute
    // height would mix the underside of a mountain with the open air beside it.
    constexpr int kBands      = 8;
    constexpr float kBandSpan = 450.0f;

    std::array<long, kBands> bandRock{};
    std::array<long, kBands> bandVoid{};
    std::array<long, kBands> bandOpen{};

    const auto bandOf = [&](std::size_t c) { return std::min(static_cast<int>(depth[c] / kBandSpan), kBands - 1); };

    for (std::size_t c = 0; c < count; c++) {
        if (depth[c] <= 0.0f) continue;

        if (solid[c] != 0) {
            bandRock[bandOf(c)]++;
        } else {
            bandVoid[bandOf(c)]++;
        }
    }

    // Then what the sky can get to, by flooding from the open air above the
    // ground rather than from the top edge of the region. Seeding from the edge
    // would call a cave unreachable purely because the rectangle was cropped
    // above the hill it opens on.
    std::vector<unsigned char> reached(count, 0);
    std::vector<int> frontier;

    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            const std::size_t c = index(i, j);

            if (solid[c] != 0 || depth[c] > 0.0f || reached[c] != 0) continue;

            reached[c] = 1;
            frontier.push_back(static_cast<int>(c));
        }
    }

    const auto spread = [&](std::vector<int> &stack, std::vector<unsigned char> &seen, unsigned char mark) {
        long size = 0;

        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            size++;

            const int i = c % cols;
            const int j = c / cols;

            const auto visit = [&](int ni, int nj) {
                if (ni < 0 || ni >= cols || nj < 0 || nj >= rows) return;

                const std::size_t n = index(ni, nj);
                if (solid[n] != 0 || seen[n] != 0) return;

                seen[n] = mark;
                stack.push_back(static_cast<int>(n));
            };

            visit(i - 1, j);
            visit(i + 1, j);
            visit(i, j - 1);
            visit(i, j + 1);
        }

        return size;
    };

    spread(frontier, reached, 1);

    for (std::size_t c = 0; c < count; c++) {
        if (depth[c] > 0.0f && solid[c] == 0 && reached[c] != 0) bandOpen[bandOf(c)]++;
    }

    // Whatever open rock the sky never arrived at is a pocket. Their sizes are
    // worth more than their number: a hundred single cells left by the roughness
    // term are noise, and one pocket the size of a room is a cave nobody can
    // enter.
    std::vector<unsigned char> pocket(count, 0);
    std::vector<long> pockets;

    long sealed = 0;

    for (std::size_t c = 0; c < count; c++) {
        if (solid[c] != 0 || reached[c] != 0 || pocket[c] != 0 || depth[c] <= 0.0f) continue;

        pocket[c] = 1;

        std::vector<int> stack{static_cast<int>(c)};
        const long size = spread(stack, pocket, 1);

        sealed += size;
        pockets.push_back(size);
    }

    // Size a sealed void stops being a fault in the field and starts being a cave
    // nobody can enter.
    //
    // A cave network in two dimensions cannot be both sparse and wholly connected
    // — corridors only join where they happen to cross, and thinning them thins
    // the crossings faster than the corridors. So the figure worth holding is not
    // how much of the void the sky reaches but how much of it is *worth* reaching
    // and does not: a hundred cells of blind crack behind a wall is rock with a
    // hole in it, and two thousand cells is a hall the player will never see.
    //
    // Two hundred cells is about eight hundred pixels across if it were square,
    // which is a screen's width of cave.
    constexpr long kWorthFinding = 200;

    long lost      = 0;
    long lostCount = 0;
    long largest   = 0;

    for (const long size : pockets) {
        largest = std::max(largest, size);

        if (size < kWorthFinding) continue;

        lost += size;
        lostCount++;
    }

    long undergroundVoid = 0;
    long undergroundRock = 0;

    for (int band = 0; band < kBands; band++) {
        undergroundVoid += bandVoid[band];
        undergroundRock += bandRock[band];
    }

    // Clearance, per unbroken vertical run of open cells. Per run and not per
    // cell, because what decides whether a passage is walked is the height of the
    // passage, and a tall chamber would otherwise vote once for every cell in it.
    //
    // Only runs wholly under the crust are counted. A run that reaches open air
    // has no ceiling, and averaging the sky into the passage heights is how a
    // world of crawlways reports itself as walkable.
    std::vector<float> runs;

    for (int i = 0; i < cols; i++) {
        int open      = 0;
        bool grounded = true;

        for (int j = 0; j < rows; j++) {
            const std::size_t c = index(i, j);
            const bool air      = solid[c] == 0;

            if (air && depth[c] > settings.caves.crust) {
                open++;
                continue;
            }

            // A run ends at rock, and is thrown away if it ended by running out
            // of crust instead — that one is a cave mouth, not a passage.
            if (open > 0 && grounded) runs.push_back(static_cast<float>(open) * step);

            grounded = !air;
            open     = 0;
        }
    }

    std::sort(runs.begin(), runs.end());

    const auto share = [](long part, long whole) { return 100.0 * static_cast<double>(part) / std::max(whole, 1L); };

    std::printf("region %.0f x %.0f px at (%.0f, %.0f)   %d x %d cells of %.0f px\n\n", region.width, region.height,
                region.x, region.y, cols, rows, step);

    std::printf("volume\n");
    std::printf("%14s %9s %12s %12s\n", "depth", "void", "of it open", "cells");

    for (int band = 0; band < kBands; band++) {
        const long total = bandRock[band] + bandVoid[band];
        if (total == 0) continue;

        const auto from = static_cast<int>(band * kBandSpan);

        if (band == kBands - 1) {
            std::printf("%9d+     %8.1f%% %11.1f%% %12ld\n", from, share(bandVoid[band], total),
                        share(bandOpen[band], bandVoid[band]), total);
        } else {
            std::printf("%9d-%-5d %8.1f%% %11.1f%% %12ld\n", from, static_cast<int>((band + 1) * kBandSpan),
                        share(bandVoid[band], total), share(bandOpen[band], bandVoid[band]), total);
        }
    }

    std::printf("%14s %8.1f%% %11.1f%% %12ld\n\n", "all rock", share(undergroundVoid, undergroundVoid + undergroundRock),
                share(undergroundVoid - sealed, undergroundVoid), undergroundVoid + undergroundRock);

    std::printf("connectivity\n");
    std::printf("  reachable from the sky  %6.1f%% of the void\n", share(undergroundVoid - sealed, undergroundVoid));
    std::printf("  sealed                  %6.1f%% in %zu pockets, largest %ld cells (%.0f px across if square)\n",
                share(sealed, undergroundVoid), pockets.size(), largest,
                std::sqrt(static_cast<double>(largest)) * step);
    std::printf("  caves lost              %6.1f%% of the void in %ld sealed voids over %ld cells\n\n",
                share(lost, undergroundVoid), lostCount, kWorthFinding);

    if (runs.empty()) {
        std::printf("clearance\n  no passages under the crust\n\n");
    } else {
        double sum   = 0.0;
        long upright = 0;
        long crouch  = 0;

        // Counted by area as well as by passage, because the two answer different
        // questions and the first is easy to read as the second. Half the passages
        // being too low to stand in sounds like a world of crawlways, but a
        // passage is a run of any length and the roughness leaves a great many
        // slivers a texel deep in the wall of a hall. What the player is actually
        // in most of the time is the share of the *space* that is stand-up-able.
        double standing = 0.0;
        double stooped  = 0.0;

        for (const float height : runs) {
            sum += height;

            if (height >= player_config::kHeight) {
                upright++;
                standing += height;
            }

            if (height >= player_config::kCrouchHeight) {
                crouch++;
                stooped += height;
            }
        }

        std::printf("clearance          (character %.0f px standing, %.0f crouched)\n", player_config::kHeight,
                    player_config::kCrouchHeight);
        std::printf("  %zu passages   median %.0f px   mean %.0f   tallest %.0f\n", runs.size(),
                    runs[runs.size() / 2], sum / static_cast<double>(runs.size()), runs.back());
        std::printf("  by passage: upright %5.1f%%  crouchable %5.1f%%\n",
                    share(upright, static_cast<long>(runs.size())), share(crouch, static_cast<long>(runs.size())));
        std::printf("  by space:   upright %5.1f%%  crouchable %5.1f%%\n\n", 100.0 * standing / std::max(sum, 1e-9),
                    100.0 * stooped / std::max(sum, 1e-9));
    }

    // The groundwater, on the same footing as the rock: what share of the open
    // space is under water, and how far the table tilts.
    //
    // The tilt is the figure that decides whether any of this works. A water
    // surface only stays where it is put if it is level, so the table has to move
    // by less than a lattice step across the width of a cave — measured here over
    // the widest passage found above, since that is the worst case in this world
    // rather than an assumed one.
    {
        // Width to judge the table's flatness over: a wide chamber, so the figure
        // is the worst a real cave would see rather than an average.
        constexpr float kCaveWidth = 300.0f;

        long wet     = 0;
        long steps   = 0;
        float across = 0.0f;

        for (int i = 0; i < cols; i++) {
            const float x                   = region.x + static_cast<float>(i) * step;
            const terrain::WaterTable table = terrain::TableAt(x, settings);

            if (i > 0 && table.level != terrain::TableAt(x - step, settings).level) steps++;

            // How far the surface moves across the width of a cave, which is the
            // whole of what decides whether it is level enough to stay put.
            across = std::max(across, std::fabs(table.level - terrain::TableAt(x - kCaveWidth, settings).level));

            for (int j = 0; j < rows; j++) {
                const std::size_t c = index(i, j);

                if (solid[c] == 0 && depth[c] > 0.0f && region.y + static_cast<float>(j) * step > table.level) wet++;
            }
        }

        std::printf("water\n");
        std::printf("  %.1f%% of the void is under the table   moves %.0f px across a %.0f px cave, worst\n",
                    share(wet, undergroundVoid), across, kCaveWidth);
        std::printf("  a step of %.0f px every %.0f px   (the lattice is %.0f)\n\n", settings.aquifer.step,
                    (steps > 0) ? region.width / static_cast<double>(steps) : region.width, step);
    }

    // The roughness field's own mid-point, which is the number RoughnessSettings
    // has to be told and cannot work out: the mean of a folded sum of octaves is
    // not the mean of one of them, and the analytic figure is well off what the
    // field does. Authoring it away from this puts every wall in the world that
    // many pixels out in the same direction.
    {
        const terrain::NoiseShape shape = settings.caves.roughness.shape;

        double folded = 0.0;
        long taken    = 0;

        for (int j = 0; j < rows; j += 3) {
            for (int i = 0; i < cols; i += 3) {
                folded += std::fabs(terrain::Signed({region.x + static_cast<float>(i) * step,
                                                     region.y + static_cast<float>(j) * step},
                                                    shape));
                taken++;
            }
        }

        std::printf("wall\n  folded roughness averages %.3f — RoughnessSettings::bias is authored at %.3f\n\n",
                    folded / std::max(taken, 1L), settings.caves.roughness.bias);
    }

    // And the mouths: columns where the sky itself reaches into the rock.
    //
    // Air within a short depth is not enough on its own, and the difference is
    // exactly what the entrance gate is for — a shaft held shut for its first
    // stretch and open below leaves a void under an unbroken lid, which is a cave
    // with no way in and reads on this test as a mouth unless the flood fill is
    // consulted. Asking whether the sky got there answers what is actually being
    // counted.
    const float lid = 40.0f;

    long mouthColumns = 0;
    long mouths       = 0;
    long widest       = 0;
    long run          = 0;

    for (int i = 0; i < cols; i++) {
        bool open = false;

        for (int j = 0; j < rows && !open; j++) {
            const std::size_t c = index(i, j);
            open = solid[c] == 0 && reached[c] != 0 && depth[c] > 0.0f && depth[c] <= lid;
        }

        if (open) {
            mouthColumns++;
            run++;
            widest = std::max(widest, run);
            continue;
        }

        if (run > 0) mouths++;
        run = 0;
    }

    if (run > 0) mouths++;

    std::printf("mouths             (sky reaching within %.0f px of the surface)\n", lid);

    if (mouths == 0) {
        std::printf("  none over %.0f px — the underground has no way in\n\n", region.width);
    } else {
        std::printf("  %ld over %.0f px, one every %.0f px   mean %.0f px wide, widest %.0f\n\n", mouths, region.width,
                    region.width / static_cast<double>(mouths), static_cast<double>(mouthColumns) / mouths * step,
                    static_cast<double>(widest) * step);
    }
}

// What exploring a cave is worth against digging blind, measured.
//
// The claim ElementSpawn::wallBias makes is that a passage pays better than the
// rock beside it, and that is a ratio between two numbers neither of which can
// be read off a setting: the share of the rock near a cave wall that holds a
// material, against the share of the rock well away from one. Both are wanted —
// too small a ratio and the caves are scenery, too large and the rock between
// them is not worth a pickaxe.
//
// Sampled through World::SpawnValue, which reproduces the whole chunk pipeline
// one vertex at a time, contest included. Anything cheaper would be measuring a
// different world from the one generated.
void ReportOre(const World &world, const terrain::Settings &settings, Rectangle region) {
    const float step = static_cast<float>(config::kResolution);

    std::array<long, kElementCount> nearWall{};
    std::array<long, kElementCount> deepRock{};

    long near = 0;
    long deep = 0;

    for (float y = region.y; y < region.y + region.height; y += step) {
        for (float x = region.x; x < region.x + region.width; x += step) {
            const Vector2 at = {x, y};

            const terrain::Ground ground = terrain::SampleGround(at, settings);
            if (ground.depth <= 0.0f || ground.density <= terrain::kSurfaceLevel) continue;

            // How far into the rock this is, in the pixels the bias is written
            // in. The reach is read from the table rather than assumed, since it
            // is the distance the claim is about.
            const bool wall = ground.solid <= kElements[ElementIndex(Element::Coal)].spawn.wallReach;

            if (wall) {
                near++;
            } else {
                deep++;
            }

            for (std::size_t e = 0; e < kElementCount; e++) {
                if (kElements[e].spawn.generator != Generator::Vein) continue;
                if (world.SpawnValue(static_cast<Element>(e), at) <= kElements[e].threshold) continue;

                if (wall) {
                    nearWall[e]++;
                } else {
                    deepRock[e]++;
                }
            }
        }
    }

    std::printf("%ld cells of rock within reach of a wall, %ld well inside it\n\n", near, deep);
    std::printf("%-9s %10s %12s %10s %8s\n", "", "bias", "at a wall", "in rock", "times");

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementSpawn &spawn = kElements[e].spawn;
        if (spawn.generator != Generator::Vein) continue;

        const double atWall = 100.0 * nearWall[e] / std::max(near, 1L);
        const double inRock = 100.0 * deepRock[e] / std::max(deep, 1L);

        std::printf("%-9s %10.3f %11.3f%% %9.3f%% %8.1f\n", kElements[e].name, spawn.wallBias, atWall, inRock,
                    (inRock > 0.0) ? atWall / inRock : 0.0);
    }
}

// How far the liquid moves after being generated, which is the whole of whether
// the world describes water at rest or water about to fall.
//
// A pool laid down as a shape the automaton disagrees with collapses the moment
// it is stepped, and it does it again every time the chunk is rebuilt — so what
// the player sees is water that has moved every time they walk back into a
// place. The fix is not to hold the liquid in memory; it is for the generated
// state to be the settled one. This measures whether it is.
//
// Reported as mass moved against mass present, so it reads as a share and does
// not depend on how much water the region happened to contain.
void ReportSettling(World &world, Rectangle region, int steps) {
    world.Update(region);

    const float before = world.TotalWater(region);

    // A snapshot of every vertex, so that movement can be measured rather than
    // inferred from the total — the automaton conserves mass exactly, so the
    // total is unchanged whether nothing moved or everything did.
    const float span = static_cast<float>(config::kResolution);

    std::vector<float> was;

    for (float y = region.y; y < region.y + region.height; y += span) {
        for (float x = region.x; x < region.x + region.width; x += span) {
            was.push_back(world.ValueAt(Element::Water, {x, y}));
        }
    }

    for (int i = 0; i < steps; i++) world.StepWater(region);

    double moved = 0.0;
    double held  = 0.0;

    std::size_t k = 0;

    for (float y = region.y; y < region.y + region.height; y += span) {
        for (float x = region.x; x < region.x + region.width; x += span, k++) {
            const float now = world.ValueAt(Element::Water, {x, y});

            moved += std::fabs(now - was[k]);
            held += was[k];
        }
    }

    std::printf("region %.0f x %.0f at (%.0f, %.0f), %d steps\n\n", region.width, region.height, region.x, region.y,
                steps);
    std::printf("water present   %.1f units over %zu vertices\n", before, was.size());
    std::printf("water moved     %.1f units, %.2f%% of what was there\n", moved, 100.0 * moved / std::max(held, 1e-9));
    std::printf("total after     %.1f units   (the automaton conserves mass, so this is a check on itself)\n",
                world.TotalWater(region));
}

void Draw(const World &world, const Grove &grove, const Harvest &gathered, const Player &player,
          const Hotbar &hotbar, const Editor &editor, const LiquidLayer &liquids, const LightLayer &lights,
          const Camera2D &camera, const debug_view::Toggles &debug, float lantern, const char *notice,
          float noticeFor) {
    const Rectangle view = ViewBounds(camera);

    BeginDrawing();

    BeginMode2D(camera);

    // The air first, filling the frame. It replaces clearing it rather than being
    // drawn over a cleared one: there is no height at which the sky is not some
    // colour, so there is nothing for a background to be.
    world.Sky().DrawAtmosphere(view);

    // Then the cloud standing in it, and then the ground over both. Underground the
    // band is out of view and this returns having done nothing.
    world.Sky().DrawClouds(view, world.Spacing());

    world.DrawTerrain(view);

    // The tufts on top of the band the terrain drew, and under everything that
    // stands in them: a trunk belongs in front of the grass around its own foot,
    // which is the reason the ferns are drawn before the trees as well.
    //
    // On the weather clock, so the sway runs with the wind that drives it and
    // both speed up together under F7.
    sod::DrawTufts(world.Grass(), view, world.Sky().Time(), world.Settings().seed);

    // The plants over the ground and behind the character, and on this side of
    // the light multiply: a tree is lit by the same daylight as the ground it
    // stands on, and has to know nothing about it to be.
    // Whatever time of year it is. There is no calendar yet, so this is spring
    // unless F9 is holding one — see weather::Sky::Turn.
    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    grove.Draw(world.Sky(), season, world.Sky().Time());

    grove.DrawFruit(season, world.Sky().Time());
    grove.DrawLeaves(world.Sky(), season, view, world.Sky().Time());

    // What the wood left on the ground, over the plants and under the character.
    grove.Fallen().Draw(world.Sky().Time());

    player.Draw();

    // Rain in front of the world rather than behind it, so it falls past a cliff
    // face instead of behind one. Still inside the light, because rain in an unlit
    // place should not be the one bright thing on screen.
    //
    // Asked of the world and not of the sky: a drop stops at the first thing under
    // it, and what is under it is the world's to know.
    world.DrawRain(view);

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

    // The stars go on this side of that line, and they are the only part of the
    // world that does.
    //
    // A star is a light rather than something lit, and the multiply cannot express
    // one: under it nothing may come out brighter than the sky's own radiance, which
    // at midnight is a tenth — so every star was a grey smudge and its colour went
    // with its brightness. Out here it keeps both. What it costs is that the ground
    // and the cloud no longer hide a star by being drawn over it, so both are asked
    // instead.
    world.DrawStars(view);

    // Drawn over the world rather than under it: the point of an overlay is to
    // check the world against what produced it, which is impossible while the
    // world covers it.
    if (debug.vertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);
    if (debug.layers) debug_view::DrawLayers(world, view);
    if (debug.chunks) debug_view::DrawChunks(world, view);
    if (debug.light) debug_view::DrawLight(world, view);

    editor.DrawCursor(hotbar, camera);

    EndMode2D();

    // Outside the camera transform, unlike every other overlay: what it shows is
    // the image that was baked, not a place in the world.
    if (debug.atlas) grove.DrawSheet();

    // Near white, because the outline under it is what does the separating now.
    const Color ink = {238, 243, 250, 255};

    // And the wet lines keep their meaning by going pale blue rather than dark.
    const Color wet = {152, 206, 255, 255};

    DrawLabel("A/D: move  |  space: jump  |  S: crouch  |  J: chop  |  P: plant  |  mouse: aim  |  F: fly", 10,
              10, ink);
    DrawLabel("left: apply brush  |  X: place/dig  |  1-0 or wheel: material  |  - / +: brush size  |  R: regenerate",
              10, 28, ink);
    DrawLabel(TextFormat("V: vertices  |  F3: chunks  |  F4: height grid  |  F5: light probes  |  F6: unlit %s  |"
                         "  F7: fast weather %s  |  F8: next quarter  |  F9: season %s  |  F10: sheet  |  , . : lantern %.1f",
                         debug.unlit ? "on" : "off", debug.fastWeather ? "on" : "off", kSeasonNames[world.Sky().Turn().index],
                         lantern),
              10, 46, ink);

    DrawLabel(TextFormat("chunks: %d (%d pinned)   edits kept: %d   plants: %d (%d drawn, %d kept)   rays: %ld",
                         world.ResidentChunks(), world.PinnedChunks(), world.RememberedEdits(), grove.VisiblePlants(),
                         grove.DrawnPlants(), grove.RememberedPlants(), world.Light().Rays()),
              10, 70, ink);

    // What the woods have given up.
    //
    // Every kind, including the ones at zero. Hiding an empty count reads as the
    // item not existing rather than as not having any, and the first thing a
    // player asks when planting will not work is whether they have a sapling at
    // all — a question the readout has to be able to answer.
    {
        const char *line = "gathered:";

        for (std::size_t i = 0; i < kItemCount; i++) {
            line = TextFormat("%s  %s %d", line, kItems[i].name, gathered[i]);
        }

        DrawLabel(TextFormat("%s   (%d on the ground)", line, grove.Fallen().Live()), 10, 142, ink);
    }

    const Vector2 centre = player.Centre();
    const auto under     = editor.Under();

    DrawLabel(TextFormat("y: %d   under cursor: %s   light here: %.2f   light at cursor: %.2f",
                         static_cast<int>(centre.y), under.has_value() ? Def(*under).name : "open",
                         world.LightLevelAt(centre), world.LightLevelAt(editor.Aim())),
              10, 88, ink);

    // The weather, and then the cloud standing in it. Read together they show the
    // chain working: the weather sets the level, the sky over this spot fills to it,
    // the shadow follows the cloud that casts it, and the rain leaves from the
    // underside of that same cloud rather than from a height of its own.
    const weather::Weather &sky = world.Sky().Now();

    DrawLabel(
        TextFormat("weather: %-8s  sky %.0f%% full   rain %.0f%%   |   here: %.0f%% cloud  %.0f%% shade  base y %d",
                   sky.name, sky.cover * 100.0f, sky.rain * 100.0f, world.Sky().CoverAt(centre.x) * 100.0f,
                   world.Sky().ShadeAt(centre.x) * 100.0f, static_cast<int>(world.Sky().UndersideAt(centre.x))),
        10, 106, sky.rain > 0.0f ? wet : ink);

    // Then the day, and how damp it has left the ground. The clock is the world's
    // own and not the wall's: a whole turn is `Day::dayMinutes` of weather time, so
    // it runs fast under F7 with everything else.
    const weather::Daylight &today = world.Sky().Today();

    const float hours  = today.phase * 24.0f;
    const float damp   = world.HumidityAt(centre);
    const int filled   = static_cast<int>(damp * 10.0f + 0.5f);
    const char *soaked = "..........";
    const char *dry    = "          ";

    DrawLabel(TextFormat("%02d:%02d  %-5s   daylight %.0f%%   |   humidity %.0f%%  [%.*s%.*s]", static_cast<int>(hours),
                         static_cast<int>((hours - std::floor(hours)) * 60.0f), today.name, today.light * 100.0f,
                         damp * 100.0f, filled, soaked, 10 - filled, dry),
              10, 124, damp > 0.66f ? wet : ink);

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

    // Whatever the world last refused to do, over the middle of the screen where
    // the eye already is after pressing a key that did nothing.
    if (noticeFor > 0.0f) {
        const int width = MeasureText(notice, 14);

        DrawLabel(notice, (GetScreenWidth() - width) / 2, GetScreenHeight() - 140, {255, 214, 140, 255});
    }

    DrawBrushMode(editor);
    hotbar.Draw(editor.Collected());

    EndDrawing();
}

} // namespace

int main(int argc, char **argv) {
    // `--probe x y w h out.png` draws one strip of the world to a file and exits.
    // See DrawProbe. Read here rather than after the window opens, so the probe
    // can ask for a hidden one: it wants a GL context and nothing else.
    const bool probing = argc >= 7 && TextIsEqual(argv[1], "--probe");

    // `--covers x0 x1 step` walks the columns and reports what each cover claims.
    // See ReportCovers.
    const bool counting = argc >= 5 && TextIsEqual(argv[1], "--covers");

    // `--tones` reports how each material's paint divides between form and
    // texture. See ReportTones.
    const bool weighing = argc >= 2 && TextIsEqual(argv[1], "--tones");

    // `--column x y0 y1` prints every material's field down one column. See
    // ReportColumn.
    const bool reading = argc >= 5 && TextIsEqual(argv[1], "--column");

    // `--caves x y w h` measures what the cave settings carve over a region. See
    // ReportCaves.
    const bool digging = argc >= 6 && TextIsEqual(argv[1], "--caves");

    // `--ore x y w h` measures what a cave wall is worth against blind rock. See
    // ReportOre.
    const bool assaying = argc >= 6 && TextIsEqual(argv[1], "--ore");

    // `--settle x y w h [steps]` measures how far generated liquid moves once it
    // is simulated. See ReportSettling.
    const bool settling = argc >= 6 && TextIsEqual(argv[1], "--settle");

    // Resizable, with a floor under it: below the minimum the hotbar is wider than
    // the frame and the head-up display runs off the side of it.
    SetConfigFlags((probing || counting || weighing || reading || digging || assaying || settling)
                       ? FLAG_WINDOW_HIDDEN
                       : FLAG_WINDOW_RESIZABLE);

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

                    // How steep the climb between two ledges is. Measured against
                    // the roughness of the ground itself — see the declaration.
                    .terraceSharp = 2.0f,

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
                    .crust     = 110.0f,
                    .crustFade = 72.0f,

                    // A tenth of the ground just under the crust is cave country,
                    // rising to half of it far below. That split is where the whole
                    // shape of the underground comes from: near the surface, where
                    // rarity is what is actually felt, most of the rock is rock and a
                    // cave is somewhere found; deep down it is generous, because that
                    // is where the volume belongs and because corridors only join
                    // where they cross — thin them out down there and the system
                    // stops being a system.
                    //
                    // Fifteen hundred pixels to go from the one to the other, which is
                    // about two screens of descent.
                    .region                = {.frequency = 0.6f, .octaves = 2, .seed = 4410},
                    .regionCoverage        = 0.42f,
                    .regionCoverageShallow = 0.10f,
                    .regionDeepens         = 1500.0f,
                    .regionFade            = 0.08f,

                    // The warp, and it is the difference between rock and vector
                    // art. Sixty pixels is a good share of the spacing between
                    // corridors, so a passage wanders by more than its own width
                    // over its length instead of running an arc; one feature every
                    // three hundred pixels, so it bends several times per screen.
                    // Three octaves, so it kinks at more than one scale.
                    .warp          = {.frequency = 3.2f, .octaves = 3, .seed = 4425},
                    .warpAmplitude = 64.0f,

                    // Two texels of flare where two layers meet. Enough to take the
                    // knife edge off a junction, small against the passages it joins.
                    .blend = 10.0f,

                    // The widenings the corridors run through. A twentieth of the
                    // eligible ground, two and a half character heights of headroom
                    // near the surface and three and a half well below it.
                    .chambers = {.shape         = {.frequency = 4.0f, .octaves = 2, .seed = 4411},
                                 .coverage      = 0.050f,
                                 .height        = 62.0f,
                                 .heightAtDepth = 92.0f,
                                 .growthFrom    = 300.0f,
                                 .growthTo      = 2000.0f,
                                 .rubble        = 14.0f},

                    // The great voids, and the reason to go down. A frequency low
                    // enough that one spans several screens, a share small enough
                    // that finding one is an event, and no height at all until well
                    // under the surface — the ramp is what makes the depth mean
                    // something rather than being somewhere the same caves repeat.
                    //
                    // Two hundred and forty pixels is over nine character heights,
                    // which is the point at which a room stops being a wide corridor
                    // and starts being somewhere with a roof out of reach.
                    .caverns = {.shape         = {.frequency = 1.1f, .octaves = 2, .seed = 4419},
                                .coverage      = 0.014f,
                                .height        = 0.0f,
                                .heightAtDepth = 240.0f,
                                .growthFrom    = 900.0f,
                                .growthTo      = 2600.0f,
                                .rubble        = 30.0f},

                    // The wall. One feature every forty pixels or so, moving it five
                    // pixels either way — a texel of the terrain grid, which is the
                    // smallest thing the outline can actually be drawn with.
                    .roughness = {.shape     = {.frequency = 24.0f, .octaves = 2, .seed = 4418},
                                  .amplitude = 5.0f,
                                  .bias      = 0.329f,
                                  .reach     = 20.0f},

                    // The halls: stretched three to one, so they run sideways and can
                    // be walked rather than fallen down. Wholly regional — outside
                    // cave country there are no halls at all, and that is where most
                    // of the rock the old settings hollowed out has gone back.
                    //
                    // The width is the width *before* the pinch takes fourteen off
                    // it, so what is carved runs from nothing where the girth field
                    // is low to some thirty-five pixels of half-width where it is
                    // high — a passage that closes to a squeeze in places and opens
                    // to a hall in others, rather than the parallel-sided pipe a
                    // single number gives. The girth field is stretched less than the
                    // halls themselves, so the width changes several times along one.
                    .galleries = {.shape        = {.frequency = 0.7f, .octaves = 3, .aspect = 3.0f, .seed = 4412},
                                  .width        = 36.0f,
                                  .widthAtDepth = 44.0f,
                                  .growthDepth  = 1400.0f,
                                  .girth        = {.frequency = 3.6f, .octaves = 2, .aspect = 2.0f, .seed = 4415},
                                  .swing        = 0.85f,
                                  .pinch        = 0.45f,
                                  .floor        = 0.18f},

                    // The links between the halls, and the one layer that survives
                    // dead rock. Less stretched, so they cut across the halls instead
                    // of running alongside them.
                    //
                    // At a floor of nearly half, a crawlway between two systems comes
                    // out around fourteen pixels of headroom — the character's
                    // crouched height exactly, so the way between one cave and the
                    // next is a squeeze on hands and knees. That is the cheapest
                    // connectivity there is: measured, the same guarantee carried on
                    // the halls cost four times the rock.
                    .crawlways = {.shape        = {.frequency = 1.4f, .octaves = 3, .aspect = 1.5f, .seed = 4413},
                                  .width        = 26.0f,
                                  .widthAtDepth = 32.0f,
                                  .growthDepth  = 1400.0f,
                                  .girth        = {.frequency = 5.0f, .octaves = 2, .aspect = 1.5f, .seed = 4416},
                                  .swing        = 0.80f,
                                  .pinch        = 0.28f,
                                  .floor        = 0.42f},

                    // The way in, and the way down. Stretched the other way, so it
                    // descends, and rare: roughly one mouth per screen and a half of
                    // travel. Rarity comes from the frequency and the aspect
                    // together, since a vertically stretched field has one curve per
                    // band of horizontal distance.
                    //
                    // It widens as it falls rather than narrowing, which is the one
                    // layer that does: a fissure is worked open from the bottom by
                    // whatever ran down it. Swung hard and not pinched at all — an
                    // entrance that closes partway down is an entrance to nothing,
                    // and this is the one layer whose whole job is to arrive
                    // somewhere.
                    //
                    // Stretched two and a half to one and no further, with an octave
                    // more than the layers it crosses. Pushed past that the field's
                    // zero set straightens into a set of near-parallel lines a fixed
                    // distance apart, and what the underground reads as then is not a
                    // cave system but a row of bars — the same fault as a corridor of
                    // constant width, stood on end.
                    .shafts = {.shape        = {.frequency = 0.40f, .octaves = 3, .aspect = 0.40f, .seed = 4414},
                               .width        = 22.0f,
                               .widthAtDepth = 26.0f,
                               .growthDepth  = 1800.0f,
                               .girth        = {.frequency = 2.5f, .octaves = 2, .aspect = 0.5f, .seed = 4417},
                               .swing        = 0.55f,
                               .floor        = 0.20f},

                    // Far past the crust, because clearing it is only half of what a
                    // shaft is for. Every other layer runs sideways, so this is the
                    // only thing in the world that carries a route from one depth to
                    // the next, and it has to reach the deep or the deep is sealed.
                    .mouthDepth = 96.0f,

                    .shaftReach = 4400.0f,
                },

            // The groundwater. A sixth of the open rock is under it, and all of
            // that is deep: the halls the player first walks into are dry, and
            // meeting water is arriving somewhere rather than the state of the
            // underground.
            .aquifer =
                {
                    // One feature spans some hundred and seventy thousand pixels,
                    // which is what a regional water table is — it stands higher in
                    // one part of a country than in another and is level everywhere
                    // in between. The slowness is not a style: it is what makes the
                    // snapping below rare, and the snapping is what makes the
                    // surface flat.
                    //
                    // Three thousand pixels down at the middle and twelve hundred of
                    // swing either way, so it runs between about eighteen hundred
                    // below the ground and four thousand. Over that range it crosses
                    // the caves at every height, which is the case worth having: a
                    // chamber with its floor under water and its roof in the air.
                    //
                    // Twelve pixels to the step — two of the lattice. Measured, one
                    // turns up every seven hundred pixels or so, and what a cave
                    // unlucky enough to contain one gets is a two-cell ledge in its
                    // surface, which settles in a few frames and is not visible
                    // doing it. Snapping to a coarser figure would put them four
                    // times further apart and make each one a waterfall.
                    .level = {.frequency = 0.006f, .octaves = 1, .seed = 4430},
                    .depth = 3000.0f,
                    .swing = 1200.0f,
                    .step  = 12.0f,
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
        // The air. Not a pair of colours to interpolate between: the two facts that
        // produce the gradient, and the gradient follows. Air thins with height and
        // blue scatters five times harder than red, which between them give the pale
        // band at the horizon, the deep blue overhead and the fade towards black
        // above that.
        .air =
            {
                // About half a screen, so the sky visibly deepens within one view
                // rather than only after climbing for a while.
                .scaleHeight = 320.0f,
                .thickness   = 3.5f,
                .rayleigh    = {0.52f, 1.15f, 2.60f},

                .overcast     = {150, 156, 168, 255},
                .overcastWash = 0.80f,
                .bandHeight   = 10.0f,
            },

        .stars =
            {
                // Seventy pixels apart puts a hundred-odd in a screen of sky, which
                // is enough to read as a sky and few enough that each one is a mark
                // rather than grain.
                .spacing = 70.0f,

                // Under the world's own pixel, which is five. A star at the size of
                // a terrain tile reads as a tile rather than as a light, and it is
                // the one thing here that is allowed off that lattice — nothing else
                // in the world is a point at an unreachable distance.
                .size = 3.0f,

                // A seventh of the world's motion. Enough that walking a screen
                // moves them a little and they sit behind the landscape rather than
                // on it; little enough that they are plainly a long way off.
                .parallax = 0.14f,

                // Swept clean for the first three hundred pixels above the ground.
                // A star sitting just over the treeline reads as a hole in the
                // picture rather than as a sky, and it is the first thing the eye
                // goes to because it is the part of the sky nearest the land.
                .rise = 320.0f,

                // Untouched through a clear or a fair sky — the clouds that are
                // there already hide what is behind them — and gone entirely by the
                // time the deck is closed, which is what a storm looks like from
                // underneath.
                .hideFrom = 0.55f,
                .hideAt   = 0.95f,

                // Faded across a cloud's outline rather than cut at it. The cloud is
                // rasterised from a lattice a dozen pixels across, so its drawn edge
                // and the field's exact one disagree over about a fiftieth of it —
                // and every one of those is a star left burning on the rim, which is
                // exactly where it gets noticed.
                .cloudEdge = 0.09f,

                // A fifth between the brightest and the faintest, and colour doing
                // the rest of the work. The full range reads as noise: the eye finds
                // the scatter before it finds the sky.
                .spread = 0.22f,
                .tint   = 0.70f,

                // Present but not restless. A seventh of the brightness, wavering a
                // little under twice a second — enough to be alive, little enough
                // that the field does not shimmer.
                .twinkle     = 0.15f,
                .twinkleRate = 1.7f,

                // Two ends of a colour temperature rather than one white, and both
                // well clear of grey: a star is one square against a nearly black
                // ground, and a tint that is merely suggested does not survive being
                // blended down.
                .hot  = {170, 202, 255, 255},
                .cool = {255, 186, 128, 255},
            },

        // How a cloud takes the light: Beer-Lambert with the powder term, per cell,
        // from that cell's own depth towards the sun. Nothing here takes the camera
        // as an input, which is the point.
        .shading =
            {
                .layers = 6,

                // Where the sun is comes from the day now, not from here. See
                // `Day::sunTilt` below: it is held off the vertical so the cloud
                // always keeps a lit flank, which is the whole difference between a
                // shaded shape and a lit one.
                .sunReach = 150.0f,

                // High, because the depth it is given is a field margin and those run
                // to about a third rather than to one. At two the darkest Beer term
                // was a half and every cloud sat in the top of the bands, uniformly
                // bright; this spreads them over the whole range.
                .absorption  = 7.0f,
                .powder      = 1.0f,
                .powderScale = 9.0f,

                // Short of the full range at both ends, so the brightest band stays
                // below pure sunlight and the darkest above pure ambient. A cloud
                // that reaches either is a cloud with a blown edge or a black hole in
                // it.
                .darkest  = 0.08f,
                .lightest = 0.92f,
            },

        .day =
            {
                // Long enough that a day is something lived through rather than
                // watched, and deliberately not a whole multiple of `spellMinutes`:
                // at four spells to a day every dawn would fall at the same point of
                // a spell for ever and the weather would never once break
                // differently over a sunrise. At 24 against 5 the two only realign
                // every fifth day.
                //
                // F7 runs this forty times faster along with the weather, which puts
                // a whole day in half a minute. F8 runs it on to the next quarter.
                .dayMinutes = 24.0f,

                // Eight in the morning, so a fresh world opens in full light with
                // the day ahead of it. Stated here rather than left to the default
                // because this is the block a reader comes to when asking what
                // time the game starts.
                .startAt = 8.0f / 24.0f,

                // Both below the horizon's own zero, so light arrives before the sun
                // does and outlasts it. Twelve minutes of full day, eight and a half
                // of full night, and about two minutes of turning at each end.
                //
                // Note what the top edge buys: the daylight is pinned at exactly one
                // for about half the cycle, so high noon is the sky this world had
                // before there was a clock in it, unchanged.
                .darkAt = -0.45f,
                .litAt  = 0.05f,

                // Half off vertical at noon. A sun straight overhead lights only the
                // tops of the clouds and takes the side off them.
                .sunTilt = 0.50f,

                // How much more air the light crosses along the horizon than
                // overhead. This one number is the sunset. Above about 0.8 the
                // middle of the sky passes through an olive on its way down, which
                // is real Rayleigh and reads as a bruise.
                .travel = 0.65f,

                // What a cloud can still hold back at midnight. Without it a storm
                // after dark is not dim, it is black.
                .nightShade = 0.15f,

                // How long the ground remembers a shower, and how fast it forgets.
                // A quarter of an hour back, halving every four minutes — so a storm
                // is still felt underfoot two spells of weather after it passed.
                .wetMinutes  = 15.0f,
                .wetHalfLife = 4.0f,

                // Both against a climate that already runs the whole range, so
                // neither may be large: a soaking should make a dry place damp, not
                // make every place the same.
                .wetGain = 0.55f,
                .dryGain = 0.30f,
            },

        // The weather this world has, and how long a spell of it lasts.
        //
        // Rain lives here and nowhere else. Minecraft, Terraria and Stardew all put
        // it here too: one sky for the whole world, on a timer. It is the only
        // arrangement in which it does not rain on one cloud and not the one beside
        // it, and it is also what makes a rainy sky overcast — the storm's `cover` is
        // that rule, rather than a second one written somewhere else.
        .moods =
            {
                {.name       = "clear",
                 .cover      = 0.14f,
                 .rain       = 0.0f,
                 .likelihood = 1.0f,
                 .sunlight   = {255, 252, 246, 255},
                 .ambient    = {150, 176, 214, 255},
                 .shade      = 0.85f},

                {.name       = "fair",
                 .cover      = 0.34f,
                 .rain       = 0.0f,
                 .likelihood = 1.6f,
                 .sunlight   = {255, 250, 240, 255},
                 .ambient    = {138, 162, 202, 255},
                 .shade      = 1.0f},

                {.name       = "overcast",
                 .cover      = 0.78f,
                 .rain       = 0.0f,
                 .likelihood = 0.8f,
                 .sunlight   = {228, 232, 240, 255},
                 .ambient    = {116, 128, 152, 255},
                 .shade      = 1.0f},

                // Most of the sky, and the only mood that rains.
                {.name       = "storm",
                 .cover      = 0.94f,
                 .rain       = 1.0f,
                 .likelihood = 0.55f,
                 .sunlight   = {176, 184, 200, 255},
                 .ambient    = {80, 88, 108, 255},
                 .shade      = 1.0f},
            },

        // A spell is minutes, not seconds: weather is something to shelter from, not
        // something that flickers. F7 runs the clock forty times faster for looking
        // at it.
        .spellMinutes = 5.0f,
        .crossMinutes = 1.2f,
        .seed         = 7717,

        // Well above the highest ground, which the relief puts at about y 20, so
        // cloud is always sky and never something that can be walked into.
        .ceiling  = -640.0f,
        .base     = -320.0f,
        .rainDrop = 100.0f,

        // The base shape. Frequency against aspect sets the size: this pair gives a
        // cloud about four hundred pixels across and a hundred and eighty deep, so a
        // couple are in view at once and each is comfortably shorter than the band it
        // floats in.
        .shape = {.frequency = 4.6f, .octaves = 3, .gain = 0.5f, .aspect = 2.3f, .seed = 5501},
        .wind  = 18.0f,

        // One feature of that shape is a little over two hundred pixels, so at this
        // a cloud has re-formed into a different cloud in about half a minute —
        // roughly the time the wind takes to carry it its own width. Slow enough
        // that watching one is watching it change rather than watching it flicker.
        .evolve = 6.0f,

        // The front, demoted to rippling the cover from place to place.
        .front     = {.frequency = 0.18f, .octaves = 2, .seed = 5502},
        .frontWind = 22.0f,

        // Both small. They texture the sky; they do not overrule the weather, or a
        // storm would have clear patches in it.
        .frontInfluence    = 0.14f,
        .humidityInfluence = 0.10f,

        // The lobes, at nearly three times the base frequency so they read as bumps on
        // the cloud rather than as the cloud itself.
        .lobes     = {.frequency = 13.0f, .octaves = 2, .aspect = 1.5f, .seed = 5504},
        .worleyMix = 0.38f,

        // A lobe cell is some seventy pixels tall, so a bump climbs through its own
        // cell in about fifteen seconds: turrets rolling up through the body of the
        // cloud. The sideways term is a sixth of the wind, which is a crawl and not a
        // slide.
        .lobeCrawl = {-3.0f, -5.0f},

        // Three times the base frequency, biting a fifth of the way in. This is the
        // rim of small lobes; without it the outline is a smooth swell.
        .detail      = {.frequency = 14.0f, .octaves = 2, .aspect = 1.6f, .seed = 5503},
        .erosion     = 0.20f,
        .erosionBand = 0.16f,

        // Twice the lobes and the other way along the wind, so the rim boils while
        // the body only rolls. This is the fastest thing in the sky and it is the
        // smallest, which is the right way round.
        .detailCrawl = {4.0f, -11.0f},

        .fieldStep = 2.0f,
        .softness  = 0.13f,
        .bandTaper = 0.85f,

        // Chosen against the exposure curve rather than by the look of the number:
        // this is a quarter darker than open sky on screen. Half of it would be 4%
        // darker and read as nothing at all.
        .shade = 0.78f,

        // Rain. The drop is mixed towards this and always ends up lighter than the
        // air behind it, so it reads against noon and against a storm alike — and
        // against a night, once there is one, without anything here being touched.
        .rainLine   = {216, 234, 255, 255},
        .rainSpeed  = 620.0f,
        .rainDrift  = 11.0f,
        .rainLength = 24.0f,
        .rainSpread = 0.55f,

        .rainDensity = 240.0f,
        .rainSpan    = 1400.0f,
    };

    World world(settings, config::kResolution);
    world.SetWeather(sky);

    // The woods. Left at the table's own defaults for now: what a stand is, how
    // thick it is and where its border falls are the numbers this is tuned by,
    // and they are settled by walking through a wood at each setting rather than
    // by argument, the same way the lantern and the cave coverage were.
    Grove grove;
    grove.Configure({.seed = settings.seed}, settings, world.Sky());


    if (argc >= 4 && TextIsEqual(argv[1], "--sun")) {
        const float x = static_cast<float>(std::atof(argv[2]));
        const float y = static_cast<float>(std::atof(argv[3]));

        const Rectangle region = {x, y, 900.0f, 400.0f};

        world.StepWeather(1.0f / 60.0f);
        for (int step = 0; step < 400 * 60; step++) world.StepWeather(1.0f / 60.0f);

        world.Update(region);
        world.StepLight(region);

        ReportSun(world, region);

        CloseWindow();
        return 0;
    }

    if (argc >= 4 && TextIsEqual(argv[1], "--surface")) {
        terrain::Settings tuned = settings;

        // The knob under test, overridable from the command line so a sweep is a
        // loop in a shell rather than a rebuild each time.
        if (argc >= 5) tuned.surface.terraceSharp = static_cast<float>(std::atof(argv[4]));
        if (argc >= 6) tuned.surface.terrace = static_cast<float>(std::atof(argv[5]));

        ReportSurface(tuned, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])));

        CloseWindow();
        return 0;
    }

    if (weighing) {
        ReportTones();

        CloseWindow();
        return 0;
    }

    if (counting) {
        ReportCovers(settings, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                     static_cast<float>(std::atof(argv[4])));

        CloseWindow();
        return 0;
    }

    if (settling) {
        ReportSettling(world,
                       {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                        static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))},
                       (argc >= 7) ? std::atoi(argv[6]) : 600);

        CloseWindow();
        return 0;
    }

    if (assaying) {
        ReportOre(world, world.Settings(),
                  {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                   static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

        CloseWindow();
        return 0;
    }

    if (digging) {
        // The world's own settings and not the ones written above, because the
        // coverage cutoffs are measured into them by the constructor and an
        // uncalibrated copy carves nothing at all.
        terrain::Settings tuned = world.Settings();

        // Any cave setting, overridden as `name=value` after the region, so that
        // finding a set of them is a loop in a shell rather than a rebuild for
        // every value. Named rather than positional because a sweep usually moves
        // two knobs and a row of bare numbers is unreadable a day later.
        terrain::CaveSettings &c = tuned.caves;

        const std::array<std::pair<const char *, float *>, 34> knobs = {{
            {"region", &c.regionCoverage},        {"shallow", &c.regionCoverageShallow},
            {"deepens", &c.regionDeepens},         {"fade", &c.regionFade},
            {"blend", &c.blend},                  {"crust", &c.crust},
            {"warp", &c.warpAmplitude},           {"warpfreq", &c.warp.frequency},
            {"gwidth", &c.galleries.width},       {"gdeep", &c.galleries.widthAtDepth},
            {"gpinch", &c.galleries.pinch},       {"gswing", &c.galleries.swing},
            {"gfloor", &c.galleries.floor},       {"gfreq", &c.galleries.shape.frequency},
            {"gaspect", &c.galleries.shape.aspect},
            {"cwidth", &c.crawlways.width},       {"cdeep", &c.crawlways.widthAtDepth},
            {"cpinch", &c.crawlways.pinch},       {"cswing", &c.crawlways.swing},
            {"cfloor", &c.crawlways.floor},       {"cfreq", &c.crawlways.shape.frequency},
            {"caspect", &c.crawlways.shape.aspect},
            {"swidth", &c.shafts.width},          {"sdeep", &c.shafts.widthAtDepth},
            {"sfloor", &c.shafts.floor},
            {"sfreq", &c.shafts.shape.frequency}, {"saspect", &c.shafts.shape.aspect},
            {"reach", &c.shaftReach},             {"mouth", &c.mouthDepth},
            {"chamber", &c.chambers.coverage},
            {"cavern", &c.caverns.coverage},      {"rough", &c.roughness.amplitude},
            {"wdepth", &tuned.aquifer.depth},     {"wswing", &tuned.aquifer.swing},
        }};

        for (int a = 6; a < argc; a++) {
            const char *split = std::strchr(argv[a], '=');

            const auto knob = (split != nullptr)
                                ? std::find_if(knobs.begin(), knobs.end(),
                                               [&](const auto &k) {
                                                   return std::strncmp(k.first, argv[a],
                                                                       static_cast<std::size_t>(split - argv[a])) == 0
                                                          && k.first[split - argv[a]] == '\0';
                                               })
                                : knobs.end();

            if (knob == knobs.end()) {
                std::printf("unknown setting '%s'\n", argv[a]);

                CloseWindow();
                return 1;
            }

            *knob->second = static_cast<float>(std::atof(split + 1));
        }

        // Recalibrated after the overrides, since three of them are coverages and
        // a coverage is only a share once its cutoff has been measured.
        terrain::Calibrate(tuned);

        ReportCaves(tuned, {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                            static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

        CloseWindow();
        return 0;
    }

    // What has been picked up. The counterpart of Editor::Collected for anything
    // that is not a material — see item.h for why the two are separate tables.
    Harvest gathered{};

    // The two ends of a day, and they are two colours rather than one turned down.
    //
    // Noon is the near-neutral light this world was lit by before there was a clock;
    // midnight is a fiftieth of it and blue where the day is not. Both are radiances
    // and neither can be read as a brightness — light reaches the screen through an
    // exposure curve, so this midnight is a readable dark rather than the near-black
    // the ratio suggests, and a torch stops washing out against it and starts
    // reading as the warm thing it is.
    world.SetDaylight({2.6f, 2.8f, 3.1f}, {0.05f, 0.06f, 0.09f});

    if (probing) {
        const Rectangle strip = {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                                 static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))};

        Harvest probed{};

        DrawProbe(world, grove, probed, strip, argv[6], (argc >= 8) ? std::atoi(argv[7]) : 1,
                  (argc >= 9) ? static_cast<float>(std::atof(argv[8])) : 0.0f,
                  (argc >= 10) ? (std::atoi(argv[9]) != 0) : true,
                  (argc >= 11) ? std::atoi(argv[10]) : 0);

        CloseWindow();
        return 0;
    }

    if (reading) {
        const float x  = static_cast<float>(std::atof(argv[2]));
        const float y0 = static_cast<float>(std::atof(argv[3]));
        const float y1 = static_cast<float>(std::atof(argv[4]));

        world.Update({x - 128.0f, y0 - 128.0f, 256.0f, y1 - y0 + 256.0f});
        ReportColumn(world, settings, x, y0, y1);

        CloseWindow();
        return 0;
    }

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

    // The last thing the world said back, and how long it has left on screen.
    const char *notice = "";
    float noticeFor    = 0.0f;

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

        // Grown over the visible band rather than the simulated one. A plant is
        // drawn and nothing else — it holds no liquid and steps no automaton — so
        // there is nothing about one off screen that has to have settled by the
        // time it scrolls in.
        grove.Update(world, ViewBounds(camera), player.Centre(), world.Sky().Time(), dt, gathered);

        hotbar.Update();
        editor.Update(world, hotbar, camera);

        debug_view::ReadToggles(debug);
        if (IsKeyPressed(KEY_R)) world.Reset();

        // An action rather than a state, so it is read here beside the other one and
        // not held in the debug toggles. Asking again while one is running queues
        // another quarter.
        if (IsKeyPressed(KEY_F8)) world.SkipToQuarter();

        // Holds a season, for looking at one rather than waiting a year. There is
        // no year yet, so without this the world is always in spring — which is
        // exactly why the key exists: the whole seasonal path can be exercised and
        // judged before there is a calendar to drive it.
        if (IsKeyPressed(KEY_F9)) world.CycleSeason();

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

        // Only on the frame the swing began. The strike box is live for the whole
        // window, so reading that instead lands nine blows per swing.
        if (player.AttackStarted()) {
            grove.Strike(player.AttackHitbox(), 1.0f, player.Centre(), world.Sky().Time());

            // And whatever grass the same swing went through. A tuft gives up
            // fibre where a tree gives up wood, into the same pile on the ground.
            const Rectangle swing = player.AttackHitbox();

            const int mown = world.MowGrass(swing, world.Sky().Time());

            if (mown > 0) {
                const Vector2 from = {swing.x + swing.width * 0.5f, swing.y + swing.height * 0.5f};

                // Thrown away from the player rather than towards them, which is
                // the side the blow came from and the way the wood already goes.
                const float away = (from.x < player.Centre().x) ? -1.0f : 1.0f;

                grove.Fallen().Spawn(Item::Fibre, mown, from, away, world.Sky().Time());
            }
        }

        // A sapling goes in where the player is standing, and the species is the
        // one the climate there would have grown anyway. Choosing it for the
        // player rather than offering a menu: what a place will support is a
        // property of the place, and planting a pine in a swamp is not a decision
        // worth surfacing before there is a reason to make it.
        if (IsKeyPressed(KEY_P)) {
            const Vector2 feet = {player.Centre().x, player.Bounds().y + player.Bounds().height};

            if (gathered[ItemIndex(Item::Sapling)] <= 0) {
                notice     = "no saplings — fell a tree for one";
                noticeFor  = kNoticeTime;
            } else if (grove.Plant(grove.Suited(feet.x), feet, world.Sky().Time())) {
                gathered[ItemIndex(Item::Sapling)]--;
            } else {
                // Said out loud rather than swallowed. A key that does nothing and
                // explains nothing is the same to a player as a key that is broken.
                notice    = "no room here — something is already growing";
                noticeFor = kNoticeTime;
            }
        }

        noticeFor = std::max(noticeFor - dt, 0.0f);

        // Re-offered every frame rather than registered once. A light that has
        // to be renewed to keep burning needs nothing told to it when the thing
        // carrying it moves, and nothing told to it when that thing is gone.
        world.AddLight(player.Centre(), Lantern(lantern), config::kLanternRadius);

        // Drifted before the light is solved, because the shade the cloud casts is
        // read during the solve. Advancing it afterwards would light every frame by
        // the sky of the frame before it, which nothing would look wrong about and
        // which would be wrong.
        world.StepWeather(dt * (debug.fastWeather ? debug_view::kFastWeather : 1.0f));

        // Offered on the same terms as the lantern above, and for the same
        // reason: a canopy that has to be re-offered to keep shading needs
        // nothing told to it when the tree is felled.
        grove.Shade(world, world.Sky().Time());

        // Solved after the world has finished moving, so the light matches the
        // frame it is about to be drawn over rather than the one before it.
        world.StepLight(active);
        lights.Update(world.Light());

        // Captured before the frame opens, since it renders to its own target.
        liquids.Capture(world, ViewBounds(camera), camera);

        Draw(world, grove, gathered, player, hotbar, editor, liquids, lights, camera, debug, lantern, notice,
             noticeFor);
    }

    grove.Unload();
    lights.Unload();
    liquids.Unload();
    CloseWindow();

    return 0;
}
