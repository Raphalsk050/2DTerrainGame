#include "probes.h"

#include "config.h"
#include "debug_view.h"
#include "drop.h"
#include "editor.h"
#include "element.h"
#include "fixture.h"
#include "flora.h"
#include "hud.h"
#include "inventory.h"
#include "item.h"
#include "light.h"
#include "marching_squares.h"
#include "picture.h"
#include "player.h"
#include "profile.h"
#include "raylib.h"
#include "render.h"
#include "rlgl.h"
#include "sod.h"
#include "soil.h"
#include "stack.h"
#include "view.h"
#include "water.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

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
void DrawProbe(World &world, Grove &grove, Inventory &gathered, Rectangle strip, const char *path, int zoom,
               float seconds, bool plants, int lit, int mood, int quarter) {
    // Held at one weather and one season where asked for, and this is what makes the
    // probe worth anything for judging the wind: what a gale does is only legible
    // against the calm afternoon beside it, and two pictures taken of whatever the
    // spell happened to be doing are not a comparison. Taken before the clock is run
    // on, so the whole of that run happens under the weather being asked about.
    if (mood >= 0) world.ForceWeather(mood);
    for (int step = 0; step <= quarter; step++) world.CycleSeason();

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

    // The ground's own pictures, before the probe opens a target of its own —
    // and deliberately, so that what this draws is the cached path the game
    // takes rather than the fallback beside it.
    world.PaintChunks(strip);

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

    // The fruit and the falling leaves too, in the frame's own order. Left out of
    // this probe for a long time, which is exactly why the leaf field could be
    // judged only by eye in a live window — the one part of the world whose whole
    // point is how it answers the weather had no still picture to be looked at.
    if (plants) {
        grove.DrawFruit(world.Sky(), season, world.Sky().Time());
        grove.DrawLeaves(world.Sky(), season, strip, world.Sky().Time());
    }

    world.DrawLiquids(strip);

    // What is falling, and then the fog, in the frame's own order.
    //
    // Both are pure functions of the clock and the weather and the probe can force
    // both, so this is the only way to look at a storm — or at a shower coming down
    // as snow, which needs cold country as well as a storm and is otherwise a thing
    // that has to be waited for in two places at once.
    world.DrawRain(strip);
    world.DrawMist(strip);

    // And the light over all of it, which is the other half of every question
    // about how dark something came out: what a material's own tones do and what
    // the multiply does to them are two answers, and tuning either while looking
    // at both together is how a palette ends up fighting a time of day.
    if (lit != 0) {
        world.StepLight(strip);

        // One draws the world as it is seen; the other draws the light on its
        // own, one flat block per probe. The second is the one to reach for when
        // the question is where the light is rather than what it is doing to a
        // palette, because a picture of the two multiplied together cannot answer
        // either of them on its own.
        if (lit == 2) {
            debug_view::DrawLight(world, strip);
        } else {
            render::ComposeLight(world.Light());
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
//
// It also reports the *spread* of that thickness, and that half is the answer to
// a different fault. A cover is a slab as thick as its column, so a table whose
// variation is small draws a band of even depth running the width of the world —
// which reads as a stripe painted under the grass rather than as ground, and
// which no amount of looking at the mean will show. The two figures that catch it
// are the standard deviation and the step between neighbouring columns, the same
// pair `--surface` reports about the ground itself and for the same reason: a
// layer with a mean of thirty-six and a step of a third of a pixel is a spirit
// level, whatever its range happens to be.
void ReportCovers(const terrain::Settings &settings, float fromX, float toX, float step) {
    struct Tally {
        double thickness = 0.0;

        // Sum of squares, for the deviation. Kept rather than a second pass,
        // since the walk is the expensive half and one number does not need one.
        double squared = 0.0;

        float thinnest = kUnboundedDepth;
        float deepest  = 0.0f;
        float where    = 0.0f;
        int columns    = 0;

        // How much the thickness moves between one sampled column and the next.
        // Only counted where the cover is present on both, so the figure is about
        // the floor wandering and not about the material's own border.
        double stepped = 0.0;
        float worstStep = 0.0f;
        int steps       = 0;

        float previous = 0.0f;
        bool had       = false;
    };

    std::array<Tally, kElementCount> tally{};

    float coldest = 1.0f;
    float hottest = 0.0f;
    float driest  = 1.0f;
    float wettest = 0.0f;

    int columns = 0;

    // Columns with nothing over the rock at all. A world with a few is a world
    // with outcrops in it; a world with many has lost its soil somewhere.
    int bare = 0;

    float highest = kUnboundedDepth;
    float lowest  = -kUnboundedDepth;

    for (float x = fromX; x <= toX; x += step, columns++) {
        const terrain::Climate climate = terrain::ClimateAt(x, settings);
        const float surface            = terrain::Height(x, settings);

        coldest = std::min(coldest, climate.temperature);
        hottest = std::max(hottest, climate.temperature);
        driest  = std::min(driest, climate.humidity);
        wettest = std::max(wettest, climate.humidity);

        // Y grows downward, so the highest ground is the smallest number.
        highest = std::min(highest, surface);
        lowest  = std::max(lowest, surface);

        bool covered = false;

        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementSpawn &spawn = kElements[e].spawn;
            if (spawn.generator != Generator::Cover) continue;

            const float thickness = CoverThickness(spawn, x, surface, climate.temperature, climate.humidity);

            Tally &into = tally[e];

            // A cover thinner than one terrain pixel is not on the ground, it is
            // a rounding error, and counting it would report a desert nobody can
            // see.
            if (thickness < config::kPixelSize) {
                into.had = false;
                continue;
            }

            covered = true;

            into.thickness += thickness;
            into.squared += static_cast<double>(thickness) * thickness;
            into.columns++;

            into.thinnest = std::min(into.thinnest, thickness);

            if (thickness > into.deepest) {
                into.deepest = thickness;
                into.where   = x;
            }

            if (into.had) {
                const float moved = std::fabs(thickness - into.previous);

                into.stepped += moved;
                into.worstStep = std::max(into.worstStep, moved);
                into.steps++;
            }

            into.previous = thickness;
            into.had      = true;
        }

        if (!covered) bare++;
    }

    std::printf("%d columns over %.0f px, every %.0f\n", columns, toX - fromX, step);
    std::printf("temperature %.2f..%.2f   humidity %.2f..%.2f\n", coldest, hottest, driest, wettest);

    // The ground's own range, because a cover with a crest is measured against it
    // and a snow line written above the highest peak in the world is a material
    // that never appears — with nothing anywhere to say why.
    std::printf("surface y %.0f..%.0f   (level %.0f, so %.0f px of relief above it)\n", highest, lowest,
                settings.surface.level, settings.surface.level - highest);

    std::printf("bare rock %.1f%% of columns\n\n", 100.0 * bare / std::max(columns, 1));

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].spawn.generator != Generator::Cover) continue;

        const Tally &t = tally[e];

        if (t.columns == 0) {
            std::printf("%-6s  never\n", kElements[e].name);
            continue;
        }

        const double mean     = t.thickness / t.columns;
        const double variance = std::max(t.squared / t.columns - mean * mean, 0.0);

        std::printf("%-6s  %5.1f%% of columns   mean %4.1f px   sd %4.1f px   thinnest %4.1f   deepest %4.1f at x %.0f\n",
                    kElements[e].name, 100.0 * t.columns / std::max(columns, 1), mean, std::sqrt(variance), t.thinnest,
                    t.deepest, t.where);

        // The raggedness, and it is the figure the mean cannot give. Reported per
        // sampled column, so it is only comparable between runs walked at the same
        // step — walk it at the lattice spacing to ask what the world is built at.
        if (t.steps > 0) {
            std::printf("        floor moves %.2f px mean, %.1f px worst, between columns %.0f px apart\n",
                        t.stepped / t.steps, t.worstStep, step);
        }
    }
}

