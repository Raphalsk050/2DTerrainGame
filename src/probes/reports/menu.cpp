#include "core/registry.h"
#include "probes/report.h"
#include "save/record.h"
#include "ui/menu.h"
#include "ui/skin.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// `--menu out.png [wide] [tall]` — every screen in front of the game, with worlds in
// the list.
//
// It exists for `--craft`'s reason (§28.7) and the reason `--hud` does (§25.5): **the
// interesting states of this screen cannot be photographed by playing.** An empty
// saves list is what a developer sees on every run; a list with six worlds in it, one
// of them being renamed in place and another with the delete question over it, takes a
// session of play to reach and is exactly where a layout goes wrong. And the pause
// screen is the same four buttons as the title meaning four different things, which no
// single picture can show.
//
// The saves it lists are real files, written and swept up: a mock list would be a
// picture of the mock, and the one thing worth checking here is that what `save::List`
// hands over is what gets drawn.
namespace {

constexpr const char *kUnder = "probe-menu-";

struct Case {
    const char *label;
    menu::Screen screen;

    // Whether the world is under the stack, which is what makes the title a pause
    // screen.
    bool paused = false;

    int picked        = 0;
    const char *renaming = nullptr;
    bool asking       = false;
};

const Case kCases[] = {
    {.label = "title", .screen = menu::Screen::Title},
    {.label = "saves", .screen = menu::Screen::Saves, .picked = 1},
    {.label = "renaming a world", .screen = menu::Screen::Saves, .picked = 2, .renaming = "the long stair"},
    {.label = "about to delete one", .screen = menu::Screen::Saves, .picked = 0, .asking = true},
    {.label = "new world", .screen = menu::Screen::NewWorld},
    {.label = "paused", .screen = menu::Screen::Title, .paused = true},
};

// What the probe puts in the list. Six, which is more than the box holds at the
// smallest window — so the scrollbar is drawn with something to say.
const char *kWorlds[] = {"first light", "the deep mine", "cliffside", "a very long name for a world",
                         "creative sandbox", "seed hunting"};

// A save with a head and nothing else in it.
//
// The head is all this screen ever reads — `save::Peek` stops at the first section —
// so a world's worth of journal would be a megabyte written to photograph six rows of
// text. What is being checked is the listing, and the listing is the head.
void Fake(const std::string &id, const char *name, int seed, bool creative, std::int64_t old) {
    save::Slot slot;

    slot.id = id;

    save::Writer out(slot.Path());

    if (!out.Ok()) return;

    out.Tag("save").Int(1).Done();
    out.Tag("name").Text(name).Done();
    out.Tag("seed").Int(seed).Done();
    out.Tag("mode").Text(creative ? "creative" : "survival").Done();
    out.Tag("clock").Real(3600.0f * 9.0f + 780.0f).Done();
    out.Tag("written").Int(old).Done();
    out.Tag("player").Real(0.0f).Real(0.0f).Int(20).Int(20).Done();
}

// A stand-in for the picture a save carries.
//
// Written at the shape a screenshot actually is — the window's — because that is the
// whole of what the preview box is sized against, and a stand-in of some other shape
// would photograph the letterboxing rather than the fit. Bands rather than a wash, on
// `--craft`'s reasoning: the thing being judged is drawn over a picture, and a picture
// that is one flat colour is the one background it will never have.
void Shot(const std::string &id, int wide, int tall) {
    save::Slot slot;

    slot.id = id;

    constexpr int kWide = 480;

    const int high = std::max(1, kWide * tall / wide);

    Image picture = GenImageColor(kWide, high, Color{126, 168, 214, 255});

    ImageDrawRectangle(&picture, 0, high * 6 / 10, kWide, high * 2 / 10, Color{96, 142, 78, 255});
    ImageDrawRectangle(&picture, 0, high * 8 / 10, kWide, high * 2 / 10, Color{58, 54, 62, 255});

    ExportImage(picture, slot.ShotPath().c_str());

    UnloadImage(picture);
}

// How many of the listed saves are this probe's.
int Mine() {
    int seen = 0;

    for (const save::Slot &slot : save::List()) {
        if (slot.id.rfind(kUnder, 0) == 0) seen++;
    }

    return seen;
}

void Sweep() {
    std::error_code oops;

    for (int i = 0; i < static_cast<int>(std::size(kWorlds)); i++) {
        std::filesystem::remove_all(std::string(save::Folder()) + "/" + kUnder + std::to_string(i), oops);
    }
}

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    // The screens are pure functions of the window, so the window has to be the size
    // being judged — and asked what it actually got, because there is a floor under it
    // and a size it refused is a picture of a layout nobody will ever see (§25.5).
    SetWindowSize((bench.argc >= 4) ? std::max(config::kMinScreenWidth, std::atoi(bench.argv[3])) : 1000,
                  (bench.argc >= 5) ? std::max(config::kMinScreenHeight, std::atoi(bench.argv[4])) : 640);

    const int wide = GetScreenWidth();
    const int tall = GetScreenHeight();

    // Off every button. A probe whose pointer happened to land on one would photograph
    // the hover state and call it the resting one.
    SetMousePosition(-100, -100);

    Sweep();

    // Measured from the real clock, so the "how long ago" line shows the answers a
    // player would actually see. A fixed timestamp made every row say the same thing —
    // a thousand days — which is a picture of the probe rather than of the screen.
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    for (int i = 0; i < static_cast<int>(std::size(kWorlds)); i++) {
        // Spread through the past, so the "how long ago" line has all four of its
        // answers in one picture rather than six rows saying "just now".
        constexpr std::int64_t kSpread[] = {30, 900, 7000, 40000, 200000, 900000};

        const std::int64_t when = now - kSpread[i];

        Fake(std::string(kUnder) + std::to_string(i), kWorlds[i], 1000 + i * 37, i == 4, when);
    }

