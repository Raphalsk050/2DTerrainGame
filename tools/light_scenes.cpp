// A bank of scenes for testing the light, and the numbers to go with them.
//
// Run it with tools/lightscenes.bat (or .sh). It does three things, in this order,
// because that is the order in which a fault is worth finding:
//
//   1. Closure. Three scenes whose answer is known in closed form -- a uniform sky
//      has to come back at exactly its own brightness, sealed rock has to be black,
//      twice the emission has to be twice the light. These catch a lost quadrant, a
//      cone arc that does not sum to the turn, or a normalisation off by 2 pi, and
//      they catch them as a number rather than as a picture that looks a bit dark.
//      One of them found the 2 pi.
//
//   2. Pictures. Twelve scenes, each isolating a single thing the solver has to get
//      right, each simple enough that the answer can be judged by eye. Two per
//      scene: the light on its own, which is where an artefact shows, and the light
//      over the material, which is what a player would see. Plus a captioned contact
//      sheet, because twelve unlabelled grey rectangles do not get looked at.
//
//   3. Roughness. The measurement that decides whether the cross blur is still
//      earning its place, and whether the checkerboard the paper describes is
//      actually present. See kBlur in radiance_shaders.h for what it found.
//
// Unlike tools/sheet.cpp this needs a real graphics device: the solver is compute
// shaders and wants an OpenGL 4.3 context, so a window is opened and immediately
// ignored.

#include "radiance.h"

#include "raylib.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kSide = 512;   // half is 256, a power of two

constexpr float kOpaque = 32.0f;   // e^-32 is nothing: a wall

using radiance::Medium;
using radiance::Radiance;

struct Material {
    float sigma = 0.0f;
    Radiance albedo{};
    Radiance emission{};
};

const Material kAir   = {0.0f, {}, {}};
const Material kStone = {kOpaque, {0.55f, 0.53f, 0.50f}, {}};
const Material kWhite = {kOpaque, {0.85f, 0.85f, 0.85f}, {}};
const Material kRed   = {kOpaque, {0.75f, 0.12f, 0.10f}, {}};
const Material kGreen = {kOpaque, {0.12f, 0.65f, 0.18f}, {}};

Material Lamp(float strength, Radiance colour = {1.0f, 0.95f, 0.85f}) {
    return {0.0f, {}, {colour.r * strength, colour.g * strength, colour.b * strength}};
}

Material Fog(float sigma, Radiance albedo = {0.9f, 0.92f, 1.0f}) {
    return {sigma, albedo, {}};
}

void Put(Medium &medium, int i, int j, const Material &material) {
    if (!medium.InBounds(i, j)) return;

    const int cell = medium.Index(i, j);

    medium.sigma[cell]    = material.sigma;
    medium.albedo[cell]   = material.albedo;
    medium.emission[cell] = material.emission;
}

void Box(Medium &medium, int x0, int y0, int x1, int y1, const Material &material) {
    for (int j = y0; j <= y1; j++) {
        for (int i = x0; i <= x1; i++) Put(medium, i, j, material);
    }
}

void Disc(Medium &medium, float cx, float cy, float radius, const Material &material) {
    const int x0 = static_cast<int>(std::floor(cx - radius));
    const int x1 = static_cast<int>(std::ceil(cx + radius));
    const int y0 = static_cast<int>(std::floor(cy - radius));
    const int y1 = static_cast<int>(std::ceil(cy + radius));

    for (int j = y0; j <= y1; j++) {
        for (int i = x0; i <= x1; i++) {
            const float dx = i + 0.5f - cx;
            const float dy = j + 0.5f - cy;

            if (dx * dx + dy * dy <= radius * radius) Put(medium, i, j, material);
        }
    }
}

void Fill(Medium &medium, float sigma, Radiance albedo, Radiance emission) {
    Box(medium, 0, 0, medium.cols - 1, medium.rows - 1, Material{sigma, albedo, emission});
}