// What the scatter actually grows over a stretch of world, and what it grows it
// on.
//
// The companion to ReportCovers, and it exists for exactly the fault ReportCovers
// exists for: a placement rule written into a table is very easy to author into a
// wood that is never there or a desert that is quietly full of oaks, and nothing
// errors either way. The only symptom of the second is a screenshot, which nobody
// takes of the one stretch of world where it is wrong.
//
// It walks the cells rather than the columns, because a cell is what the scatter
// decides one plant per, and it reports the ground under each plant so that
// "trees in the desert" is a number rather than an impression.
void ReportWoods(const flora::Settings &flora, const terrain::Settings &terrain, float fromX, float toX) {
    struct Tally {
        int grown = 0;
        int onSoil = 0;
        int onSand = 0;
        int onSnow = 0;
        int onRock = 0;
    };

    std::array<Tally, flora::kSpeciesCount> tally{};

    // The scatter wants a surface to stand its plants on. Taken from the shape of
    // the land alone rather than from a world, since what is being measured is the
    // placement rule and not what anybody has dug.
    constexpr float kStep = 6.0f;

    const int columns = static_cast<int>((toX - fromX) / kStep) + 3;

    std::vector<float> top(static_cast<std::size_t>(columns));

    for (int i = 0; i < columns; i++) {
        top[static_cast<std::size_t>(i)] = terrain::Height(fromX + static_cast<float>(i) * kStep, terrain);
    }

    const flora::Ground ground = {
        .top = top.data(), .sunk = nullptr, .count = columns, .originX = fromX, .spacing = kStep};

    std::array<int, kElementCount + 1> cells{};

    std::vector<flora::Plant> plants;

    for (std::size_t l = 0; l < flora::kLayerCount; l++) {
        const auto layer = static_cast<flora::Layer>(l);

        flora::Scatter(layer, fromX, toX, flora, terrain, ground, plants);

        for (const flora::Plant &plant : plants) {
            Tally &into = tally[flora::SpeciesIndex(plant.species)];

            into.grown++;

            const std::optional<Element> cover = SurfaceCoverAt(plant.base.x, terrain);

            if (!cover.has_value()) into.onRock++;
            else if (*cover == Element::Soil) into.onSoil++;
            else if (*cover == Element::Sand) into.onSand++;
            else if (*cover == Element::Snow) into.onSnow++;
        }
    }

    // And how much of the world is each kind of ground, so a species count can be
    // read against the room there was for it.
    int walked = 0;

    for (float x = fromX; x <= toX; x += 40.0f, walked++) {
        const std::optional<Element> cover = SurfaceCoverAt(x, terrain);

        cells[cover.has_value() ? ElementIndex(*cover) : kElementCount]++;
    }

    std::printf("%.0f px of world   soil %.1f%%  sand %.1f%%  snow %.1f%%  bare %.1f%%\n\n", toX - fromX,
                100.0 * cells[ElementIndex(Element::Soil)] / std::max(walked, 1),
                100.0 * cells[ElementIndex(Element::Sand)] / std::max(walked, 1),
                100.0 * cells[ElementIndex(Element::Snow)] / std::max(walked, 1),
                100.0 * cells[kElementCount] / std::max(walked, 1));

    for (std::size_t e = 0; e < flora::kSpeciesCount; e++) {
        const Tally &t = tally[e];

        std::printf("%-6s  %6d grown   on soil %6d   sand %6d   snow %6d   bare rock %6d\n", flora::kSpecies[e].name,
                    t.grown, t.onSoil, t.onSand, t.onSnow, t.onRock);
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
// Whether every plant the table can grow fits in the slot it will be drawn into.
//
// This exists because of a fault that is invisible from every other angle. A
// plant is rasterised into a fixed slot of the sprite sheet, and the canvas is
// clamped to that slot — but the *anchor*, which is where the trunk foot sits
// inside the sprite and therefore the whole of how the plant is positioned in the
// world, is worked out from the plant's own extent. While a plant fits, the two
// agree. Once it outgrows the slot the drawing is cropped and the anchor goes on
// pointing at where the foot would have been, so the plant is drawn hanging off
// its own base by exactly the overflow.
//
// Nothing about that errors, nothing draws a wrong colour, and no small plant
// shows it: a sapling fits, so a sapling is seated correctly, and the error
// arrives gradually as the tree grows into its mature size. What it looks like is
// a tree standing in the wrong place, which is why it was reported as a placement
// bug and looked for in the placement code.
//
// Walked over the scale range the scatter actually rolls — see flora.cpp, which
// takes (0.86 + 0.28 r) times (0.78 + 0.22 s), so 1.14 is the largest plant the
// world can contain — and over many ids, since the skeleton is jittered per plant
// and the widest one is not the median one.
void ReportSprites() {
    constexpr int kIds = 400;

    // The two ends of the scatter's own roll. Nothing outside this is reachable,
    // and measuring outside it would size the sheet for plants that never grow.
    constexpr float kLeastScale = 0.86f * 0.78f;
    constexpr float kMostScale  = 1.14f;

    const int slotW = canopy::SlotWidth();
    const int slotH = canopy::SlotHeight();

    std::printf("slot %d x %d texels, at %.0f px each: %.0f x %.0f world px\n\n", slotW, slotH, config::kFloraPixel,
                slotW * config::kFloraPixel, slotH * config::kFloraPixel);

    bool overflowed = false;

    for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
        const auto species = static_cast<flora::Species>(s);

        int worstW = 0;
        int worstH = 0;
        int worstStage = 0;

        for (int stage = 0; stage < flora::kStageCount; stage++) {
            for (int id = 0; id < kIds; id++) {
                // Both ends of the roll rather than the top alone: a plant's
                // widest part is not always its tallest, and the crown of a small
                // one can still reach further sideways than the trunk of a big one.
                for (const float scale : {kLeastScale, kMostScale}) {
                    const flora::Skeleton skeleton =
                        flora::Build(species, static_cast<flora::Stage>(stage), id, scale);

                    float top    = skeleton.height;
                    float bottom = 0.0f;

                    for (int i = 0; i < skeleton.lobeCount; i++) {
                        const flora::Lobe &lobe = skeleton.lobes[i];
                        const float reach       = lobe.radius * lobe.flatten;

                        top    = std::max(top, lobe.at.y + reach);
                        bottom = std::min(bottom, lobe.at.y - reach);
                    }

                    // Exactly the arithmetic canopy::Render does, padding included,
                    // or this measures a different question than the one that bites.
                    const int wantW =
                        static_cast<int>(std::ceil((skeleton.right - skeleton.left) / config::kFloraPixel))
                        + canopy::kSpritePad * 2;
                    const int wantH = static_cast<int>(std::ceil((top - bottom) / config::kFloraPixel))
                                    + canopy::kSpritePad * 2;

                    if (wantH > worstH || (wantH == worstH && wantW > worstW)) worstStage = stage;

                    worstW = std::max(worstW, wantW);
                    worstH = std::max(worstH, wantH);
                }
            }
        }

        const bool fits = worstW <= slotW && worstH <= slotH;
        if (!fits) overflowed = true;

        std::printf("%-8s worst %3d x %3d texels at stage %d   %s\n", flora::Def(species).name, worstW, worstH,
                    worstStage, fits ? "fits" : "OVERFLOWS — cropped");

        // And then the question the sizes are only a proxy for: is the foot of the
        // drawn plant where the anchor says it is?
        //
        // Rasterised for real and read back, because that is the only way to ask.
        // The anchor is a number computed from the skeleton and the drawing is
        // made by a different pass over the same skeleton, so the two agreeing is
        // a claim about the code rather than about the table — and it is exactly
        // the claim that fails when a plant is cropped, since cropping moves the
        // drawing and leaves the anchor where it was.
        for (int stage = 0; stage < static_cast<int>(flora::kStageCount); stage++) {
            flora::Plant plant;
            plant.id      = 7;
            plant.species = species;
            plant.scale   = kMostScale;

            std::vector<Color> pixels;
            int w = 0;
            int h = 0;
            Vector2 anchor{};

            canopy::Render(plant, static_cast<flora::Stage>(stage), flora::Season::Summer, false, pixels, w, h,
                           anchor);

            int lowest = -1;

            for (int y = h - 1; y >= 0 && lowest < 0; y--) {
                for (int x = 0; x < w; x++) {
                    if (pixels[static_cast<std::size_t>(y) * w + x].a != 0) {
                        lowest = y;
                        break;
                    }
                }
            }

            // Printed as a number rather than as a pass, because the interesting
            // value is not zero and never was. A plant is *allowed* to be drawn
            // below its own base — foliage hangs, and a fern's fronds reach the
            // ground on both sides of where it is rooted — so what this reports is
            // how far below, and the judgement about whether that is foliage or a
            // trunk is one only a person looking at the plant can make.
            //
            // Negative is the drawing stopping short of the anchor, which is the
            // one direction that cannot be foliage: nothing hangs *upward*, so a
            // plant whose lowest drawn texel is above its own base is a plant
            // standing on nothing.
            const float below = (static_cast<float>(lowest) - anchor.y) * config::kFloraPixel;

            std::printf("         stage %d: anchor row %5.1f, lowest drawn row %3d of %3d, %+.0f px below the base\n",
                        stage, anchor.y, lowest, h, below);

            if (anchor.y > static_cast<float>(h)) overflowed = true;
        }
    }

    std::printf("\n%s\n", overflowed ? "at least one plant does not fit its slot" : "every plant fits its slot");
}

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

    // How far the daylight reaches down each column, which is the one thing about
    // the sun that is still a number rather than a picture.
    //
    // It used to report the two terms of a sweep -- how deep the sun was carried and
    // what it was carried at -- because those were separate inventions and either
    // could be the one that was wrong. There is no sweep now and no sun term: light
    // gets down a column by being transported down it, so what is left to ask is how
    // far it got, and the answer is one column of the field read downwards.
    constexpr float kDim = 0.05f;

    std::printf("%8s %8s %8s %8s\n", "x", "top", "depth", "at depth");

    for (int i = 0; i < field.Cols(); i++) {
        const Vector2 head = field.ProbePosition(i, 0);
        if (head.x < region.x || head.x > region.x + region.width) continue;

        const float top = light::Expose(light::Luminance(field.ProbeAt(i, 0)), field.Exposure());

        int fell = -1;

        for (int j = 0; j < field.Rows(); j++) {
            const float level =
                light::Expose(light::Luminance(field.ProbeAt(i, j)), field.Exposure());

            if (level < kDim) {
                fell = j;
                break;
            }
        }

        if (fell < 0) {
            std::printf("%8.0f %8.3f %8s %8s\n", head.x, top, "-", "-");
            continue;
        }

        const Vector2 at = field.ProbePosition(i, fell);

        std::printf("%8.0f %8.3f %8.0f %8.3f\n", head.x, top, at.y - head.y,
                    light::Expose(light::Luminance(field.ProbeAt(i, fell)), field.Exposure()));
    }
}

