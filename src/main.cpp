#include "backdrop.h"
#include "canopy.h"
#include "commands.h"
#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "fixture.h"
#include "grove.h"
#include "hotbar.h"
#include "hud.h"
#include "liquid_layer.h"
#include "lit_layer.h"
#include "menu.h"
#include "player.h"
#include "probes.h"
#include "profile.h"
#include "render.h"
#include "console.h"
#include "scuff.h"
#include "sod.h"
#include "soil.h"
#include "raylib.h"
#include "rlgl.h"
#include "terrain.h"
#include "view.h"
#include "weather.h"
#include "world.h"

#include <algorithm>
#include <string>
#include <cctype>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

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

// The lantern as linear light, at a given strength.
light::Radiance Lantern(float strength) {
    constexpr float kByte = 1.0f / 255.0f;

    return {config::kLanternGlow.r * kByte * strength, config::kLanternGlow.g * kByte * strength,
            config::kLanternGlow.b * kByte * strength};
}

// And any lamp described the way the tables describe one.
light::Radiance Glow(const ElementLight &light) {
    constexpr float kByte = 1.0f / 255.0f;

    return {light.glow.r * kByte * light.strength, light.glow.g * kByte * light.strength,
            light.glow.b * kByte * light.strength};
}

// Half-side of the box an aimed swing lands in, in world pixels.
//
// A little over the slack the cursor uses to *choose* the axe, so that a click the
// cursor accepted is a click that connects. The two are separate figures because
// they answer separate questions — what the hand is willing to aim at, and what the
// blow covers — and tying them together would make widening the aim quietly widen
// the axe.
constexpr float kAimedBlow = 12.0f;


PlayerInput ReadPlayerInput(const Camera2D &camera, bool swinging) {
    PlayerInput input;

    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) input.moveX -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) input.moveX += 1.0f;

    input.jumpPressed   = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W);
    input.jumpHeld      = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W);
    input.crouchHeld    = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);

    // The left button, whatever the hand became: an axe in a trunk, a spade in the
    // hillside, a fist through the grass. The arm swings for all three, because to
    // the player they are one action — point at a thing and hit it — and it was
    // only ever the code that thought digging and chopping were different enough to
    // need different controls.
    //
    // There was a key here as well, and it has gone. What it did was land a blow in
    // front of the character while the cursor was pointing somewhere else, which is
    // two aims for one arm: a player who has just learnt that the mouse says where
    // the hand works has to unlearn it for this one action. Every blow now lands
    // where the cursor is.
    //
    // Held rather than pressed, so laying into a tree or a seam is one held button
    // and not a drumroll. What stops it becoming one blow a frame is the swing's own
    // cooldown, which is where that rule already lived.
    input.attackPressed = swinging;

    // The vertical axis is the same two keys as jump and crouch. Only flight
    // reads it, and while flying neither of those actions applies, so there is
    // nothing for it to conflict with.
    if (input.jumpHeld) input.moveY -= 1.0f;
    if (input.crouchHeld) input.moveY += 1.0f;

    input.flyToggled = IsKeyPressed(KEY_F);
    input.sprintHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    input.aimWorld = GetScreenToWorld2D(GetMousePosition(), camera);

    return input;
}

// Whether the hand is asking for the view rather than for the bar.
//
// The wheel already steps through the hotbar, and control-wheel is what every
// other program on the machine zooms with, so the modifier is read in one place
// and both readers are told about it — otherwise a zoom would also change the
// slot in hand.
bool ZoomModifier() {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER)
        || IsKeyDown(KEY_RIGHT_SUPER);
}

// Steps the view in and out, in whole multiples and nothing between them.
//
// Whole, and that is not a matter of taste. Everything in the world is drawn on
// one of two pixel grids — five world units for the terrain's outline, two for a
// plant's texel — and both are chosen to be a whole number of screen pixels. A
// multiplier of one and a half turns the first into seven and a half, which
// rasterises as columns alternating seven pixels wide and eight, and that is the
// one thing art at this size may never do. At a whole multiple every texel keeps
// its shape whatever the view is doing, so the picture zooms rather than
// resamples.
//
// One is the floor because it is what the world was framed against: it shows the
// most ground of any setting, and there is nothing to be gained by pulling
// further back except a character too small to read. Everything above it is the
// player's own comfort.
void ReadZoom(Camera2D &camera) {
    int level = static_cast<int>(std::lround(camera.zoom));

    if (IsKeyPressed(KEY_PAGE_UP)) level++;
    if (IsKeyPressed(KEY_PAGE_DOWN)) level--;

    if (ZoomModifier()) {
        const float wheel = GetMouseWheelMove();

        if (wheel > 0.0f) level++;
        if (wheel < 0.0f) level--;
    }

    camera.zoom = static_cast<float>(std::clamp(level, config::kMinZoom, config::kMaxZoom));
}

void FollowPlayer(Camera2D &camera, const Player &player, float dt) {
    const Vector2 target = player.Centre();

    // Exponential approach, expressed so that the rate is the same whatever the
    // frame duration.
    const float t = 1.0f - std::exp(-kCameraFollow * dt);

    camera.target.x += (target.x - camera.target.x) * t;
    camera.target.y += (target.y - camera.target.y) * t;
}

} // namespace