// A hollow room with walls of the given material.
void Room(Medium &medium, int x0, int y0, int x1, int y1, int thickness, const Material &wall) {
    Box(medium, x0, y0, x1, y0 + thickness - 1, wall);
    Box(medium, x0, y1 - thickness + 1, x1, y1, wall);
    Box(medium, x0, y0, x0 + thickness - 1, y1, wall);
    Box(medium, x1 - thickness + 1, y0, x1, y1, wall);
}

struct Scene {
    const char *name;
    const char *asks;
    int solves;        // how many, for the bounce to settle
    float exposure;    // per scene, because an enclosed room and open country are
                       // nowhere near the same brightness and one curve for both
                       // hides whichever it is not set for
    void (*build)(Medium &);
};

// ---------------------------------------------------------------------------
// The scenes.
// ---------------------------------------------------------------------------

// A small light and one occluder. The umbra has to be sharp and the penumbra has to
// widen with distance from the occluder. This is the whole reason for holographic
// cascades over ordinary ones.
void BuildHardShadow(Medium &medium) {
    Disc(medium, 70.0f, 256.0f, 6.0f, Lamp(14.0f));
    Box(medium, 180, 200, 190, 312, kStone);
}

// Three bars at increasing distance from the same light. The penumbra under each
// should be wider than the one before it, and by a predictable amount.
void BuildPenumbraRamp(Medium &medium) {
    Disc(medium, 40.0f, 256.0f, 10.0f, Lamp(18.0f));
    Box(medium, 120, 120, 126, 180, kStone);
    Box(medium, 230, 220, 236, 290, kStone);
    Box(medium, 350, 330, 356, 420, kStone);
}

// Light through a narrow gap. Nothing may come through the wall itself, and the fan
// beyond the gap must be clean.
void BuildPinhole(Medium &medium) {
    Box(medium, 0, 0, 150, kSide - 1, kStone);
    Box(medium, 148, 240, 152, 272, kAir);
    Disc(medium, 70.0f, 256.0f, 26.0f, Lamp(20.0f));
}

// A wall one cell thick, running the whole height with a single gap in it. Left of
// it must be bright, right of it must be dark except for the fan through the gap,
// and the wall itself must not glow through. The old solver read a coarsened copy of
// the world and had a dial trading this against shadows that came out too dark;
// there is no such dial here, so this is either right or it is broken.
void BuildThinWall(Medium &medium) {
    Disc(medium, 100.0f, 256.0f, 14.0f, Lamp(20.0f));

    Box(medium, 250, 0, 250, kSide - 1, kStone);
    Box(medium, 250, 246, 250, 266, kAir);
}

// A closed box, one white light in the ceiling, a red wall and a green wall. The
// floor has to pick up red on one side and green on the other, and the shadow under
// the block must not be black. This is the test for multiple bounces, and it is the
// thing the old solver could not do at all.
void BuildColourBleed(Medium &medium) {
    Room(medium, 40, 40, 472, 472, 14, kWhite);

    Box(medium, 40, 40, 53, 472, kRed);
    Box(medium, 459, 40, 472, 472, kGreen);

    Box(medium, 200, 56, 312, 66, Lamp(1.6f));
    Disc(medium, 256.0f, 330.0f, 34.0f, kStone);
}

// The same box with the bounce turned off, for comparison. Everything the light does
// not see directly must be black.
void BuildColourBleedDirect(Medium &medium) {
    BuildColourBleed(medium);
}

// A bank of fog with a light behind a slotted wall. Shafts should form, and they
// should soften with distance rather than ending on a line.
void BuildVolumetric(Medium &medium) {
    // Thin, and only moderately scattering. A medium with an albedo near one is a
    // diffuser: multiple scattering amplifies by 1/(1 - albedo), which at 0.9 is ten
    // times over, and what that does to a shaft of light is wash it out completely.
    // That is right, and it is why real god rays come from thin haze rather than
    // from fog.
    Box(medium, 0, 0, kSide - 1, kSide - 1, Fog(0.006f, {0.40f, 0.42f, 0.50f}));

    Disc(medium, 60.0f, 256.0f, 18.0f, Lamp(9.0f));

    for (int n = 0; n < 5; n++) {
        const int y = 60 + n * 90;
        Box(medium, 150, y, 158, y + 52, kStone);
    }
}