// The wind each kind of weather actually produces, and what it does to the things
// that read it.
//
// This exists because the wind is the one field in the world whose whole purpose is
// a *difference* between two weathers, and a difference is the hardest thing to
// judge by eye — a wood in a gale looks windy on its own; only the calm afternoon
// beside it says whether the gale is doing anything. The failure this is built to
// catch is exactly that: the shares everything rooted to the ground reads were once
// normalised by the current weather's own envelope, so a storm and a clear day both
// came back about a half and every gale in the world was drawn as a breeze.
//
// The last two columns are the ones with a floor under them. The sway is drawn in
// whole plant texels, so a crown or a blade moving less than one of them does not
// move at all — a calm row whose lean rounds to zero is a wood that stands frozen,
// and no amount of looking at a storm will show it.
void ReportWind(World &world, Grove &grove, Inventory &gathered, Rectangle strip) {
    const float pixel = config::kFloraPixel;

    // The tallest thing that sways, so the sway is judged where it is largest. A
    // shorter tree moves proportionally less and hits the texel floor sooner.
    float tallest = 0.0f;

    for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
        const flora::SpeciesDef &def = flora::Def(static_cast<flora::Species>(s));
        tallest = std::max(tallest, def.height[flora::StageIndex(flora::Stage::Mature)]);
    }

    std::printf("gale %.1f px/s   tallest crown %.0f px   plant texel %.0f px\n\n", world.Sky().Gale(), tallest, pixel);

    std::printf("%-9s %7s %15s %9s %9s %9s %9s\n", "mood", "mean", "|push|", "held", "shake", "grass", "leaf");
    std::printf("%-9s %7s %15s %9s %9s %9s %9s\n", "", "px/s", "min..max", "px max", "px min", "px min", "px max");

    for (int mood = 0; mood < weather::kMoodCount; mood++) {
        world.ForceWeather(mood);
        world.StepWeather(1.0f / 60.0f);

        const weather::Sky &sky = world.Sky();

        float lowest = 1.0f, highest = 0.0f;

        // How far the wind holds a crown over, and how far it shakes one. They are
        // measured apart because they fail apart: the hold is what says a gale is
        // blowing, and the shake is the one that has to clear the texel grid or a
        // calm afternoon is a photograph.
        // The hold is taken at its largest and the shake at its smallest, because
        // that is where each one fails: a gale is judged by how far over it holds a
        // crown at its hardest, and a calm is judged by whether anything is still
        // moving at its quietest.
        float held  = 0.0f;
        float shook = 1e9f, blade = 1e9f;
        float leaf  = 0.0f;

        // Walked over both axes the field varies on. A gust is a wave crossing the
        // world, so a single column at a single moment is one point of it and says
        // nothing about the range; and the quarter turns on a clock of its own, so a
        // sweep that did not also run the clock would miss the lull entirely.
        // Long enough to carry the quarter round several whole turns, and stepped
        // finely enough not to skip over a gust crest on the way. A sweep shorter
        // than the backing period reports whatever the wind happened to be doing
        // during it and calls that the range.
        constexpr int kColumns = 64;
        constexpr int kSteps   = 2400;

        for (int step = 0; step < kSteps; step++) {
            for (int i = 0; i < kColumns; i++) {
                const float x = static_cast<float>(i) * 137.0f;

                const float push = sky.Stir(x);

                lowest  = std::min(lowest, push);
                highest = std::max(highest, push);

                // What the shares are worth once something reads them. Trees and
                // grass take the share; anything in flight takes the speed.
                //
                // The quiver keeps its floor here exactly as it does where it is
                // drawn, and that is the point of measuring rather than multiplying
                // out an envelope: scaled by the wind alone it would report zero in
                // dead air, which is the one reading that must not be wrong.
                const float quiver = kSwayIdle + (1.0f - kSwayIdle) * std::sqrt(push);
                const float ripple = sod::kBladeIdle + (1.0f - sod::kBladeIdle) * std::sqrt(push);

                held  = std::max(held, tallest * kSwayHold * push);
                shook = std::min(shook, tallest * kSwaySwing * quiver * 2.0f);

                blade = std::min(blade, static_cast<float>(sod::kBladeTall) * pixel * sod::kBladeSwing * ripple * 2.0f);

                // A leaf off the tallest crown, which is the longest fall and so
                // the furthest the air can take one.
                leaf = std::max(leaf, std::fabs(weather::Carry(sky.WindAt(x), 1.0f, tallest / kLeafFall,
                                                               weather::kLeafDrag)));
            }

            // Long enough a run to carry the quarter round through a lull, or the
            // calm end of every row would be whatever the bearing happened to be.
            world.StepWeather(4.0f);
        }

        std::printf("%-9s %7.1f %6.2f ..%6.2f %9.1f %9.1f %9.1f %9.0f\n", sky.Now().name, sky.Now().wind, lowest,
                    highest, held, shook, blade, leaf);
    }

    // And what the wood actually sheds under each of them, counted rather than
    // reasoned about.
    //
    // Every season against every weather, because the two failures this is built to
    // catch are both invisible in any single cell of the table: a wood that sheds
    // the same number of leaves in a gale as in still air, and one that only sheds
    // at all in autumn. Neither shows up while looking at one afternoon, and both
    // have happened here.
    std::printf("\nleaves adrift over a %.0f px view of wood\n\n", strip.width);

    std::printf("%-10s", "season");
    for (int mood = 0; mood < weather::kMoodCount; mood++) {
        world.ForceWeather(mood);
        world.StepWeather(1.0f / 60.0f);
        std::printf("%10s", world.Sky().Now().name);
    }
    std::printf("\n");

    for (int quarter = 0; quarter < flora::kSeasonCount; quarter++) {
        world.CycleSeason();

        std::printf("%-10s", flora::kSeasonNames[world.Sky().Turn().index % 4]);

        for (int mood = 0; mood < weather::kMoodCount; mood++) {
            world.ForceWeather(mood);

            // Counted over a run rather than on one frame. A gust is a wave and the
            // quarter turns behind it, so a single frame is one point of the range
            // and reports it as the whole.
            int most = 0;

            for (int step = 0; step < 900; step++) {
                world.StepWeather(2.5f);
                world.Update(strip);
                grove.Update(world, strip, {strip.x, strip.y}, world.Sky().Time(), 1.0f / 60.0f, gathered);

                const auto turn = static_cast<flora::Season>(world.Sky().Turn().index);

                grove.DrawLeaves(world.Sky(), turn, strip, world.Sky().Time());

                most = std::max(most, grove.Drifting());
            }

            std::printf("%10d", most);
        }

        std::printf("\n");
    }

    // Back to whatever the clock says, so this leaves nothing held.
    world.ForceWeather(-1);
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

    // Read and discarded, so that what is reported below is this scan's own work
    // rather than the world's calibration, which samples scattered points at
    // startup and misses every one of them by construction.
    terrain::Effort();
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

    {
        const terrain::Work work = terrain::Effort();

        std::printf("cave memo\n");
        std::printf("  %ld lookups, %ld rebuilt (%.1f%% missed), %ld placement tests\n", work.asked, work.built,
                    100.0 * static_cast<double>(work.built) / std::max(work.asked, 1L), work.sited);
        std::printf("  %.1f lookups and %.2f rebuilds per sample\n\n",
                    static_cast<double>(work.asked) / std::max(count, std::size_t{1}),
                    static_cast<double>(work.built) / std::max(count, std::size_t{1}));
    }

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