    // One of them with a picture and the rest without, so the sheet shows both — the
    // fitted preview and the line that stands in for a save written before there was
    // one.
    Shot(std::string(kUnder) + "1", wide, tall);

    std::printf("\n%d x %d -> %s\n\n", wide, tall, path);

    std::vector<Image> shots;
    std::vector<std::string> notes;

    int listed = 0;

    for (const Case &one : kCases) {
        menu::Menu screen;

        if (one.paused) {
            screen.Play();

            save::Slot playing;

            playing.id   = std::string(kUnder) + "1";
            playing.name = kWorlds[1];

            screen.Playing(playing);
        }

        // Pushed only where it is not already the screen on top. The stack starts on
        // the title, so opening it again would make it two deep — and a title two deep
        // wears a back arrow pointing at itself, which is a picture of a bug the game
        // does not have.
        if (screen.Top() != one.screen) screen.Open(one.screen);

        if (one.screen == menu::Screen::Saves) {
            screen.Pick(one.picked);

            // The probe's own and nobody else's.
            //
            // `Menu::Listed` counts everything on the disk, and the disk may hold worlds
            // somebody is actually playing — which made the check read "7 of 6" the first
            // time it was run beside a real save. A check that counts other people's
            // things is a check that fails for the wrong reason.
            listed = std::max(listed, Mine());

            if (one.renaming != nullptr) screen.Renaming(one.renaming);
            if (one.asking) screen.Asking(std::string(kUnder) + std::to_string(one.picked));
        }

        RenderTexture2D shot = LoadRenderTexture(wide, tall);

        BeginTextureMode(shot);

        // Through `Menu::Draw` itself and never a copy of it, which is the whole point
        // of the sheet: what is being checked is the screen the player gets.
        screen.Draw();

        EndTextureMode();

        Image whole = LoadImageFromTexture(shot.texture);
        ImageFlipVertical(&whole);

        UnloadRenderTexture(shot);

        screen.Unload();

        shots.push_back(whole);
        notes.emplace_back(one.label);

        std::printf("  %-22s %s\n", one.label, one.paused ? "the title standing on a world" : "");
    }

    // The preview box is the shape a screenshot is.
    //
    // The fault this was added for: the box was a fixed share of the panel height and a
    // screenshot is the shape of the window, so the picture was fitted into it with a
    // band of empty panel above and below — a strip floating in a hole. Measured as the
    // share of the box a picture of the window shape actually covers, which is the whole
    // of it when the two agree.
    float fills = 0.0f;

    {
        menu::Menu one;

        const Rectangle box = one.Preview();

        const float scale = std::min(box.width / static_cast<float>(wide), box.height / static_cast<float>(tall));

        const float drawn = (static_cast<float>(wide) * scale) * (static_cast<float>(tall) * scale);

        fills = (box.width * box.height > 0.0f) ? drawn / (box.width * box.height) : 0.0f;
    }

    const bool fitted = fills > 0.98f;

    std::printf("  preview fills %.0f%% of its box\n", fills * 100.0f);

    // Every world written is a world listed. The one thing about this screen that can
    // be wrong without looking wrong: a head `save::Read` understands and `save::Peek`
    // does not is a save that loads perfectly and cannot be found.
    const bool found = listed == static_cast<int>(std::size(kWorlds));

    std::printf("\n  %d of %d worlds listed\n", listed, static_cast<int>(std::size(kWorlds)));

    // The sheet: two rows of three, because six side by side is a picture nobody can
    // read at any width a screen has.
    constexpr int kAcross    = 3;
    constexpr int kLabelTall = 22;
    constexpr int kPad       = 14;

    const int down = (static_cast<int>(shots.size()) + kAcross - 1) / kAcross;

    Image sheet = GenImageColor(kAcross * (wide + kPad) + kPad, down * (tall + kLabelTall + kPad) + kPad,
                                Color{16, 18, 22, 255});

    for (std::size_t i = 0; i < shots.size(); i++) {
        const int column = static_cast<int>(i) % kAcross;
        const int row    = static_cast<int>(i) / kAcross;

        const int x = kPad + column * (wide + kPad);
        const int y = kPad + row * (tall + kLabelTall + kPad);

        ImageDrawText(&sheet, notes[i].c_str(), x + 2, y, 16, skin::kText);

        ImageDraw(&sheet, shots[i], {0.0f, 0.0f, static_cast<float>(wide), static_cast<float>(tall)},
                  {static_cast<float>(x), static_cast<float>(y + kLabelTall), static_cast<float>(wide),
                   static_cast<float>(tall)},
                  WHITE);
    }

    const bool wrote = ExportImage(sheet, path);

    for (Image &shot : shots) UnloadImage(shot);

    UnloadImage(sheet);

    Sweep();

    std::printf("  %s\n", found ? "every world written is a world the screen lists"
                                : "NOT LISTED — a save on disk is missing from the screen");
    std::printf("  %s\n\n", fitted ? "a preview fills the box it is drawn in"
                                   : "LETTERBOXED — the picture box is not the shape of a screenshot");

    return (wrote && found && fitted) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--menu",
    .wants = 3,
    .shows = false,
    .blurb = "--menu out.png [wide] [tall] - every screen in front of the game, with worlds in the list",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