// Ground with an overhang and a cave under it, open sky above. Daylight must land on
// what is exposed, fall off through the mouth, and not reach the back of the cave --
// with no skyline, no per-column cover and no sunlight depth anywhere in the solver.
void BuildSkyAndCave(Medium &medium) {
    for (int i = 0; i < kSide; i++) {
        const float hill = 190.0f + 40.0f * std::sin(i * 0.017f) + 18.0f * std::sin(i * 0.055f);

        Box(medium, i, static_cast<int>(hill), i, kSide - 1, kStone);
    }

    // A cave, and a shaft up to daylight at the far end of it.
    Box(medium, 120, 300, 400, 360, kAir);
    Box(medium, 372, 214, 396, 360, kAir);

    // A ledge with a hollow under it, which is the case that used to need a special
    // rule: the nearest open space below is the cave, not the sky.
    Box(medium, 60, 250, 150, 268, kAir);
}

// Three coloured lamps in a white room. The channels have to stay apart where they
// are apart and add where they overlap, and the walls have to take their colour.
void BuildColouredLights(Medium &medium) {
    Room(medium, 30, 30, 482, 482, 16, kWhite);

    Disc(medium, 150.0f, 200.0f, 12.0f, Lamp(12.0f, {1.0f, 0.15f, 0.12f}));
    Disc(medium, 362.0f, 200.0f, 12.0f, Lamp(12.0f, {0.12f, 1.0f, 0.25f}));
    Disc(medium, 256.0f, 360.0f, 12.0f, Lamp(12.0f, {0.20f, 0.35f, 1.0f}));

    Box(medium, 246, 230, 266, 330, kStone);
}

// A light two cells across. The paper names this as its one real limitation:
// sources smaller than about eight times the probe spacing alias, because a small
// change of position moves the light across a whole ray. Here to be looked at
// honestly rather than hidden.
void BuildTinyLight(Medium &medium) {
    Disc(medium, 256.0f, 256.0f, 1.0f, Lamp(400.0f));

    for (int n = 0; n < 8; n++) {
        const float angle = n * 0.785f;

        Box(medium, static_cast<int>(256 + 90 * std::cos(angle)) - 4,
            static_cast<int>(256 + 90 * std::sin(angle)) - 4,
            static_cast<int>(256 + 90 * std::cos(angle)) + 4,
            static_cast<int>(256 + 90 * std::sin(angle)) + 4, kStone);
    }
}

// One lamp in a stone room. Every face turned towards it must be lit and every face
// turned away must not be, and a little way into the rock must be black. The old
// solver could not do this at all -- it had a whole extra pass that pushed light
// sideways into solids, plus a reach and a lip to tune, because a volume with no
// albedo has nothing for a surface to show.
void BuildSurfaces(Medium &medium) {
    Room(medium, 40, 40, 472, 472, 40, kStone);

    Disc(medium, 150.0f, 150.0f, 10.0f, Lamp(15.0f));

    Box(medium, 240, 120, 300, 400, kStone);
    Box(medium, 300, 340, 420, 400, kStone);
}

// A single bright lamp at one corner of the region. The old solver gathered light
// over a bounded reach and cut off hard at the end of it -- 2046 world pixels, with
// a visible edge. Here the reach is the region, so the far corner must be dim but
// never cut.
void BuildLongRange(Medium &medium) {
    Disc(medium, 30.0f, 30.0f, 12.0f, Lamp(60.0f));

    for (int n = 1; n <= 6; n++) {
        Box(medium, n * 70, n * 70, n * 70 + 10, n * 70 + 40, kStone);
    }
}

