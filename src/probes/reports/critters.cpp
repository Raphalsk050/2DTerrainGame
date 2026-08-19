#include "core/config.h"
#include "core/figure.h"
#include "core/registry.h"
#include "entity/mob/mob_def.h"
#include "probes/report.h"

#include <cstdio>
#include <cstdlib>

// `--critters out.png [zoom]` — a contact sheet of every creature.
//
// The counterpart of `tools/sheet.cpp` for plants, and it exists for the reason
// that one gives: the art here is authored at six texels a side and judged in a
// world where a creature is thirty pixels tall behind a hillside at whatever the
// light is doing. The question "is this recognisable, is it facing the right way,
// does it read against the ground" is a question about the *image*, and looking at
// it in the game means finding one first.
//
// It draws each creature at the world's own texel — `config::kPixelSize` — so what
// is on the sheet is the size it is in the world, not a blown-up version of it. The
// zoom multiplies the whole sheet afterwards, which is a different thing: the
// picture stays exactly the picture, and there is no resampling of a creature into a
// grid it was not drawn on. Same argument CLAUDE.md §5.5 makes about the terrain
// texture.
//
// Both facings are drawn, side by side. A mirrored sprite is the one fault that is
// invisible in a single still: a boar whose snout is on the wrong end looks fine
// until it turns round.
namespace {

// Room around each creature, in world pixels, and how much of the row a name gets.
constexpr int kPad  = 14;
constexpr int kName = 92;

// The plate each one stands on. Two tones so that a dark creature and a pale one are
// both legible on the same sheet — a single grey is always wrong for one of them.
constexpr Color kFloorA = {56, 60, 70, 255};
constexpr Color kFloorB = {104, 110, 124, 255};

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    const int zoom = (bench.argc >= 4) ? std::max(1, std::atoi(bench.argv[3])) : 4;

    const int rows = mob::kinds::Count();

    if (rows <= 0) {
        std::printf("no creatures registered\n");

        return 1;
    }

    const float texel = static_cast<float>(config::kPixelSize);

    // Wide enough for the widest creature drawn twice, plus its name.
    int widest = 0;
    int tallest = 0;

    for (int r = 0; r < rows; r++) {
        const figure::Figure &look = mob::kinds::Of(mob::Kind{r}).look;

        widest  = std::max(widest, look.width);
        tallest = std::max(tallest, look.height);
    }

    const int cellW = kName + (widest * config::kPixelSize + kPad) * 2 + kPad;
    const int cellH = tallest * config::kPixelSize + kPad * 2;

    RenderTexture2D sheet = LoadRenderTexture(cellW, cellH * rows);

    BeginTextureMode(sheet);
    ClearBackground({24, 26, 32, 255});

    for (int r = 0; r < rows; r++) {
        const mob::Def &def = mob::kinds::Of(mob::Kind{r});

        const float top = static_cast<float>(r * cellH);

        DrawRectangleRec({0.0f, top, static_cast<float>(cellW), static_cast<float>(cellH)},
                         ((r % 2) == 0) ? kFloorA : kFloorB);

        DrawText(def.name, 8, static_cast<int>(top) + cellH / 2 - 5, 10, {236, 240, 248, 255});

        // Bottom-centre is where `figure::Draw` anchors, which is where a body's own
        // position is — so a creature that looks like it is standing on the line here
        // is standing on the ground in the world.
        const float feet = top + static_cast<float>(cellH - kPad);

        const float rightAt = static_cast<float>(kName + kPad) + def.look.width * texel * 0.5f;
        const float leftAt  = rightAt + def.look.width * texel + static_cast<float>(kPad);

        // The line it stands on, so a figure drawn a texel high or low is obvious
        // rather than merely slightly odd.
        DrawLineV({static_cast<float>(kName), feet}, {static_cast<float>(cellW - kPad), feet},
                  {255, 255, 255, 60});

        figure::Draw(def.look, {rightAt, feet}, texel, 1, WHITE);
        figure::Draw(def.look, {leftAt, feet}, texel, -1, WHITE);

        // And the collider it actually has, which is the thing the drawing is
        // supposed to agree with. A figure wider than its box is a creature stopped
        // by walls it visibly is not touching; narrower and it stands inside them.
        const Rectangle box = {rightAt - def.build.width / 2.0f, feet - def.build.height, def.build.width,
                               def.build.height};

        DrawRectangleLinesEx(box, 1.0f, {255, 210, 90, 140});
    }

    EndTextureMode();

    Image out = LoadImageFromTexture(sheet.texture);

    ImageFlipVertical(&out);

    if (zoom > 1) {
        // Nearest neighbour, always. Anything else resamples the art into a grid it
        // was not drawn on, which is the whole thing a contact sheet exists to let
        // you look at.
        ImageResizeNN(&out, out.width * zoom, out.height * zoom);
    }

    const bool wrote = ExportImage(out, path);

    UnloadImage(out);
    UnloadRenderTexture(sheet);

    std::printf("%d creatures, %d x %d at zoom %d -> %s\n", rows, cellW, cellH * rows, zoom, path);

    for (int r = 0; r < rows; r++) {
        const mob::Def &def = mob::kinds::Of(mob::Kind{r});

        std::printf("  %-10s %2d x %-2d texels = %3.0f x %-3.0f px   body %3.0f x %-3.0f   %s\n", def.name,
                    def.look.width, def.look.height, def.look.width * texel, def.look.height * texel,
                    def.build.width, def.build.height, def.temper);
    }

    return wrote ? 0 : 1;
}

const probes::Report row = {
    .name  = "--critters",
    .wants = 3,
    .shows = false,
    .blurb = "--critters out.png [zoom] - a contact sheet of every creature",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
