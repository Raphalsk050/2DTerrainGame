#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "grove.h"
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
    DrawText(text, x + 1, y + 1, 14, {235, 240, 248, 190});
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

void Draw(const World &world, const Grove &grove, const Player &player, const Hotbar &hotbar, const Editor &editor,
          const LiquidLayer &liquids, const LightLayer &lights, const Camera2D &camera,
          const debug_view::Toggles &debug, float lantern) {
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

    // The plants over the ground and behind the character, and on this side of
    // the light multiply: a tree is lit by the same daylight as the ground it
    // stands on, and has to know nothing about it to be.
    grove.Draw(flora::Season::Summer);

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

    const Color ink = {14, 18, 24, 255};

    DrawLabel("A/D: move  |  space: jump  |  S: crouch  |  J: attack  |  mouse: aim  |  F: fly", 10, 10, ink);
    DrawLabel("left: apply brush  |  X: place/dig  |  1-9 or wheel: material  |  - / +: brush size  |  R: regenerate",
              10, 28, ink);
    DrawLabel(TextFormat("V: vertices  |  F3: chunks  |  F4: height grid  |  F5: light probes  |  F6: unlit %s  |"
                         "  F7: fast weather %s  |  F8: next quarter  |  , . : lantern %.1f",
                         debug.unlit ? "on" : "off", debug.fastWeather ? "on" : "off", lantern),
              10, 46, ink);

    DrawLabel(TextFormat("chunks: %d (%d pinned)   edits kept: %d   plants: %d (%d drawn)   water: %.1f   rays: %ld",
                         world.ResidentChunks(), world.PinnedChunks(), world.RememberedEdits(), grove.VisiblePlants(),
                         grove.DrawnPlants(), world.TotalWater(view), world.Light().Rays()),
              10, 70, ink);

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
        10, 106, sky.rain > 0.0f ? Color{16, 44, 82, 255} : ink);

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
              10, 124, damp > 0.66f ? Color{16, 44, 82, 255} : ink);

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
    grove.Configure({.seed = settings.seed}, settings);

    // The two ends of a day, and they are two colours rather than one turned down.
    //
    // Noon is the near-neutral light this world was lit by before there was a clock;
    // midnight is a fiftieth of it and blue where the day is not. Both are radiances
    // and neither can be read as a brightness — light reaches the screen through an
    // exposure curve, so this midnight is a readable dark rather than the near-black
    // the ratio suggests, and a torch stops washing out against it and starts
    // reading as the warm thing it is.
    world.SetDaylight({2.6f, 2.8f, 3.1f}, {0.05f, 0.06f, 0.09f});

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

        // Grown over the visible band rather than the simulated one. A plant is
        // drawn and nothing else — it holds no liquid and steps no automaton — so
        // there is nothing about one off screen that has to have settled by the
        // time it scrolls in.
        grove.Update(world, ViewBounds(camera));

        hotbar.Update();
        editor.Update(world, hotbar, camera);

        debug_view::ReadToggles(debug);
        if (IsKeyPressed(KEY_R)) world.Reset();

        // An action rather than a state, so it is read here beside the other one and
        // not held in the debug toggles. Asking again while one is running queues
        // another quarter.
        if (IsKeyPressed(KEY_F8)) world.SkipToQuarter();

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
        world.StepWeather(dt * (debug.fastWeather ? debug_view::kFastWeather : 1.0f));

        // Solved after the world has finished moving, so the light matches the
        // frame it is about to be drawn over rather than the one before it.
        world.StepLight(active);
        lights.Update(world.Light());

        // Captured before the frame opens, since it renders to its own target.
        liquids.Capture(world, ViewBounds(camera), camera);

        Draw(world, grove, player, hotbar, editor, liquids, lights, camera, debug, lantern);
    }

    grove.Unload();
    lights.Unload();
    liquids.Unload();
    CloseWindow();

    return 0;
}