const Scene kScenes[] = {
    {"01-hard-shadow", "sharp umbra, penumbra widening with distance", 2, 0.9f, BuildHardShadow},
    {"02-penumbra-ramp", "each bar's shadow softer than the last", 2, 0.9f, BuildPenumbraRamp},
    {"03-pinhole", "a clean fan through the gap, nothing through the wall", 2, 0.7f, BuildPinhole},
    {"04-thin-wall", "one cell of stone stops it dead", 2, 0.7f, BuildThinWall},
    {"05-colour-bleed", "red and green in the shadow, and it is not black", 16, 1.1f,
     BuildColourBleed},
    {"06-colour-bleed-direct", "the same with no bounce: the shadow goes black", 2, 1.1f,
     BuildColourBleedDirect},
    {"07-volumetric", "shafts through fog, softening with distance", 8, 1.0f, BuildVolumetric},
    {"08-sky-and-cave", "daylight on what is exposed, dark at the back of the cave", 10, 1.2f,
     BuildSkyAndCave},
    {"09-coloured-lights", "three channels apart, and adding where they meet", 12, 1.0f,
     BuildColouredLights},
    {"10-tiny-light", "the known limit: a source under 8x the probe spacing aliases", 2, 3.0f,
     BuildTinyLight},
    {"11-surfaces", "faces towards the lamp lit, faces away and deep rock black", 14, 1.0f,
     BuildSurfaces},
    {"12-long-range", "dim in the far corner, but never cut off", 3, 0.8f, BuildLongRange},
};

// The material, drawn as a picture, so the light can be seen over the thing it is
// lighting rather than on its own.
Image MaterialPicture(const Medium &medium) {
    Image picture = GenImageColor(medium.cols, medium.rows, BLACK);
    Color *pixels = static_cast<Color *>(picture.data);

    for (int j = 0; j < medium.rows; j++) {
        for (int i = 0; i < medium.cols; i++) {
            const int cell = medium.Index(i, j);

            const Radiance albedo = medium.albedo[cell];
            const bool solid      = medium.sigma[cell] > 1.0f;
            const bool glowing    = medium.emission[cell].r + medium.emission[cell].g +
                                 medium.emission[cell].b > 0.0f;

            // Open air is given a tone of its own rather than left black. Nothing in
            // a game is drawn there, but this is a test map: the whole point is to
            // see the field, and multiplying it into black hides exactly what is
            // being judged.
            Color colour = {74, 78, 88, 255};

            if (solid) {
                colour = {static_cast<unsigned char>(albedo.r * 255.0f),
                          static_cast<unsigned char>(albedo.g * 255.0f),
                          static_cast<unsigned char>(albedo.b * 255.0f), 255};
            } else if (medium.sigma[cell] > 0.0f) {
                const float thin = 40.0f;
                colour           = {static_cast<unsigned char>(albedo.r * thin),
                                    static_cast<unsigned char>(albedo.g * thin),
                                    static_cast<unsigned char>(albedo.b * thin), 255};
            }

            if (glowing) colour = {255, 246, 214, 255};

            pixels[j * medium.cols + i] = colour;
        }
    }

    return picture;
}

// How rough the field is along each axis, over a window that ought to be smooth.
//
// The second difference, because it kills a linear gradient and leaves only the
// wiggle -- a falloff away from a lamp reads as zero, and a row that disagrees with
// its neighbours does not. Measured along rows and along columns separately, since
// the whole question is whether the method's one asymmetry is showing: rows are
// never subdivided by the cascades, so an error that lives per row has nothing to
// average it out and comes through as horizontal streaking.
//
// A field with no directional artefact has the two within a whisker of each other.
struct Roughness {
    double alongY = 0.0;   // across rows: where striping would live
    double alongX = 0.0;   // across columns
    double level  = 0.0;   // mean brightness in the window, to read the others against
};