// Where a frame's time actually goes, at a place in the world.
//
// Every phase the loop runs, in the order it runs them, timed separately. It
// exists because a frame that has gone slow says nothing about which of six
// things did it, and the two costs worth finding — the ones that grow with
// distance from the surface — are invisible from any single-phase measurement.
//
// Run twice over: the first pass pays for whatever the region needed generating,
// which is a real cost but a one-off, and the second is what a frame standing
// still actually costs.
void ReportFrame(World &world, Grove &grove, Inventory &gathered, Vector2 at, int frames) {
    const Rectangle view   = {at.x - 500.0f, at.y - 300.0f, 1000.0f, 600.0f};
    const Rectangle active = view::Expand(view, config::kSimulationMargin);

    constexpr float kStep = 1.0f / 60.0f;

    std::printf("at (%.0f, %.0f), surface y %.0f, %d frames\n\n", at.x, at.y,
                terrain::Height(at.x, world.Settings()), frames);
    std::printf("%-14s %10s %10s\n", "", "first ms", "then ms");

    struct Phase {
        const char *name;
        double first;
        double rest;
    };

    std::array<Phase, 5> phases = {{{"world.Update", 0, 0},
                                    {"grove.Update", 0, 0},
                                    {"StepWater", 0, 0},
                                    {"StepWeather", 0, 0},
                                    {"StepLight", 0, 0}}};

    profile::Begin();

    for (int f = 0; f < frames; f++) {
        // The first frame generates the chunks the rest of them read, so it is
        // measured separately above and thrown away here. Everything the report
        // is about is the steady state.
        if (f == 1) profile::Reset();

        profile::Frame();

        double marks[5]{};

        double t0 = GetTime();
        world.Update(active);
        marks[0] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        grove.Update(world, view, at, world.Sky().Time(), kStep, gathered);
        marks[1] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        world.StepWater(active);
        marks[2] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        world.StepWeather(kStep);
        marks[3] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        world.StepLight(active);
        marks[4] = (GetTime() - t0) * 1000.0;

        for (std::size_t p = 0; p < phases.size(); p++) {
            if (f == 0) {
                phases[p].first = marks[p];
            } else {
                phases[p].rest += marks[p];
            }
        }
    }

    double total = 0.0;

    for (const Phase &phase : phases) {
        const double rest = phase.rest / std::max(frames - 1, 1);

        total += rest;

        std::printf("%-14s %10.2f %10.2f\n", phase.name, phase.first, rest);
    }

    std::printf("%-14s %10s %10.2f   (%.0f fps)\n\n", "steady frame", "", total, (total > 0.0) ? 1000.0 / total : 0.0);
    std::printf("chunks resident %d, pinned %d\n", world.ResidentChunks(), world.PinnedChunks());

    profile::Report("phases");
}


} // namespace