int main(int argc, char **argv) {
    // Whether this run is a probe rather than a game — see probes.h. Asked before
    // the window opens, because it decides whether the window shows at all.
    const bool measuring = probes::Headless(argc, argv);

    // `--profile [frames]` plays the game with nobody at the keys and reports
    // where the frame went. This one wants a window on screen: the draw is half
    // of what it is measuring.
    const bool profiling = argc >= 2 && TextIsEqual(argv[1], "--profile");

    // Resizable, with a floor under it: below the minimum the hotbar is wider than
    // the frame and the head-up display runs off the side of it.
    SetConfigFlags(measuring ? FLAG_WINDOW_HIDDEN : FLAG_WINDOW_RESIZABLE);

    // Off while profiling, so a frame that finishes early is not slept away and
    // the report says what the frame actually costs rather than what it was
    // capped at.
    const int targetFps = profiling ? 0 : config::kTargetFps;

    InitWindow(config::kScreenWidth, config::kScreenHeight, config::kGameName);
    SetWindowMinSize(config::kMinScreenWidth, config::kMinScreenHeight);
    SetTargetFPS(targetFps);

    // Escape stops closing the window, so that it can close the inventory
    // instead.
    //
    // raylib binds it to WindowShouldClose by default, which with a panel on
    // screen means the key every player presses to back out of a panel quits the
    // game instead — losing whatever they were in the middle of. The window still
    // closes by its own button, and Minecraft's escape has never quit anything
    // either.
    SetExitKey(KEY_NULL);

    // Assets are opened through paths relative to the executable.
    ChangeDirectory(GetApplicationDirectory());

    // The world, written one layer at a time. Every number below is in world
    // pixels or in features per terrain::kFeatureSpan pixels, so the settings can
    // be read against the size of the character: it is 26 pixels tall, 12 wide,
    // and jumps 72.
    //
    // Not const, and only for one reason: `Calibrate` writes the measured cutoffs
    // back into it further down. Nothing else here may be touched after this
    // point — the whole generator is a pure function of these numbers, and a
    // setting that changed while the world was running would mean two halves of the
    // same map generated from two different worlds.
    terrain::Settings settings =
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

                    // Where the ranges stand. One feature every twenty thousand
                    // pixels or so, which is ten screens: a range has to be
                    // something walked towards for a long while and then walked
                    // into, or it is scenery that happens to be tall.
                    .range         = {.frequency = 0.05f, .octaves = 2, .seed = 4406},
                    .rangeCoverage = 0.14f,
                    .rangeEdge     = 0.09f,

                    // And the crests inside one. Sixteen times the range's own
                    // frequency, so a range is several summits with valleys between
                    // them rather than one enormous cone — a feature every twelve
                    // hundred pixels, which is three or four peaks across a range.
                    //
                    // The frequency is half of what decides whether a mountain can
                    // be climbed, the amplitude being the other half: the crest
                    // rises over half a feature, so at this pair a flank averages a
                    // slope of 0.7 and the terrace turns that into a twenty-four
                    // pixel riser every thirty-four of run. Against a jump of
                    // seventy-two that is a staircase. At twice this it was a slope
                    // of 1.4 and a riser every seventeen, which is a wall with
                    // notches in it.
                    .ridge = {.frequency = 0.8f, .octaves = 3, .seed = 4407},

                    // Four hundred pixels at the crest, on top of the hundred or two
                    // the relief and the hills already give. A peak therefore stands
                    // some five hundred above the plains, which against a character
                    // of twenty-six is nineteen of it and very nearly the whole of a
                    // full-screen view from the valley floor.
                    //
                    // Bounded from above by the sky rather than by taste. The cloud
                    // deck hangs between y = -640 and y = -320, and a peak that
                    // climbed past the underside of it would be standing *inside*
                    // the cloud — which the rain already answers correctly, by
                    // having nowhere left to fall from, but which reads as a summit
                    // that mysteriously never gets any weather. At this figure the
                    // tops sit just under the deck and only the rare one pokes into
                    // it during a storm, when the base drops a hundred pixels.
                    //
                    // The terrace is what makes the climb possible at all — the
                    // whole slope is snapped into ledges a quarter of a jump apart,
                    // so a mountainside is a staircase rather than a wall.
                    .ridgeAmplitude = 470.0f,

                    // Under one, which broadens the crest instead of sharpening it.
                    //
                    // This was 2.2 and it drew a range of shark's teeth: every
                    // summit came to a point one lattice column wide, so there was
                    // nowhere up there to stand and the climb ended on a spike. A
                    // mountain has to be somewhere to *go*, and that means shelves
                    // on the way up and a top to arrive at — both of which live in
                    // the mid-range of the fold, which is precisely what an exponent
                    // under one keeps and one over it throws away.
                    //
                    // The three octaves of the ridge field then do the rest: with
                    // the crest flattened, their bumps land as ledges and shoulders
                    // across the summit rather than as ripples down a point.
                    .ridgeSharp = 0.72f,

                    // And the shelves cut across the face. Forty-eight pixels is
                    // two of the world's own ledges and two thirds of the jump, so
                    // a riser is climbed rather than scaled, and it is coarse enough
                    // that a shelf is a run of flat ground wide enough to build on
                    // and to meet something on.
                    //
                    // Snapped most of the way rather than all of it. At one the face
                    // is a flight of identical stairs, which is legible and dead; at
                    // this the shelves are plainly there and no two of them are the
                    // same width, because what is under them is still a mountain.
                    .shelfStep = 48.0f,
                    .shelf     = 0.66f,

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
                    // A hundred and ten pixels of solid ground over everything
                    // below, which is what keeps the surface a surface. Only an
                    // entrance may cross it.
                    .crust     = 110.0f,
                    .crustFade = 72.0f,

                    // The systems themselves. Every number here is a fact about a
                    // walk rather than about a field, which is the point of the
                    // change: a passage is twenty-four steps of six pixels because
                    // that is how far the digger went.
                    .systems =
                        {
                            // A cell is wide and short because a system is. Nine
                            // of them are searched per query, so the shape of the
                            // cell is what keeps that search small.
                            .cellSpan = 800.0f,
                            .cellRise = 400.0f,

                            // Half the eligible cells hold one. With the region
                            // gate over the top, that comes out at a system every
                            // couple of screens near the surface and most of the
                            // ground occupied far below.
                            .chance = 0.9f,

                            // Two hundred and forty steps of six pixels: fourteen
                            // hundred pixels of walking, which the wander folds
                            // into something under a thousand across. Six is the
                            // lattice step, so the walk cannot cut a corner finer
                            // than the world can draw.
                            .steps      = 240,
                            .stepLength = 6.0f,

                            .wander  = 0.34f,
                            .damping = 0.72f,

                            // Two fifths of a step into the vertical, so a passage
                            // is walked rather than fallen down.
                            .squash = 0.42f,

                            // Thirty-two pixels of headroom near the crust, opening
                            // to forty-eight far below — comfortably over the
                            // character's twenty-six either way, since a passage
                            // that has to be crouched through is a crawlway and
                            // this layer is the route.
                            .radius        = 16.0f,
                            .radiusAtDepth = 24.0f,
                            .growthDepth   = 1800.0f,

                            .taper = 0.16f,

                            // Three branches off each trunk, at a shade over sixty
                            // degrees: enough that a system has somewhere to go
                            // wrong in, few enough that it still reads as one
                            // route with sides rather than as a net.
                            .branches     = 3,
                            .branchLength = 0.42f,
                            .branchAngle  = 1.05f,
                            .branchRadius = 0.78f,

                            // A room every twenty steps or so, three and a bit
                            // times the width of the passage that leads into it —
                            // a hundred pixels of headroom, which is four of the
                            // character.
                            .roomChance = 0.05f,
                            .roomSteps  = 7,
                            .roomSwell  = 3.2f,
                            .roomFloor  = 0.3f,

                            // Under a third of systems reach daylight, and the one
                            // that does opens forty-four pixels wide.
                            .entranceChance = 0.9f,
                            .entranceRadius = 22.0f,
                            .entranceWander = 0.16f,
                            .entranceSteps  = 90,
                        },

                    // A tenth of the ground just under the crust is cave country,
                    // rising to half of it far below. Rarity is only ever felt at
                    // the surface; depth is where the volume belongs.
                    .region                = {.frequency = 0.6f, .octaves = 2, .seed = 4410},
                    .regionCoverage        = 0.55f,
                    .regionCoverageShallow = 0.35f,
                    .regionDeepens         = 1500.0f,

                    // The wall: a fret over the whole of it, and rounded bites
                    // taken out of it. A swept circle is smooth, and this is what
                    // stops a corridor being an outline made of arcs.
                    .roughness = {.shape         = {.frequency = 24.0f, .octaves = 2, .seed = 4418},
                                  .amplitude     = 5.0f,
                                  .bias          = 0.329f,
                                  .lobes         = {.frequency = 20.0f, .octaves = 1, .seed = 4427},
                                  .lobeAmplitude = 9.0f,
                                  .lobeBite      = 0.62f,
                                  .reach         = 22.0f},
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
                    .depth = 3600.0f,
                    .swing = 1100.0f,
                    .step  = 12.0f,
                },

            // The two axes every biome in the world is chosen on: which cover lies
            // on the rock, which trees grow in it, what the grass under them is,
            // and how much it rains.
            //
            // One feature spans a dozen screens. It was four or five, and four or
            // five is not a region — it is a patch: a desert could be crossed in
            // under a minute of running, and the belt of steppe that is supposed to
            // approach it was a few hundred pixels wide. The fields are folded
            // Perlin and the frequency is only a scale, so nothing about *what*
            // values occur changes here — the same deserts and snowfields are still
            // reached, they are simply travelled into rather than stumbled over.
            //
            // Humidity stays the faster of the two so that a temperature band still
            // holds more than one kind of country: at equal frequencies the pair
            // move together and the world comes out as one axis with two names.
            .climate =
                {
                    .temperature = {.frequency = 0.08f, .octaves = 2, .seed = 4420},
                    .humidity    = {.frequency = 0.11f, .octaves = 2, .seed = 4421},

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
                // The winds are in pixels per second at ground level, and the spread
                // between the calmest row and the windiest is the thing to judge
                // rather than any one figure: it is the whole range every rooted
                // thing in the world swings through. The rows must stay in the order
                // of weather::Mood — this array is read by index.
                //
                // Not zero at the calm end, and it cannot be. The sway is drawn in
                // whole plant texels, so a crown moving less than one of them does
                // not move at all, and a wood standing perfectly rigid on a clear
                // afternoon reads as broken rather than as calm.
                {.name       = "clear",
                 .cover      = 0.14f,
                 .rain       = 0.0f,
                 .wind       = 7.0f,
                 .likelihood = 1.0f,
                 .sunlight   = {255, 252, 246, 255},
                 .ambient    = {150, 176, 214, 255},
                 .shade      = 0.85f},

                {.name       = "fair",
                 .cover      = 0.34f,
                 .rain       = 0.0f,
                 .wind       = 15.0f,
                 .likelihood = 1.6f,
                 .sunlight   = {255, 250, 240, 255},
                 .ambient    = {138, 162, 202, 255},
                 .shade      = 1.0f},

                // A bright, broken sky with the air tearing through it. Less cloud
                // than fair weather and more than twice the wind, which is the one
                // combination nothing derived from the cover could ever produce —
                // and the reason the wind is a column of this table at all.
                {.name       = "blustery",
                 .cover      = 0.30f,
                 .rain       = 0.0f,
                 .wind       = 40.0f,
                 .likelihood = 0.5f,
                 .sunlight   = {255, 250, 242, 255},
                 .ambient    = {140, 166, 206, 255},
                 .shade      = 0.95f},

                {.name       = "overcast",
                 .cover      = 0.78f,
                 .rain       = 0.0f,
                 .wind       = 24.0f,
                 .likelihood = 0.8f,
                 .sunlight   = {228, 232, 240, 255},
                 .ambient    = {116, 128, 152, 255},
                 .shade      = 1.0f},

                // Most of the sky, the only mood that rains, and the hardest it
                // blows. This row alone sets the envelope every share is taken
                // against — see Sky::Gale.
                {.name       = "storm",
                 .cover      = 0.94f,
                 .rain       = 1.0f,
                 .wind       = 54.0f,
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
        .cloudWind = 18.0f,

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
        .rainDrift  = 8.0f,
        .rainLength = 24.0f,
        .rainSpread = 0.55f,

        .rainDensity = 240.0f,
        .rainSpan    = 1400.0f,
    };

    // Measured before anything reads the settings, and not only before the world is
    // built.
    //
    // World's constructor calibrates its own copy, which is right and is not
    // enough: every report below is handed this one, and an uncalibrated copy is a
    // different world. The mountains are what made that visible — the range cutoff
    // defaults to a value no sample clears, so `--covers` reported a world with no
    // ranges and no snow in it while the game had both. Calibration is a
    // measurement and idempotent, so running it twice costs a moment at startup and
    // nothing else.
    terrain::Calibrate(settings);

    World world(settings, config::kResolution);
    world.SetWeather(sky);

    // The woods. Left at the table's own defaults for now: what a stand is, how
    // thick it is and where its border falls are the numbers this is tuned by,
    // and they are settled by walking through a wood at each setting rather than
    // by argument, the same way the lantern and the cave coverage were.
    Grove grove;

    fixture::Fixtures fixtures;
    grove.Configure({.seed = settings.seed}, settings, world.Sky());


    // What has been picked up. The counterpart of Editor::Collected for anything
    // that is not a material â€” see item.h for why the two are separate tables.
    Inventory inventory{};

    // The two ends of a day, and they are two colours rather than one turned down.
    //
    // Noon is the near-neutral light this world was lit by before there was a clock;
    // midnight is a twenty-fifth of it and blue where the day is not. Both are
    // radiances and neither can be read as a brightness â€” light reaches the screen
    // through an exposure curve, so this midnight is a readable dark rather than the
    // near-black the ratio suggests, and a torch stops washing out against it and
    // starts reading as the warm thing it is.
    //
    // **This is the knob for a ground that is too dark at night**, and the second
    // triple is the whole of it. Down at these values the exposure curve is near
    // enough straight, so doubling them roughly doubles what the ground comes out
    // at â€” which is not true at the noon end, where the curve is saturated and
    // halving the radiance costs five per cent.
    //
    // It no longer touches the drawn sky. The two came apart when the sky moved to
    // the source side of the light multiply (see lit_layer.h); how bright the night
    // *looks* is weather::Atmosphere::night, and this is how much light the world
    // actually receives. What every step up here costs is the torch: it is a step
    // towards a night a player can cross without lighting one.
    world.SetDaylight({2.6f, 2.8f, 3.1f}, {0.20f, 0.24f, 0.36f});

    // Every way of measuring this world rather than playing it, and the one line
    // that offers all of them. See probes.h.
    if (const std::optional<int> status = probes::Run(argc, argv, world, grove, settings, sky)) {
        CloseWindow();
        return *status;
    }

    // Dropped in above the ground at the origin rather than at a fixed height,
    // since the surface there is now wherever the relief put it.
    Player player({0.0f, terrain::Height(0.0f, settings) - 96.0f});

    // The dust under the character's feet. Beside the player rather than inside it,
    // because what a foot throws up is the ground's answer and not the body's — see
    // scuff.h.
    scuff::Trail trail;
    Editor editor;

    LiquidLayer liquids;

    // The world's own layer, so that the light multiplies the world and not the
    // sky behind it. See lit_layer.h.
    LitLayer lit;

    Backdrop backdrop;
    backdrop.Create();

    Camera2D camera = {};
    camera.offset   = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = static_cast<float>(config::kMinZoom);

    float accumulated = 0.0f;
    float lantern     = config::kLanternStrength;

    // The last thing the world said back, and how long it has left on screen.
    const char *notice = "";
    float noticeFor    = 0.0f;

    debug_view::Toggles debug;

    // Whether the inventory panel is up.
    //
    // Held here beside the debug toggles rather than inside the inventory, for
    // the reason debug_view::Toggles gives for the same choice: what is on the
    // screen is the state of the screen, and the loop is where the screen is
    // decided. It is also what this gates the whole simulation on, and a gate
    // hidden inside the thing it gates is a gate nobody finds.
    bool packOpen = false;

    // Whether the brush is waiting for the hand to come off the button.
    //
    // A click outside the panel dismisses it, and the button is still down for
    // the several frames a human click lasts — while the brush reads it held
    // rather than pressed. Without this, getting out of the inventory digs a
    // hole in whatever was behind it.
    bool holdOff = false;

    // The profile of a game being played by hand — see F2 below.
    //
    // Frames counted since it was turned on, and when the report was last written.
    // Both live out here with the other things that are states of the screen rather
    // than of the world.
    int profiled = 0;

    // The screens in front of the game, and how the world being played was made.
    //
    // The mode is held out here with the other things a session is rather than
    // inside the world, and the reason is that it is not a property of the world
    // at all: the same seed played twice is the same country, and which of the two
    // sets of rules a player is under is a fact about the sitting rather than about
    // the ground. When there are saves it will be written beside the seed in one of
    // them, which is a line in that file and nothing here.
    menu::Menu menu;

    Gamemode mode = Gamemode::Survival;

    // Making a world, one stage per frame.
    //
    // The stages are here rather than inside the world because most of them are not
    // the world's: the wood has to be replanted, the character put somewhere to
    // stand, the bag emptied. What they have in common is that each is a piece of
    // work short enough to sit inside a frame, so the screen in front of them keeps
    // drawing and the bar keeps moving.
    //
    // Each stage says what it is about to do, is drawn once, and *then* does it. A
    // stage that announced itself after the fact would name the thing the player has
    // already waited through, and the longest one would be the one nobody ever saw a
    // line for.
    struct Making {
        bool running = false;

        int stage = 0;
        int said  = -1;

        Gamemode mode = Gamemode::Survival;
    } making;

    // Straight into the world when nobody is at the keys. A profile of the title
    // screen is a profile of four rectangles, and the report is about the frame the
    // game actually costs.
    if (profiling) menu.Play();

    // The chat line and its log.
    //
    // Held out here with the other things that are states of the screen. Everything
    // that reads a key is gated on it being shut, which is the whole discipline the
    // feature needs: a box that takes typing while the character still answers to
    // WASD is a box that walks you off a cliff mid-sentence.
    console::Console chat;

    chat.Say("press T to type a command — /help lists them", console::Tone::Note);

    // `--profile [frames] [still]` plays the game as normal, with nobody at the
    // keys, and reports where the frame went before it closes. The only way to
    // see the draw beside the simulation, which the headless reports cannot show.
    const int profileFrames = profiling ? ((argc >= 3) ? std::atoi(argv[2]) : 600) : 0;

    // Full screen, always, because that is how the game is played and because
    // nearly everything in the frame is priced by the area of the view: the light
    // solves a region around it, the water steps one, and the ground is drawn
    // over it. A measurement in a small window is a measurement of a different
    // game.
    if (profiling) {
        const int monitor = GetCurrentMonitor();

        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));

        if (!IsWindowFullscreen()) ToggleFullscreen();
    }

    // And flying across the world rather than standing still, unless asked
    // otherwise. Standing still is the one thing a player never does, and it is
    // the case every cache in here is at its best: nothing streams, nothing is
    // invalidated, and the frame reads far better than it plays.
    const bool flying = profiling && !(argc >= 4 && TextIsEqual(argv[3], "still"));

    // The first frames are chunk generation and shader compilation, which is not
    // what a steady frame costs.
    constexpr int kWarmup = 60;

    int played = 0;

    if (profiling) profile::Begin();

    while (!WindowShouldClose()) {
        if (profiling) {
            played++;

            if (played > profileFrames + kWarmup) break;
            if (played == kWarmup) profile::Reset();

            profile::Frame();
        }

        const float dt = GetFrameTime();

        // A menu is up, and it has the frame to itself.
        //
        // Everything below this is the game: the simulation, the hand, the light and
        // the draw. None of it runs while a screen is in front, which is what makes
        // the title screen a screen rather than a picture hung over a world that is
        // still stepping behind it — and it is why the loop asks the stack rather
        // than keeping a paused flag of its own.
        if (!menu.Playing()) {
            const menu::Wish wish = menu.Update();

                    if (wish.quit) break;

            if (wish.create) {
                // A new country in the object every other system already points at
                // — see World::BeginRebuild for why it is done this way round and
                // not by building a second world.
                settings.seed = wish.seed;

                // Measured again here as well as inside the world, and for the
                // reason the startup path gives: this copy is what the wood is
                // configured against and what the character's landing height is read
                // from, and an uncalibrated copy is a different world.
                terrain::Calibrate(settings);

                world.BeginRebuild(settings);

                making = {};

                making.running = true;
                making.mode    = wish.mode;

                menu.Open(menu::Screen::Loading);
            }

            // A slice of the making, if one is under way.
            //
            // About a frame's worth, and the world stops on the first block past it —
            // see World::StepRebuild. What that buys is a screen that answers the
            // mouse and a bar that moves while several seconds of measurement go by,
            // instead of a window the desktop paints over as not responding.
            if (making.running) {
                constexpr float kSlice = 0.012f;

                // How far along the bar each stage stands. The measurement is nearly
                // all of the wait, so it is given nearly all of the bar — a bar whose
                // stages are evenly spaced would crawl through the first and jump
                // through the rest, which is a bar that lies about how long is left.
                constexpr float kMeasured = 0.78f;

                const char *doing = "";
                float share       = 0.0f;

                switch (making.stage) {
                case 0: doing = "reading the shape of the land"; share = 0.0f; break;
                case 1: doing = "measuring what this world holds"; share = kMeasured; break;
                case 2: doing = "growing the woods"; share = 0.84f; break;
                case 3: doing = "finding you somewhere to stand"; share = 0.88f; break;
                case 4: doing = "cutting the caves under your feet"; share = 0.94f; break;
                default: doing = "painting the ground"; share = 1.0f; break;
                }

                // Said first and done next frame, so the line on screen is about the
                // work being waited for rather than the work already over.
                if (making.said != making.stage) {
                    menu.Working(doing, share);

                    making.said = making.stage;
                } else {
                    switch (making.stage) {
                    case 0:
                        // Nothing of its own: BeginRebuild did it. The stage exists so
                        // that the first line is on screen before the measurement
                        // takes the frame.
                        making.stage++;
                        break;

                    case 1: {
                        // The long one, and the only one that comes back unfinished.
                        // Its own line names the seam being measured, so the screen
                        // says which of the six is costing the second it is costing.
                        const World::Making step = world.StepRebuild(kSlice);

                        menu.Working(step.what, step.share * kMeasured);

                        if (step.done) making.stage++;
                        break;
                    }

                    case 2:
                        world.SetWeather(sky);

                        // Everything that remembers the world that is gone. The wood
                        // keeps records keyed on the cell a tree grows in, the
                        // fixtures stand in cells, and both would otherwise turn up in
                        // a country that never had them.
                        grove.Clear();
                        grove.Configure({.seed = settings.seed}, settings, world.Sky());

                        fixtures.Clear();
                        inventory.Clear();

                        making.stage++;
                        break;

                    case 3:
                        mode = making.mode;

                        // Dropped in above the ground at the origin, as at startup:
                        // the surface there is wherever this seed's relief put it.
                        player = Player({0.0f, terrain::Height(0.0f, settings) - 96.0f});

                        camera.target = player.Centre();
                        camera.offset = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f + GetScreenHeight() / 4.0f};

                        packOpen = false;
                        holdOff  = true;

                        making.stage++;
                        break;

                    case 4:
                        // The chunks the player will be standing in. Done here rather
                        // than left to the first frame of play, which is where it used
                        // to land: a game that opens on a stutter is a game that opens
                        // badly, and this is a screen that is already waiting.
                        world.Update(view::Expand(view::Bounds(camera), config::kSimulationMargin));

                        making.stage++;
                        break;

                    case 5:
                        // And their pictures, for the same reason. PaintChunks opens a
                        // render target, so it has to run outside a frame — which is
                        // where this is.
                        world.PaintChunks(view::Bounds(camera));

                        making.stage++;
                        break;

                    default:
                        making.running = false;

                        chat.Say(TextFormat("world %d, %s", settings.seed, NameOf(mode)), console::Tone::Done);

                        menu.Play();
                        break;
                    }
                }
            }

            BeginDrawing();
            menu.Draw();
            EndDrawing();

            continue;
        }

        // The wall clock rather than the weather's, so a line stays readable for as
        // long as it takes to read whatever F7 is doing to the sky.
        chat.Step(dt);

        // Opened on T, and never while the pack is up or while it is already open —
        // in the second case the T belongs in the box, and Console::Open would eat
        // it anyway.
        if (!chat.IsOpen() && !packOpen && IsKeyPressed(KEY_T)) chat.Open();

        // Everything below asks whether the player is typing before it reads a key.
        const bool typing = chat.IsOpen();

        if (typing) {
            const std::string sent = chat.Read();

            if (!sent.empty()) commands::Run(sent, world, grove, inventory, player, camera, chat);
        }

        // The frame can change size between any two frames, so the two things that
        // are sized to it are set from it every frame rather than when it changes.
        // Nothing then has to notice a resize, and there is no path where something
        // was told about one and something else was not.
        camera.offset = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f + GetScreenHeight() / 4.0f};
        liquids.Fit(GetScreenWidth(), GetScreenHeight());
        lit.Fit(GetScreenWidth(), GetScreenHeight());
        backdrop.Fit(GetScreenWidth(), GetScreenHeight());

        // How far in the view is set, read before anything asks what the view
        // covers. Outside the panel gate, unlike everything else about the world:
        // a player who opened the inventory to look at something and wants to look
        // at it closer is asking about the screen and not about the world.
        ReadZoom(camera);

        // Chunks are generated over the simulated band, not merely the visible
        // one. A write-back to a vertex whose chunk is absent is dropped, which
        // would quietly destroy the liquid that flowed there.
        const Rectangle active = view::Expand(view::Bounds(camera), config::kSimulationMargin);

        // Escape, and what it means depends on what is in front of the world.
        //
        // Asked before the panel toggle below and against the panel as it stands
        // now, which is the whole of the ordering: the toggle clears `packOpen` in
        // this same frame, so a test written after it would see a shut panel and
        // pause the game on the very press that closed one.
        if (!typing && IsKeyPressed(KEY_ESCAPE) && !packOpen) menu.Open(menu::Screen::Title);

        // Read before the gate below, since it is the one key that has to work on
        // both sides of it. Escape closes the panel as well, which it can only do
        // because the exit key was cleared at startup — see SetExitKey.
        if (!typing && (IsKeyPressed(KEY_TAB) || (packOpen && IsKeyPressed(KEY_ESCAPE)))) {
            packOpen = !packOpen;

            // Anything still on the cursor goes into the world rather than into
            // nowhere. Closing over a full hand is the one way a stack could be
            // held by a panel that is no longer drawn.
            if (!packOpen) {
                holdOff = true;

                const Stack held = inventory.Release();

                if (!held.Empty()) {
                    grove.Fallen().Toss(held, player.Centre(), GetScreenToWorld2D(GetMousePosition(), camera),
                                        world.Sky().Time());
                }
            }
        }

        // Streaming carries on with the panel up. It is the one step that is
        // about where the view is rather than about time passing, the view is not
        // going anywhere, and leaving it running means there is no state to
        // catch up on when the panel closes.
        world.Update(active);

        if (packOpen) {
            const Inventory::Gesture gesture = inventory.Update(mode);

            if (!gesture.thrown.Empty()) {
                grove.Fallen().Toss(gesture.thrown, player.Centre(), GetScreenToWorld2D(GetMousePosition(), camera),
                                    world.Sky().Time());
            }

            if (gesture.close) {
                packOpen = false;
                holdOff  = true;
            }
        }

        if (holdOff && IsMouseButtonUp(MOUSE_BUTTON_LEFT) && IsMouseButtonUp(MOUSE_BUTTON_RIGHT)) holdOff = false;

        // Everything from here to the light solve is the world moving, and none
        // of it runs while the panel is up. Single player, so a panel is a pause;
        // and a drop left falling behind an open inventory is a drop that has
        // timed out and gone by the time it is looked at again.
        if (!packOpen) {
            PROFILE_ZONE("grove.Update");

            // Grown over the visible band rather than the simulated one. A plant is
            // drawn and nothing else — it holds no liquid and steps no automaton — so
            // there is nothing about one off screen that has to have settled by the
            // time it scrolls in.
            grove.Update(world, view::Bounds(camera), player.Centre(), world.Sky().Time(), dt, inventory);

            // Anything whose surface has been dug out from under it comes down.
            // Beside the grove's own pass and for the same reason: what a fixture
            // is fixed to is the world, and the world is what the player has just
            // been changing.
            fixtures.Undermine(world, grove.Fallen(), world.Sky().Time());

            // The two that read the mouse wait out the click that closed the
            // panel; the plants above do not, since nothing about them is a
            // click.
            if (!holdOff && !typing) {
                hotbar::Update(inventory, ZoomModifier());

                // Handed the player's body from before it moves this frame, which
                // is what the reach is measured from, which side an overflowing
                // dig throws its blocks out on, and the room no block may be laid
                // in. A frame of lag at a run is six pixels against a reach of
                // ninety-six, and a block laid into where the body is about to be
                // is a block the body is stopped by rather than buried in.
                const char *said =
                    editor.Update(world, inventory, grove, fixtures, camera, player.Bounds(), mode,
                                  world.Sky().Time());

                if (said != nullptr) {
                    notice    = said;
                    noticeFor = kNoticeTime;
                }
            }
        }

            // F2: the profile of what is actually being played.
        //
        // `--profile` measures a flight over the surface with nobody at the keys,
        // which is the right way to compare two builds and the wrong way to find out
        // why *this* is slow — the answer is usually somewhere the flight never goes.
        // This is the same instrument with a player at the controls: press it once to
        // start counting, press it again to write the report out.
        //
        // The report goes to a file rather than to the console, because a game
        // started by double-clicking it has no console. It is written beside the
        // executable, which is where the working directory was moved to at startup.
        if (!typing && IsKeyPressed(KEY_F2)) {
            if (profile::Running()) {
                const bool wrote = profile::Write("profile.txt", "played");

                chat.Say(wrote ? TextFormat("profile of %d frames written to profile.txt", profiled)
                               : "could not write profile.txt",
                         wrote ? console::Tone::Done : console::Tone::Failed);

                profile::End();

                profiled = 0;
            } else {
                profile::Begin();
                profile::Reset();

                profiled = 0;

                chat.Say("profiling — press F2 again to write it out", console::Tone::Note);
            }
        }

        // Counted only while it is running, since every figure in the report is
        // divided by this — and never while `--profile` is doing the counting
        // itself, or every figure in *its* report comes out halved.
        if (!profiling && profile::Running()) {
            profile::Frame();

            profiled++;

            // Written as it goes as well as on the way out. A profile is wanted most
            // when something has gone wrong, and something that has gone wrong is
            // exactly the case where the second press never happens.
            if (profiled % 240 == 0) profile::Write("profile.txt", "played");
        }

        if (!typing) debug_view::ReadToggles(debug);
        if (!typing && IsKeyPressed(KEY_R)) {
            world.Reset();
            fixtures.Clear();
        }

        // An action rather than a state, so it is read here beside the other one and
        // not held in the debug toggles. Asking again while one is running queues
        // another quarter.
        if (!typing && IsKeyPressed(KEY_F8)) world.SkipToQuarter();

        // Holds a season, for looking at one rather than waiting a year. There is
        // no year yet, so without this the world is always in spring — which is
        // exactly why the key exists: the whole seasonal path can be exercised and
        // judged before there is a calendar to drive it.
        if (!typing && IsKeyPressed(KEY_F9)) world.CycleSeason();

        // And holds a weather, for the same reason one stop further on: which
        // weather blows is a pure function of the spell and the seed, so a storm
        // is something to be waited for rather than something to be looked at.
        // Everything a gale does — the rain, the shade, the gusts, the leaves
        // coming off a wood — has to be judged with one blowing.
        if (!typing && IsKeyPressed(KEY_F12)) world.CycleWeather();

        // C: take the cloud out of the light while leaving it on screen. It used to
        // gate the tree shade as well, which made it useless for clearing either
        // suspect on its own — and then the tree shade turned out to be the bug and
        // went. See World::AddCover's note.
        if (!typing && IsKeyPressed(KEY_C)) world.ToggleSkyCover();

        // B: the multi-bounce, on and off. Beside C because it is the same kind of
        // key — neither is a way of playing, both are ways of telling two things
        // apart that are on top of each other while both are on.
        //
        // It is here because the bounce is fed back temporally: one bounce a frame,
        // this frame's scattering source being last frame's fluence. That history is
        // the only thing in the solver that remembers where the region *was*, so it
        // is the only thing that can disagree with itself when the region moves —
        // and the bank of scenes measures exactly that, at 2.4% of level with the
        // bounce on against 0.07% with it off (see LIGHT.md §3). Anything that
        // flickers under a walking player is either in that 2.4% or is not the
        // bounce at all, and one keypress is the whole difference between those two
        // answers.
        //
        // Off is not a look. Direct light with volumetric occlusion is a duller
        // world and a wrong one; nothing lit round a corner, no colour carried off a
        // surface. See light::Settings::bounce.
        if (!typing && IsKeyPressed(KEY_B)) {
            light::Settings tuned = world.LightSettings();

            tuned.bounce = !tuned.bounce;

            world.SetLightSettings(tuned);
        }

        // An action beside the other two, and for the reason F8 gives: the debug
        // toggles hold the state of the screen, and this is not a state.
        if (!typing && IsKeyPressed(KEY_F11)) inventory.Stock();

        // Turned up and down while walking, since how much light the player
        // carries is a balance question and the only way to settle it is to be
        // underground at each setting. Zero is a valid answer: it leaves the
        // dark to torches alone.
        if (!typing && IsKeyPressed(KEY_COMMA)) lantern = std::max(lantern - config::kLanternStep, 0.0f);
        if (!typing && IsKeyPressed(KEY_PERIOD)) lantern = std::min(lantern + config::kLanternStep, config::kLanternMax);

        // The accumulator is not fed while the panel is up, rather than being fed
        // and the stepping skipped. Skipping alone would leave it holding
        // kMaxAccumulated by the time the panel closed and run a quarter of a
        // second of water in one frame — the whole pond would jump.
        if (!packOpen) {
            accumulated = std::min(accumulated + dt, kMaxAccumulated);

            while (accumulated >= kWaterStep) {
                world.StepWater(active);
                accumulated -= kWaterStep;
            }

            // Taken before the step, because a landing is only visible from the
            // near side of it: once the body is resting on the ground its downward
            // speed has already been cleared, and a puff sized from that is a puff
            // every landing throws at nothing.
            const float fell = std::max(player.Velocity().y, 0.0f);

            // A neutral input while typing rather than no update at all: the
            // character has to keep falling and keep being pushed out of walls, it
            // simply must not answer to the keys that are spelling a command.
            // Flown across the world under its own power while profiling, since
            // standing still is the one thing a player never does and the one
            // case every cache in here is at its best. See `--profile`.
            const PlayerInput moves =
                flying ? PlayerInput{.moveX      = 1.0f,
                                     .jumpHeld   = false,
                                     .flyToggled = (played == kWarmup / 2),
                                     .sprintHeld = true}
                : typing ? PlayerInput{}
                         : ReadPlayerInput(camera, !packOpen && !holdOff && editor.Left() != Editor::Hand::Idle
                                                       && IsMouseButtonDown(MOUSE_BUTTON_LEFT));

            PROFILE_ZONE("player");

            player.Update(moves, world, dt);

            trail.Update(world, player.Bounds(), std::fabs(player.Velocity().x), fell, player.IsGrounded(),
                         world.Sky().Time());
            FollowPlayer(camera, player, dt);
        }

        // Only on the frame the swing began. The strike box is live for the whole
        // window, so reading that instead lands nine blows per swing.
        if (!packOpen && !typing && player.AttackStarted()) {
            // Where the blow lands: where the cursor is, whichever tool the hand
            // became. The arm follows the aim, because a blow landing in front of
            // the character while the cursor points elsewhere is a second, invisible
            // cursor for the player to keep track of.
            const Rectangle swing = {editor.Aim().x - kAimedBlow, editor.Aim().y - kAimedBlow, kAimedBlow * 2.0f,
                                     kAimedBlow * 2.0f};

            // Bounded by the editor's own reach, which is the reach the ground is
            // dug at. The arm still swings out there — a swing at nothing is a swing
            // — and nothing it passes through is touched.
            if (editor.Reachable()) {
                // One blow's worth, in logs cut through.
                //
                // Chopping is hit-based and Minecraft's breaking is time-based, and
                // this is where the two are reconciled: a held attack lands a blow
                // every kAttackCooldown, a tree is flora::Settings::toughness logs
                // thick, and a log costs flora::kLogSeconds by hand. So a blow is
                // worth one cadence's share of one log, and a tree comes down in the
                // time its own logs would take.
                //
                // Nothing about the swing changes when there is an axe. An axe is a
                // ToolSpeed, and it divides the same seconds the ground's do.
                constexpr float kChopBlow = player_config::kAttackCooldown / flora::kLogSeconds;

                // Wood only where the press decided this hand was an axe — see
                // Editor::Hand. The latch is what keeps a player digging out from
                // under a tree from felling it the moment the cursor wanders onto
                // the trunk, and a swing that struck whatever it happened to be over
                // would undo it.
                if (editor.Left() == Editor::Hand::Chop) {
                    grove.Strike(swing, kChopBlow, player.Centre(), world.Sky().Time());
                }

                // And whatever grass the same swing went through, whichever tool it
                // was: a handful of grass comes away from a spade as readily as from
                // an axe. A tuft gives up fibre where a tree gives up wood, into the
                // same pile on the ground.
                const int mown = world.MowGrass(swing, world.Sky().Time());

                if (mown > 0) {
                    const Vector2 from = {swing.x + swing.width * 0.5f, swing.y + swing.height * 0.5f};

                    // Thrown away from the player rather than towards them, which is
                    // the side the blow came from and the way the wood already goes.
                    const float away = (from.x < player.Centre().x) ? -1.0f : 1.0f;

                    grove.Fallen().Scatter(ItemsOf(Item::Fibre, mown), from, away, world.Sky().Time());
                }
            }
        }

        noticeFor = std::max(noticeFor - dt, 0.0f);

        // The light goes out with the solve and not on its own.
        //
        // AddLight is re-offered every frame and cleared by each solve, so the
        // two have to be skipped together: skipping only the offer would let the
        // solve run against an empty list and put the lantern out, and skipping
        // only the solve would leave offers piling up against a field nothing is
        // clearing. Left alone, the light already on screen is the light of a
        // world that has stopped moving, which is the right answer.
        if (!packOpen) {
            // Re-offered every frame rather than registered once. A light that has
            // to be renewed to keep burning needs nothing told to it when the thing
            // carrying it moves, and nothing told to it when that thing is gone.
            // A torch in the hand is a torch, held up. The base lantern stays
            // underneath it rather than being replaced: what it is for is placing a
            // foot in the dark, and a game that goes black whenever the last torch
            // is spent is a game that has taken something away rather than given
            // something. Holding one lights the way properly.
            const Stack &carried = inventory.Held();

            const bool lit = carried.holds == Holds::Item && carried.count > 0
                          && Def(carried.AsItem()).placement == Placement::Fixture;

            if (lit) {
                const std::optional<fixture::Kind> kind = fixture::KindOf(carried.AsItem());

                if (kind.has_value()) {
                    const ElementLight &glow = fixture::Of(*kind).light;

                    world.AddLight(player.Centre(), Lantern(lantern) + Glow(glow), config::kLanternRadius * 3.0f);
                } else {
                    world.AddLight(player.Centre(), Lantern(lantern), config::kLanternRadius);
                }
            } else {
                world.AddLight(player.Centre(), Lantern(lantern), config::kLanternRadius);
            }

            // And every one that has been put up, re-offered on the same terms.
            fixtures.Illuminate(world, active);

            // Drifted before the light is solved, because the shade the cloud casts is
            // read during the solve. Advancing it afterwards would light every frame by
            // the sky of the frame before it, which nothing would look wrong about and
            // which would be wrong.
            {
                PROFILE_ZONE("StepWeather");

                world.StepWeather(dt * (debug.fastWeather ? debug_view::kFastWeather : 1.0f));
            }

            // Solved after the world has finished moving, so the light matches the
            // frame it is about to be drawn over rather than the one before it.
            world.StepLight(active);

        }

        // The ground, rasterised into a picture per chunk. Out here with the two
        // below because it opens a texture of its own, which cannot be done
        // inside a frame.
        world.PaintChunks(view::Bounds(camera));

        // Captured before the frame opens, since it renders to its own target.
        {
            PROFILE_ZONE("liquids.Capture");

            liquids.Capture(world, view::Bounds(camera), camera);
        }

        // And the world, into a target of its own so the multiply below reaches
        // the world and stops short of the sky behind it. Same constraint again,
        // and the reason this is the whole scene bar the atmosphere rather than
        // just the light: the multiply has to happen where the layer is, and the
        // layer cannot be opened once the frame has been.
        {
            PROFILE_ZONE("DrawLitWorld");

            lit.Capture();

            if (lit.Ready()) render::LitWorld(world, grove, fixtures, player, trail, liquids, world.Light(), camera, debug);

            lit.Finish();
        }

        // And the world itself, when it is about to be put behind a panel. Same
        // constraint, one step further: a texture mode cannot be opened inside a
        // frame, so the scene is drawn into the backdrop's target out here and
        // only the blurred result is drawn once the frame is open.
        if (packOpen) {
            backdrop.Capture();
            render::Scene(world, grove, inventory, player, editor, lit, camera, debug, !packOpen);
            backdrop.Finish();
        }

        BeginDrawing();

        {
            PROFILE_ZONE("DrawScene");

            if (packOpen) backdrop.Compose(config::kPanelDim);
            else render::Scene(world, grove, inventory, player, editor, lit, camera, debug, !packOpen);
        }

        {
            PROFILE_ZONE("DrawHud");

            hud::Draw(world, grove, player, editor, camera, debug, lantern, notice, noticeFor);

            // The panel replaces the bar rather than sitting over it, since it draws
            // those same nine slots as its own bottom row.
            if (packOpen) inventory.Draw(mode);
            else hotbar::Draw(inventory);

            // Over everything, panel and bar included: an answer that arrived behind the
            // inventory is an answer nobody read.
            chat.Draw(dt);
        }

        {
            // The frame handed over and waited on. With a target frame rate set
            // this is also where the slack goes, so it reads as the whole of
            // whatever the rest of the frame did not use.
            PROFILE_ZONE("EndDrawing");

            EndDrawing();
        }
    }

    if (profiling) {
        profile::Report("frame");

        // And beside the executable, on the same terms F2 writes it: a run started
        // from a shortcut has nowhere to print, and the two ways of asking for a
        // profile should leave the same thing behind.
        profile::Write("profile.txt", "frame");
    }

    world.UnloadPainted();
    grove.Unload();
    liquids.Unload();
    lit.Unload();
    backdrop.Unload();
    CloseWindow();

    return 0;
}