Roughness Measure(const radiance::Field &field, const Medium &medium, int x0, int y0, int x1,
                  int y1) {
    Roughness out;
    int counted = 0;

    const auto luminance = [&](int i, int j) {
        const Vector2 at = {medium.origin.x + (i + 0.5f) * medium.spacing,
                            medium.origin.y + (j + 0.5f) * medium.spacing};

        return static_cast<double>(radiance::Luminance(field.At(at)));
    };

    for (int j = y0 + 1; j < y1 - 1; j++) {
        for (int i = x0 + 1; i < x1 - 1; i++) {
            const double here = luminance(i, j);

            out.alongY += std::fabs(luminance(i, j - 1) - 2.0 * here + luminance(i, j + 1));
            out.alongX += std::fabs(luminance(i - 1, j) - 2.0 * here + luminance(i + 1, j));
            out.level += here;

            counted++;
        }
    }

    if (counted > 0) {
        out.alongY /= counted;
        out.alongX /= counted;
        out.level /= counted;
    }

    return out;
}

Image Compose(const Image &material, const Image &light) {
    Image out = ImageCopy(material);

    Color *dst       = static_cast<Color *>(out.data);
    const Color *lit = static_cast<const Color *>(light.data);

    for (int n = 0; n < out.width * out.height; n++) {
        dst[n].r = static_cast<unsigned char>(dst[n].r * lit[n].r / 255);
        dst[n].g = static_cast<unsigned char>(dst[n].g * lit[n].g / 255);
        dst[n].b = static_cast<unsigned char>(dst[n].b * lit[n].b / 255);
    }

    return out;
}

// The three scenes whose answer is known exactly.
//
// Worth running before anything is looked at, because a picture cannot tell you the
// difference between "a bit dark" and "a factor of 2 pi dark", and one of these can.
bool Closure(radiance::Field &field) {
    Medium medium;

    medium.Resize(kSide, kSide);
    medium.spacing = 1.0f;
    medium.origin  = {0.0f, 0.0f};

    radiance::Settings settings;
    settings.exposure = 1.0f;
    settings.bounce   = false;

    bool passed = true;

    const auto middle = [&]() {
        const Vector2 at = {medium.cols * medium.spacing * 0.5f,
                            medium.rows * medium.spacing * 0.5f};

        return static_cast<double>(radiance::Luminance(field.At(at)));
    };

    // Twice each time, because At() reads the readback of the previous solve.
    const auto settle = [&](int times) {
        for (int n = 0; n < times; n++) field.Solve(medium, settings);
    };

    const auto report = [&](const char *name, double got, double want, double tolerance) {
        const bool ok = std::fabs(got - want) <= tolerance;

        std::printf("%-34s got %9.5f  want %9.5f  %s\n", name, got, want, ok ? "ok" : "MISMATCH");

        passed = passed && ok;
    };

    // Every ray leaves the region and every one sees the same sky, so the answer is
    // the sky's own radiance, exactly. Nothing else in this file tests as much at
    // once: a quadrant that is not being added, an arc that does not close, or a
    // normalisation applied twice all show up here and only here.
    settings.sky.radiance = {1.0f, 1.0f, 1.0f};
    settings.sky.horizon  = -2.0f;   // sky in every direction, downward included
    settings.sky.zenith   = -1.9f;

    Fill(medium, 0.0f, {}, {});
    settle(2);

    if (!field.Ready()) {
        std::printf("the shaders did not build; see the log above\n");
        return false;
    }

    report("empty world, sky all round", middle(), 1.0, 0.02);

    // Nothing gets in and nothing is emitted. Catches a sky credited along a ray
    // rather than at the end of one, which would light the inside of solid rock.
    Fill(medium, 32.0f, {}, {});
    settle(2);

    report("sealed in rock", middle(), 0.0, 0.01);

    // Twice the emission, twice the light. Catches a non-linearity anywhere in the
    // chain, which a single brightness cannot.
    settings.sky.radiance = {0.0f, 0.0f, 0.0f};

    Fill(medium, 0.0f, {}, {0.01f, 0.01f, 0.01f});
    settle(2);
    const double once = middle();

    Fill(medium, 0.0f, {}, {0.02f, 0.02f, 0.02f});
    settle(2);
    const double twice = middle();

    report("emission doubles", (once > 1e-6) ? (twice / once) : 0.0, 2.0, 0.05);

    return passed;
}

} // namespace