bool probes::Headless(int argc, char **argv) {
    // `--probe x y w h out.png` draws one strip of the world to a file and exits.
    // See DrawProbe. Read here rather than after the window opens, so the probe
    // can ask for a hidden one: it wants a GL context and nothing else.
    const bool probing = argc >= 7 && TextIsEqual(argv[1], "--probe");

    // `--covers x0 x1 step` walks the columns and reports what each cover claims.
    // See ReportCovers.
    const bool counting = argc >= 5 && TextIsEqual(argv[1], "--covers");

    // `--woods x0 x1` walks the scatter's cells and reports what grows where. See
    // ReportWoods.
    const bool cruising = argc >= 4 && TextIsEqual(argv[1], "--woods");

    // `--sprites` reports whether every plant fits the slot it is drawn into. See
    // ReportSprites.
    const bool sizing = argc >= 2 && TextIsEqual(argv[1], "--sprites");

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

    // `--frame x y [frames]` times every phase of the loop at a place. See
    // ReportFrame.
    const bool timing = argc >= 4 && TextIsEqual(argv[1], "--frame");

    // Reports a table and draws nothing, so it wants no window on screen either.
    const bool gauging = argc >= 2 && TextIsEqual(argv[1], "--wind");

    // `--sodcheck [frames]` walks a view across the world and checks the band it
    // remembers against one built from cold.
    const bool checking = argc >= 2 && TextIsEqual(argv[1], "--sodcheck");

    // `--build [cells]` sets a run of cells and digs it back out, checking that
    // the exchange is exact. See the block itself for why it is worth a mode.
    const bool building = argc >= 2 && TextIsEqual(argv[1], "--build");


    return probing || counting || cruising || weighing || reading || digging || assaying || settling || timing
        || gauging || checking || building;
}

