#include "core/registry.h"
#include "core/stack.h"
#include "entity/life/health.h"
#include "entity/player/player_config.h"
#include "hand/editor.h"
#include "item/inventory.h"
#include "probes/report.h"
#include "ui/bottom.h"
#include "ui/hud.h"
#include "world/element.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// `--hud out.png [health] [wide] [tall]` — a picture of the strip along the foot of the
// screen.
//
// Every other thing this project draws has a way of being looked at without playing:
// `--critters` for a creature, `--probe` for the world, `tools/sheet.cpp` for a tree.
// The head-up display had none, and it showed — three separate files each put something
// twenty-odd pixels above the hotbar, none of them knowing about the others, and the
// only way to find out was to launch the game and look at a screenshot.
//
// So the strip is rendered on its own, over a plate that is deliberately *not* a flat
// grey: the readout is drawn over a moving world and the one thing it has to survive is
// the background changing under it. A layout judged against a neutral panel is a layout
// judged against the one background it will never have.
//
// It draws `hud::Strip` itself rather than a copy of it, which is the whole point — a
// probe that reproduced the layout would be checking the reproduction.
namespace {

// The plate behind it. Bands rather than a wash, standing in for the sky at the top of
// a hill and the dark of a cave at the bottom.
constexpr Color kSky   = {126, 168, 214, 255};
constexpr Color kGrass = {96, 142, 78, 255};
constexpr Color kRock  = {58, 54, 62, 255};
constexpr Color kDeep  = {22, 20, 26, 255};

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    // How much of the character is left, so the row of hearts can be judged part-full
    // rather than only at its two ends.
    const int left = (bench.argc >= 4) ? std::clamp(std::atoi(bench.argv[3]), 0, player_config::kHealth)
                                       : player_config::kHealth / 2;

    // The strip lays itself out against the window, so the window has to be the size
    // being judged. Resized rather than assumed: the whole bar is centred on the frame
    // and a picture taken at some other width is a picture of a layout nobody will see.
    SetWindowSize((bench.argc >= 5) ? std::max(320, std::atoi(bench.argv[4])) : 900,
                  (bench.argc >= 6) ? std::max(200, std::atoi(bench.argv[5])) : 420);

    // And then asked what it actually got. The window has a floor under it — see
    // `config::kMinScreenHeight` — so a size it refused is a picture of the strip
    // hanging off the bottom of a texture that is the wrong shape. Asked first, drawn
    // afterwards.
    const int wide = GetScreenWidth();
    const int tall = GetScreenHeight();

    // Which set of rules, because the vitals are survival's alone and an absence is not
    // something a single picture can show. Rendered twice and compared is the only way
    // to check a rule whose whole effect is that something is *not* there.
    const bool creative = (bench.argc >= 7) && TextIsEqual(bench.argv[6], "creative");

    const Gamemode mode = creative ? Gamemode::Creative : Gamemode::Survival;

    Inventory pack;

    pack.Stock();

    Editor hand;

    life::Health vigour{.most = player_config::kHealth, .now = left};

    RenderTexture2D shot = LoadRenderTexture(wide, tall);

    BeginTextureMode(shot);

    // Four bands, so the strip is seen against a bright sky, open ground, rock and the
    // dark all in one picture.
    const Color bands[] = {kSky, kGrass, kRock, kDeep};

    for (int b = 0; b < 4; b++) {
        DrawRectangle(b * wide / 4, 0, wide / 4, tall, bands[b]);
    }

    hud::Strip(pack, vigour, hand, mode);

    EndTextureMode();

    Image out = LoadImageFromTexture(shot.texture);

    ImageFlipVertical(&out);

    const bool wrote = ExportImage(out, path);

    UnloadImage(out);
    UnloadRenderTexture(shot);

    const bottom::Strip strip = bottom::Of();

    std::printf("\n%d x %d, %s, %d of %d health -> %s\n\n", wide, tall, creative ? "creative" : "survival", left,
                player_config::kHealth, path);

    // The rows and the gaps between them, in numbers, because "the information is too
    // close together" is a statement about pixels and the picture alone cannot be
    // measured with a ruler.
    std::printf("  %-8s y %6.0f .. %-6.0f  x %6.0f .. %-6.0f\n", "name", strip.name.y,
                strip.name.y + strip.name.height, strip.name.x, strip.name.x + strip.name.width);
    std::printf("  %-8s y %6.0f .. %-6.0f  x %6.0f .. %-6.0f\n", "vitals", strip.vitals.y,
                strip.vitals.y + strip.vitals.height, strip.vitals.x, strip.vitals.x + strip.vitals.width);
    std::printf("  %-8s y %6.0f .. %-6.0f  x %6.0f .. %-6.0f\n", "hand", strip.hand.y,
                strip.hand.y + strip.hand.height, strip.hand.x, strip.hand.x + strip.hand.width);
    std::printf("  %-8s y %6.0f .. %-6.0f  x %6.0f .. %-6.0f\n", "bar", strip.bar.y,
                strip.bar.y + strip.bar.height, strip.bar.x, strip.bar.x + strip.bar.width);

    const float overName = strip.vitals.y - (strip.name.y + strip.name.height);
    const float overRow  = strip.bar.y - (strip.vitals.y + strip.vitals.height);

    std::printf("\n  gaps: %.0f px under the name, %.0f px under the vitals\n", overName, overRow);

    // The fault this probe was written for. Rows that touch or overlap are the
    // complaint, and a number is the only way to be sure they do not.
    const bool crowded = overName < 1.0f || overRow < 1.0f;

    std::printf("  %s\n\n", crowded ? "CROWDED — rows are touching" : "every row has clear air under it");

    return (wrote && !crowded) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--hud",
    .wants = 3,
    .shows = false,
    .blurb = "--hud out.png [health] [wide] [tall] [creative] - the strip along the foot of the screen",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