int main(int argc, char **argv) {
    std::string out   = "build/scenes";
    std::string only;
    bool pictures     = true;
    bool measurements = true;

    for (int n = 1; n < argc; n++) {
        const std::string arg = argv[n];

        if (arg == "--out" && n + 1 < argc) {
            out = argv[++n];
        } else if (arg == "--only" && n + 1 < argc) {
            only = argv[++n];
        } else if (arg == "--checks") {
            pictures     = false;
            measurements = false;
        } else if (arg == "--no-measure") {
            measurements = false;
        } else {
            std::printf(
                "usage: light_scenes [--out DIR] [--only NAME] [--checks] [--no-measure]\n"
                "  --out         where the pictures go (default build/scenes)\n"
                "  --only NAME   render one scene, by the name in its filename\n"
                "  --checks      the closed-form checks alone, no pictures\n"
                "  --no-measure  skip the roughness measurement\n");
            return arg == "--help" ? 0 : 1;
        }
    }

    SetTraceLogLevel(LOG_WARNING);
    InitWindow(320, 200, "light scenes");

    {
        radiance::Field checks;

        std::printf("closed form\n");

        const bool passed = Closure(checks);

        checks.Unload();

        if (!passed) {
            std::printf("\nclosure failed: the pictures below are not worth reading yet.\n");
        }

        if (!pictures) {
            CloseWindow();
            return passed ? 0 : 1;
        }

        std::printf("\n");
    }

    radiance::Field field;

    std::vector<Image> sheet;

    for (const Scene &scene : kScenes) {
        if (!only.empty() && std::strstr(scene.name, only.c_str()) == nullptr) continue;

        Medium medium;

        medium.Resize(kSide, kSide);
        medium.spacing = 1.0f;
        medium.origin  = {0.0f, 0.0f};

        radiance::Settings settings;
        settings.exposure = scene.exposure;
        settings.bounce   = true;

        // A dim overcast sky by default, so nothing is lit purely by its own lamp
        // and the boundary is always being exercised.
        settings.sky.radiance = {0.05f, 0.06f, 0.08f};
        settings.sky.horizon  = -0.35f;
        settings.sky.zenith   = 0.25f;

        if (std::string(scene.name) == "08-sky-and-cave") {
            settings.sky.radiance = {1.05f, 1.02f, 0.92f};
            settings.sky.horizon  = -0.15f;
            settings.sky.zenith   = 0.30f;
        }

        if (std::string(scene.name) == "06-colour-bleed-direct") settings.bounce = false;

        scene.build(medium);

        for (int n = 0; n < scene.solves; n++) field.Solve(medium, settings);

        if (!field.Ready()) {
            std::printf("shaders did not build\n");
            CloseWindow();
            return 1;
        }

        Image light    = LoadImageFromTexture(field.Screen());
        Image material = MaterialPicture(medium);
        Image view     = Compose(material, light);

        ExportImage(light, (out + "/" + scene.name + "-light.png").c_str());
        ExportImage(view, (out + "/" + scene.name + "-view.png").c_str());

        std::printf("%-24s %s\n", scene.name, scene.asks);

        sheet.push_back(ImageCopy(view));

        UnloadImage(light);
        UnloadImage(material);
        UnloadImage(view);
    }

    // A contact sheet, four across, each panel captioned with what it is asking.
    // Only when every scene ran: a sheet of one panel is the panel, and the captions
    // are indexed against the full table.
    if (only.empty())
    // Without the caption the sheet is twelve grey rectangles and reading it means
    // counting along the rows, which is exactly the sort of friction that stops a
    // test from being looked at.
    {
        const int across  = 4;
        const int down    = (static_cast<int>(sheet.size()) + across - 1) / across;
        const int tile    = 256;
        const int caption = 34;
        const int pad     = 8;

        const int cellW = tile + pad;
        const int cellH = tile + caption + pad;

        Image board = GenImageColor(across * cellW + pad, down * cellH + pad,
                                    Color{18, 19, 23, 255});

        for (std::size_t n = 0; n < sheet.size(); n++) {
            Image tileImage = ImageCopy(sheet[n]);
            ImageResize(&tileImage, tile, tile);

            const int col = static_cast<int>(n) % across;
            const int row = static_cast<int>(n) / across;

            const int x = pad + col * cellW;
            const int y = pad + row * cellH;

            ImageDraw(&board, tileImage,
                      Rectangle{0, 0, static_cast<float>(tile), static_cast<float>(tile)},
                      Rectangle{static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(tile), static_cast<float>(tile)},
                      WHITE);

            ImageDrawText(&board, kScenes[n].name, x, y + tile + 3, 10,
                          Color{226, 232, 240, 255});
            ImageDrawText(&board, kScenes[n].asks, x, y + tile + 17, 10,
                          Color{132, 140, 152, 255});

            UnloadImage(tileImage);
        }

        ExportImage(board, (out + "/00-contact-sheet.png").c_str());
        UnloadImage(board);
    }

    for (Image &spare : sheet) UnloadImage(spare);
    sheet.clear();

    // ---- is the cross blur still earning its place? -------------------------
    //
    // The paper adds it because the method leaves a checkerboard: v_n(k) has an even
    // y component at every level above the first, so probes of odd and even row never
    // interact. The doubled direction count taken from the reference implementation
    // is supposed to remove that at the source. This is the measurement that says
    // whether it did, and whether the blur is now taking out an artefact or just
    // softening a field that is already clean.
    if (measurements) {
        Medium medium;

        medium.Resize(kSide, kSide);
        medium.spacing = 1.0f;
        medium.origin  = {0.0f, 0.0f};

        BuildHardShadow(medium);

        radiance::Settings settings;
        settings.exposure     = 0.9f;
        settings.bounce       = true;
        settings.sky.radiance = {0.05f, 0.06f, 0.08f};
        settings.sky.horizon  = -0.35f;
        settings.sky.zenith   = 0.25f;

        std::printf("\nrow-to-row roughness, lit region of 01-hard-shadow\n");
        std::printf("%-14s %12s %12s %10s %10s\n", "cross blur", "across rows", "across cols",
                    "ratio", "level");

        for (int pass = 0; pass < 2; pass++) {
            settings.crossBlur = (pass == 1);

            for (int n = 0; n < 3; n++) field.Solve(medium, settings);

            const Roughness rough = Measure(field, medium, 95, 60, 175, 185);

            std::printf("%-14s %12.6f %12.6f %10.3f %10.4f\n", settings.crossBlur ? "on" : "off",
                        rough.alongY, rough.alongX,
                        rough.alongX > 0.0 ? rough.alongY / rough.alongX : 0.0, rough.level);
        }

        // The same in the penumbra, which is where a row-wise error is most visible
        // because the field is changing fastest there.
        std::printf("\nthe same, inside the penumbra\n");

        for (int pass = 0; pass < 2; pass++) {
            settings.crossBlur = (pass == 1);

            for (int n = 0; n < 3; n++) field.Solve(medium, settings);

            const Roughness rough = Measure(field, medium, 300, 150, 460, 210);

            std::printf("%-14s %12.6f %12.6f %10.3f %10.4f\n", settings.crossBlur ? "on" : "off",
                        rough.alongY, rough.alongX,
                        rough.alongX > 0.0 ? rough.alongY / rough.alongX : 0.0, rough.level);
        }
    }

    std::printf("\nrays per solve: %ld\n", field.Rays());

    field.Unload();
    CloseWindow();

    return 0;
}