std::optional<int> probes::Run(int argc, char **argv, World &world, Grove &grove, terrain::Settings &settings,
                               const weather::Settings &sky) {
    (void)sky;

    // `--probe x y w h out.png` draws one strip of the world to a file and exits.
    // See DrawProbe. Read here rather than after the window opens, so the probe
    // can ask for a hidden one: it wants a GL context and nothing else.
    const bool probing = argc >= 7 && TextIsEqual(argv[1], "--probe");

    // `--covers x0 x1 step` walks the columns and reports what each cover claims.
    // See ReportCovers.
    const bool counting = argc >= 5 && TextIsEqual(argv[1], "--covers");

    // `--woods x0 x1` walks the scatter's cells and reports what grows where. See
    // ReportWoods.
    const bool cruising = argc >= 4 && TextIsEqual(argv[1], "--woods");

    // `--sprites` reports whether every plant fits the slot it is drawn into. See
    // ReportSprites.
    const bool sizing = argc >= 2 && TextIsEqual(argv[1], "--sprites");

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

    // `--frame x y [frames]` times every phase of the loop at a place. See
    // ReportFrame.
    const bool timing = argc >= 4 && TextIsEqual(argv[1], "--frame");

    // Reports a table and draws nothing, so it wants no window on screen either.
    const bool gauging = argc >= 2 && TextIsEqual(argv[1], "--wind");

    // `--sodcheck [frames]` walks a view across the world and checks the band it
    // remembers against one built from cold.
    const bool checking = argc >= 2 && TextIsEqual(argv[1], "--sodcheck");

    // `--build [cells]` sets a run of cells and digs it back out, checking that
    // the exchange is exact. See the block itself for why it is worth a mode.
    const bool building = argc >= 2 && TextIsEqual(argv[1], "--build");


    if (argc >= 4 && TextIsEqual(argv[1], "--sun")) {
        const float x = static_cast<float>(std::atof(argv[2]));
        const float y = static_cast<float>(std::atof(argv[3]));

        const Rectangle region = {x, y, 900.0f, 400.0f};

        world.StepWeather(1.0f / 60.0f);
        for (int step = 0; step < 400 * 60; step++) world.StepWeather(1.0f / 60.0f);

        world.Update(region);
        world.StepLight(region);

        ReportSun(world, region);

        return 0;
    }

    if (gauging) {
        // Over the birch wood rather than the origin: the leaf field needs
        // deciduous trees to come off, and the trees at the origin are pines.
        Inventory gauged{};

        ReportWind(world, grove, gauged, {1800.0f, -260.0f, 900.0f, 400.0f});

        return 0;
    }

    if (argc >= 4 && TextIsEqual(argv[1], "--surface")) {
        terrain::Settings tuned = settings;

        // The knob under test, overridable from the command line so a sweep is a
        // loop in a shell rather than a rebuild each time.
        if (argc >= 5) tuned.surface.terraceSharp = static_cast<float>(std::atof(argv[4]));
        if (argc >= 6) tuned.surface.terrace = static_cast<float>(std::atof(argv[5]));

        ReportSurface(tuned, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])));

        return 0;
    }

    if (weighing) {
        ReportTones();

        return 0;
    }

    if (sizing) {
        ReportSprites();

        return 0;
    }

    if (counting) {
        ReportCovers(settings, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                     static_cast<float>(std::atof(argv[4])));

        return 0;
    }

    if (cruising) {
        // The grove's own settings, calibrated: the coverage figures are measured
        // cutoffs and a scatter run against uncalibrated ones is a scatter of a
        // different world.
        ReportWoods(grove.Settings(), settings, static_cast<float>(std::atof(argv[2])),
                    static_cast<float>(std::atof(argv[3])));

        return 0;
    }

    if (argc >= 2 && TextIsEqual(argv[1], "--sodcheck")) {
        // Walks two identical worlds along the same path, one remembering its
        // grass band between frames and one made to work the whole band out
        // again, and reports any column they disagree about.
        //
        // Both worlds see exactly the same chunks come and go, which is what
        // makes the comparison about the memory and nothing else: a column whose
        // chunk is resident is answered by interpolating the field and one whose
        // chunk is not is answered from the noise, so two worlds with different
        // chunks resident differ for reasons that have nothing to do with this.
        const int frames = (argc >= 3) ? std::atoi(argv[2]) : 400;

        World plain(settings, config::kResolution);
        plain.SetWeather(sky);

        int checked = 0;
        int wrong   = 0;
        float worst = 0.0f;

        for (int f = 0; f < frames; f++) {
            // Deliberately not a whole number of columns, chunks or tufts, so the
            // band lands out of step with every grid it is built on — and far
            // enough each frame that the walk crosses chunk borders, which is
            // what brings new ground into the band.
            const Rectangle view = {static_cast<float>(f) * 53.0f - 500.0f, -300.0f, 1000.0f, 600.0f};

            world.Update(view);

            plain.ForgetGrass();
            plain.Update(view);

            const sod::Blades kept  = world.Grass();
            const sod::Blades again = plain.Grass();

            if (kept.count != again.count || kept.firstColumn != again.firstColumn) {
                std::printf("frame %d: band differs (%d at %d against %d at %d)\n", f, kept.count, kept.firstColumn,
                            again.count, again.firstColumn);
                wrong++;
                continue;
            }

            for (int i = 0; i < kept.count; i++) {
                const float dTop   = std::fabs(kept.top[i] - again.top[i]);
                const float dCover = std::fabs(kept.cover[i] - again.cover[i]);

                checked++;

                worst = std::max({worst, dTop, dCover});

                if (dTop > 0.0f || dCover > 0.0f) {
                    if (wrong < 10) {
                        std::printf("frame %d column %d: top %.4f against %.4f, cover %.2f against %.2f\n", f,
                                    kept.firstColumn + i, kept.top[i], again.top[i], kept.cover[i], again.cover[i]);
                    }

                    wrong++;
                }
            }
        }

        std::printf("\n%d columns checked over %d frames, %d differ, worst %.6f\n", checked, frames, wrong, worst);

        return (wrong == 0) ? 0 : 1;
    }

    if (argc >= 2 && TextIsEqual(argv[1], "--build")) {
        // Sets a run of cells into open sky, digs the same run back out, and
        // checks that the world gave back exactly what it was given.
        //
        // It exists because the exchange is the one part of building that cannot
        // be seen. A wall that is a pixel out is obvious the moment it is looked
        // at; a wall that quietly pays back a block more than it cost is a way of
        // making planks out of nothing, and it looks exactly like a wall. The rate
        // is kVerticesPerBlock against config::kBuildCellArea, and those are two
        // numbers in two files that have to stay equal — see the static_assert in
        // stack.h, which catches them being unequal but not a cell that fails to
        // fill.
        //
        // Sky rather than ground, and high above it: a cell set into a hillside
        // hands back what it displaced as well, which is correct and is a
        // different sum. Here the only material in play is the one being tested.
        const int cells = (argc >= 3) ? std::atoi(argv[2]) : 24;

        const Element what = Element::WoodPlank;
        const std::size_t e = ElementIndex(what);

        // Well clear of any terrain, so every cell starts empty.
        const int cy = -40;

        world.Update({0.0f, static_cast<float>(cy) * config::kBuildCell - 400.0f, cells * config::kBuildCell + 800.0f,
                      1200.0f});

        int laid   = 0;
        int filled = 0;

        for (int i = 0; i < cells; i++) {
            const World::Stroke set = world.PlaceCell(what, i, cy);

            if (set.filled > 0) laid++;

            filled += set.filled;
        }

        int freed = 0;

        for (int i = 0; i < cells; i++) {
            freed += world.ExcavateCell(i, cy).freed[e];
        }

        const float cost = static_cast<float>(filled) / kVerticesPerBlock;
        const float back = static_cast<float>(freed) / kVerticesPerBlock;

        std::printf("cell %d px, %d vertices, %.0f per block\n", config::kBuildCell, config::kBuildCellArea,
                    kVerticesPerBlock);
        std::printf("%d cells asked for, %d took a piece\n", cells, laid);
        std::printf("vertices: %d filled, %d freed\n", filled, freed);
        std::printf("blocks:   %.3f spent, %.3f returned\n", cost, back);

        // And that the square the player is shown is the square the piece lands
        // in. This is the other half of building and it is the half that was
        // wrong: the grid was ruled on multiples of the cell side and the block
        // landed on the vertices, which sit half a lattice step inside that — so
        // every piece appeared three pixels up and to the left of the outline that
        // promised it. Nothing about the exchange above catches that.
        //
        // Checked as a round trip. Every point inside a cell's own bounds has to
        // name that cell, and the vertices the cell writes have to lie within
        // them.
        bool square = true;

        for (int i = 0; i < cells; i++) {
            const Rectangle at = World::CellBounds(i, cy);

            // Inset by a hair, since the far edge belongs to the cell beyond.
            const float in = 0.05f;

            const Vector2 corners[] = {{at.x + in, at.y + in},
                                       {at.x + at.width - in, at.y + in},
                                       {at.x + in, at.y + at.height - in},
                                       {at.x + at.width - in, at.y + at.height - in},
                                       {at.x + at.width * 0.5f, at.y + at.height * 0.5f}};

            for (const Vector2 &corner : corners) {
                int bx = 0;
                int by = 0;
                World::ToCell(corner, bx, by);

                if (bx != i || by != cy) {
                    if (square) {
                        std::printf("WRONG: (%.2f, %.2f) is inside cell %d,%d but names %d,%d\n", corner.x, corner.y, i,
                                    cy, bx, by);
                    }

                    square = false;
                }
            }

            // The three vertices across, and the square each of them owns.
            const float half = config::kResolution / 2.0f;

            for (int v = 0; v < config::kBuildCellVertices; v++) {
                const float x = static_cast<float>(i * config::kBuildCellVertices + v) * config::kResolution;

                if (x - half < at.x - 0.001f || x + half > at.x + at.width + 0.001f) {
                    if (square) std::printf("WRONG: vertex at %.2f is outside cell %d's bounds %.2f..%.2f\n", x, i, at.x,
                                            at.x + at.width);

                    square = false;
                }
            }
        }

        // And that every cell is *drawn* the same size.
        //
        // The field says a block is a block, and the rasteriser is what the player
        // actually sees. It anchors its squares to the world in multiples of
        // kPixelSize and gives each lattice cell the squares whose centres fall
        // inside it — see marching_squares::DrawPainted. So a run of world that is
        // not a whole number of squares comes out as a different number of them
        // depending on where it falls, and two identical blocks are drawn
        // different sizes. That is what a wall with bites out of it is.
        //
        // Counted exactly as DrawPainted counts it, so this is the drawing and not
        // a model of it.
        // Counting the squares is not enough, and this is the trap: a size can give
        // every cell the same *number* of squares and still start each one in the
        // wrong place. The run has to begin and end exactly on the cell's own
        // edges, or the block is drawn beside the square it was promised — which is
        // the same complaint as the preview being out, arriving by a different
        // road.
        //
        // What that needs is for kPixelSize to divide the cell *and* the half-step
        // it is offset by, so the only sizes that work at all are the divisors of
        // kCellOffset.
        const float pixel = Def(what).paint.texel;

        const auto drawnSpan = [pixel](float from, float span, float &outFrom, float &outTo) {

            const int m0 = static_cast<int>(std::floor(from / pixel));
            const int m1 = static_cast<int>(std::ceil((from + span) / pixel));

            int count = 0;

            for (int m = m0; m <= m1; m++) {
                const float centre = (static_cast<float>(m) + 0.5f) * pixel;
                if (centre < from || centre >= from + span) continue;

                if (count == 0) outFrom = static_cast<float>(m) * pixel;

                outTo = static_cast<float>(m + 1) * pixel;
                count++;
            }

            return count;
        };

        int fewest  = 1 << 30;
        int most    = 0;
        float slip  = 0.0f;

        for (int i = 0; i < cells; i++) {
            const Rectangle at = World::CellBounds(i, cy);

            float drawnFrom = 0.0f;
            float drawnTo   = 0.0f;

            const int across = drawnSpan(at.x, at.width, drawnFrom, drawnTo);

            fewest = std::min(fewest, across);
            most   = std::max(most, across);

            slip = std::max({slip, std::fabs(drawnFrom - at.x), std::fabs(drawnTo - (at.x + at.width))});
        }

        const bool steady = (fewest == most) && slip < 0.001f;

        std::printf("drawn:    %d..%d squares of %.0f px across a %d px cell, edges out by %.1f px\n", fewest, most,
                    pixel, config::kBuildCell, slip);

        if (fewest != most) {
            std::printf("WRONG: identical blocks are drawn %d and %d squares wide — %.0f does not divide %d\n", fewest,
                        most, pixel, config::kBuildCell);
        } else if (slip >= 0.001f) {
            std::printf("WRONG: blocks are drawn %.1f px off their own cell — %.0f does not divide the %.0f px"
                        " half-step the grid is offset by\n",
                        slip, pixel, config::kResolution / 2.0f);
        }

        const bool whole = (filled == cells * config::kBuildCellArea);
        const bool even  = (filled == freed);

        if (!whole) std::printf("WRONG: a cell did not fill its own %d vertices\n", config::kBuildCellArea);
        if (!even) std::printf("WRONG: digging returned %d vertices against %d laid\n", freed, filled);

        // The wall and the block that shares its cell — the one pair in the world
        // that occupies the same place, and the only thing here a journal of one
        // material per vertex could not have described.
        //
        // Three things to get wrong, all of them silent: the wall could be wiped
        // out by the block going in front of it, the spade could take both at once
        // or neither, and the pair could survive in memory but not survive the
        // chunk being dropped and rebuilt.
        const int wx = cells + 4;

        const Rectangle wall = World::CellBounds(wx, cy);
        const Vector2 middle = {wall.x + wall.width * 0.5f, wall.y + wall.height * 0.5f};

        world.PlaceCell(Element::WoodWall, wx, cy);
        world.PlaceCell(Element::WoodPlank, wx, cy);

        const bool together = world.WalledAt(wx, cy) && world.OccupantAt(middle) == Element::WoodPlank;

        // Walked far enough away that the chunk is released, then back — the same
        // check planning.md §4.5 describes for an ordinary edit.
        world.Update({static_cast<float>(wx) * config::kBuildCell + 12000.0f, 0.0f, 1000.0f, 600.0f});
        world.Update({static_cast<float>(wx) * config::kBuildCell - 500.0f, middle.y - 300.0f, 1000.0f, 600.0f});

        const bool kept = world.WalledAt(wx, cy) && world.OccupantAt(middle) == Element::WoodPlank;

        // The spade takes the block first and the wall only once the block is gone.
        const World::Stroke first = world.ExcavateCell(wx, cy);

        const bool blockFirst = first.freed[ElementIndex(Element::WoodPlank)] > 0
                             && first.freed[ElementIndex(Element::WoodWall)] == 0 && world.WalledAt(wx, cy);

        const World::Stroke second = world.ExcavateCell(wx, cy);

        const bool wallAfter = second.freed[ElementIndex(Element::WoodWall)] > 0 && !world.WalledAt(wx, cy);

        // And the same pair where the block in front was never placed by anybody —
        // a wall put up against a hillside the generator made.
        //
        // The case above cannot catch this one, and the difference is the whole of
        // what a journal record means. There the front layer has a record of its
        // own, so replaying it is right; here it has none, and the wall's record
        // says nothing about it. Read as saying the front is *empty* — which is
        // what an absent `std::optional` was taken to mean — the hillside behind
        // the wall was cleared the next time the chunk was built. It survives in
        // memory either way, because an edited chunk is pinned, so the only way to
        // see it is to drop the chunk and build it again.
        const float groundX = static_cast<float>(wx + 8) * config::kBuildCell;

        world.Update({groundX - 500.0f, terrain::Height(groundX, settings) - 300.0f, 1000.0f, 900.0f});

        int gx = 0;
        int gy = 0;
        World::ToCell({groundX, terrain::Height(groundX, settings) + 3.0f * config::kBuildCell}, gx, gy);

        World::Yield before{};
        world.CellHolds(gx, gy, before);

        world.PlaceCell(Element::WoodWall, gx, gy);

        World::Yield after{};
        world.CellHolds(gx, gy, after);

        // Walked out of residency and back, which is what replays the journal.
        world.Update({groundX + 12000.0f, 0.0f, 1000.0f, 600.0f});
        world.Update({groundX - 500.0f, terrain::Height(groundX, settings) - 300.0f, 1000.0f, 900.0f});

        World::Yield rebuilt{};
        world.CellHolds(gx, gy, rebuilt);

        // A cell of hillside to begin with, or the check is testing open sky and
        // would pass whatever happened.
        int held = 0;
        for (std::size_t k = 0; k < kElementCount; k++) held += before[k];

        const bool standing = held > 0 && after == before && rebuilt == before;

        const bool layered = together && kept && blockFirst && wallAfter && standing;

        if (!together) std::printf("WRONG: a plank set in front of a wall did not leave both standing\n");
        if (!kept) std::printf("WRONG: the pair did not survive the chunk being dropped and rebuilt\n");
        if (!blockFirst) std::printf("WRONG: digging took the wall before the block in front of it\n");
        if (!wallAfter) std::printf("WRONG: digging the open cell did not take the wall\n");

        // What a cell holds, as materials rather than as a total: the fault this
        // catches leaves the *count* alone and changes what is being counted — the
        // ground goes, and the wall behind it is what the same walk finds instead.
        const auto say = [](const char *when, const World::Yield &yield) {
            std::printf("  %s:", when);

            for (std::size_t k = 0; k < kElementCount; k++) {
                if (yield[k] > 0) std::printf(" %s=%d", StyleOf(static_cast<Element>(k)).name, yield[k]);
            }

            std::putchar(10);
        };

        if (held <= 0) {
            std::printf("WRONG: the hillside check found no ground at cell %d,%d to stand a wall behind\n", gx, gy);
        } else if (after != before) {
            std::printf("WRONG: putting a wall up changed the ground in front of it\n");
            say("was", before);
            say("now", after);
        } else if (rebuilt != before) {
            std::printf("WRONG: the ground behind a wall did not survive the chunk being rebuilt\n");
            say("was", before);
            say("now", rebuilt);
        }

        if (layered) {
            std::printf("layered: a wall keeps its cell under a block, is dug out after it, and takes nothing from "
                        "the hillside it is put up against\n");
        }

        if (whole && even && square && steady && layered) {
            std::printf("\nexact: %d cells cost %.0f blocks and gave back %.0f\n", laid, cost, back);
            std::printf("aligned: every point in a cell names it, and its vertices sit inside it\n");
            std::printf("drawn:   every cell rasterises to the same squares, on its own edges\n");
        }

        // And a picture of it, where one was asked for.
        //
        // Everything above is arithmetic, and none of it can see whether the face
        // a material draws is the face somebody authored — a picture tiled a third
        // of a block out passes every check here and is obvious the moment it is
        // looked at. This builds a patch of wall out of each cell-laid material and
        // draws it through the real path, cache and all.
        if (argc >= 4 && TextIsEqual(argv[3], "--png")) {
            const int side = 9;

            // Well clear of the run above and of any terrain.
            const int px = 60;
            const int py = -50;

            const Element kinds[] = {Element::WoodPlank, Element::Cobblestone, Element::WoodWall};

            const Rectangle around = {static_cast<float>(px) * config::kBuildCell - 400.0f,
                                      static_cast<float>(py) * config::kBuildCell - 400.0f,
                                      static_cast<float>(3 * (side + 2) + 1) * config::kBuildCell + 800.0f,
                                      static_cast<float>(side + 2) * config::kBuildCell + 800.0f};

            // Resident before anything is laid into them. A cell written into a
            // chunk that is not here yet is remembered and replayed later, which is
            // correct and is not what this picture is testing — and getting it
            // wrong leaves a seam exactly on the chunk border, which looks like a
            // fault in the drawing.
            world.Update(around);

            for (int k = 0; k < 3; k++) {
                const int ox = px + k * (side + 2);

                // Snow packed round each patch, because that is the case the
                // fringe shows in and it is not a hypothetical: snow is drawn at
                // the terrain's coarse texel and is outranked by everything built,
                // so its silhouette used to reach out over a block and paint the
                // ground around it in its own big squares. The block then covered
                // only its own exact area and the overhang stayed — a pale rind
                // down the side of every piece, in any chunk cold enough to hold
                // snow.
                for (int i = -2; i < side + 2; i++) {
                    for (int j = -2; j < side + 2; j++) world.PlaceCell(Element::Snow, ox + i, py + j);
                }

                for (int i = 0; i < side; i++) {
                    for (int j = 0; j < side; j++) world.PlaceCell(kinds[k], ox + i, py + j);
                }
            }

            const Rectangle strip = {static_cast<float>(px) * config::kBuildCell - 20.0f,
                                     static_cast<float>(py) * config::kBuildCell - 20.0f,
                                     static_cast<float>(3 * (side + 2) + 1) * config::kBuildCell + 40.0f,
                                     static_cast<float>(side + 2) * config::kBuildCell + 40.0f};

            Inventory unused{};

            // Unlit, which is the whole point of it: what a material's own face
            // looks like and what the light is doing to it are two questions, and
            // a solid wall of blocks puts its own middle in the dark — so a lit
            // picture of one shows the light and not the drawing.
            // Weather forced clear, or what falls out of the sky lands in the
            // picture: at this height a shower draws streaks straight down the
            // wall, and they read as seams in the drawing rather than as rain.
            DrawProbe(world, grove, unused, strip, argv[4], 4, 0.0f, false, 0, 0, 0);

            std::printf("wrote %s\n", argv[4]);
        }

        return (whole && even && square && steady && layered) ? 0 : 1;
    }

    if (timing) {
        Inventory timed{};

        ReportFrame(world, grove, timed,
                    {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3]))},
                    (argc >= 5) ? std::atoi(argv[4]) : 20);

        return 0;
    }

    if (settling) {
        ReportSettling(world,
                       {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                        static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))},
                       (argc >= 7) ? std::atoi(argv[6]) : 600);

        return 0;
    }

    if (assaying) {
        ReportOre(world, world.Settings(),
                  {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                   static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

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

        const std::array<std::pair<const char *, float *>, 18> knobs = {{
            {"region", &c.regionCoverage},
            {"shallow", &c.regionCoverageShallow},
            {"deepens", &c.regionDeepens},
            {"crust", &c.crust},
            {"chance", &c.systems.chance},
            {"span", &c.systems.cellSpan},
            {"rise", &c.systems.cellRise},
            {"step", &c.systems.stepLength},
            {"wander", &c.systems.wander},
            {"squash", &c.systems.squash},
            {"radius", &c.systems.radius},
            {"deep", &c.systems.radiusAtDepth},
            {"room", &c.systems.roomChance},
            {"swell", &c.systems.roomSwell},
            {"floor", &c.systems.roomFloor},
            {"mouth", &c.systems.entranceChance},
            {"fret", &c.roughness.amplitude},
            {"lobe", &c.roughness.lobeAmplitude},
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

                return 1;
            }

            *knob->second = static_cast<float>(std::atof(split + 1));
        }

        // Recalibrated after the overrides, since three of them are coverages and
        // a coverage is only a share once its cutoff has been measured.
        terrain::Calibrate(tuned);

        ReportCaves(tuned, {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                            static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

        return 0;
    }

    if (probing) {
        const Rectangle strip = {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                                 static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))};

        Inventory probed{};

        DrawProbe(world, grove, probed, strip, argv[6], (argc >= 8) ? std::atoi(argv[7]) : 1,
                  (argc >= 9) ? static_cast<float>(std::atof(argv[8])) : 0.0f,
                  (argc >= 10) ? (std::atoi(argv[9]) != 0) : true, (argc >= 11) ? std::atoi(argv[10]) : 0,
                  (argc >= 12) ? std::atoi(argv[11]) : -1, (argc >= 13) ? std::atoi(argv[12]) : -1);

        return 0;
    }

    if (reading) {
        const float x  = static_cast<float>(std::atof(argv[2]));
        const float y0 = static_cast<float>(std::atof(argv[3]));
        const float y1 = static_cast<float>(std::atof(argv[4]));

        world.Update({x - 128.0f, y0 - 128.0f, 256.0f, y1 - y0 + 256.0f});
        ReportColumn(world, settings, x, y0, y1);

        return 0;
    }


    return std::nullopt;
}
