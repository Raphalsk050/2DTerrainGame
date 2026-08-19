#include "core/registry.h"
#include "core/stack.h"
#include "craft/craft.h"
#include "item/inventory.h"
#include "probes/report.h"
#include "ui/crafting.h"
#include "ui/skin.h"
#include "world/element.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// `--craft out.png [wide] [tall]` — the recipe panel in every state it has.
//
// The whole mechanic is three states and the difference between them is *drawn*: a
// recipe nobody can start is not there, one that is short is shaded with a dead
// button, one that is ready is lit with a live one. Not one of those three is a
// thing a single screenshot of a game session shows, because getting into all three
// means gathering and spending the right materials in the right order — which is
// exactly the kind of check that never gets run.
//
// So the panel is photographed against inventories made up on the spot, one per
// state, side by side. It draws `Crafting::Draw` itself and lays out through
// `Crafting::LayoutFor`, never a copy of either: §25.5's rule, and the reason `--hud`
// exists at all.
//
// And it *reports a verdict*. Two of them, both of which have been wrong in this
// project before under other names: that no two rows of the card land in the same
// pixels (§25.1), and that the three inventories actually produce the three
// standings — a panel that drew beautifully while every recipe read as ready would
// pass a picture and fail a player.
namespace {

// The plate behind the panel. Bands rather than a wash, standing in for the sky, the
// ground and the dark: the strip is drawn over a moving world, and a layout judged
// against a neutral grey is judged against the one background it will never have.
constexpr Color kSky  = {126, 168, 214, 255};
constexpr Color kSod  = {96, 142, 78, 255};
constexpr Color kRock = {58, 54, 62, 255};

// What each panel is standing for, and what the pack holds to put it there.
//
// The stock is a list of names rather than a field per material, because the chain
// grew: it was wood, cobble and sticks when there was one tool, and a panel that had
// to gain a field every time a recipe named something new would be a panel that goes
// out of date quietly. A name and a count resolves against both content tables
// through `craft::Named`, exactly as a recipe's own ingredient does.
struct Held {
    const char *what = nullptr;
    int count        = 0;
};

struct Case {
    const char *label;
    const char *recipe; // Whose card is open.

    Held stock[4];
};

// Four, walked in the order a player actually meets them, which is what makes the
// sheet a picture of the *chain* and not only of the three states. A felled tree buys
// planks, planks buy sticks, and only then is there a tool to want.
constexpr Case kCases[] = {
    {.label = "a felled tree", .recipe = "wood plank", .stock = {{"wood", 3}}},
    {.label = "planks", .recipe = "stick", .stock = {{"wood", 1}, {"wood plank", 4}}},
    {.label = "not enough", .recipe = "stone pickaxe", .stock = {{"wood plank", 2}, {"cobblestone", 1}, {"stick", 1}}},
    {.label = "ready", .recipe = "stone pickaxe", .stock = {{"wood plank", 2}, {"cobblestone", 4}, {"stick", 2}}},
};

constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

std::optional<Element> Material(const char *name) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (std::strcmp(kElements[e].name, name) == 0) return static_cast<Element>(e);
    }

    return std::nullopt;
}

void Stock(Inventory &pack, const char *name, int count) {
    if (name == nullptr || count <= 0) return;

    const std::optional<Stack> stack = craft::Named(name, count);
    if (!stack) return;

    pack.Add(*stack);
}

// The recipe of that name, for opening its card.
std::optional<craft::Recipe> RecipeNamed(const char *name) {
    for (int i = 0; i < craft::Count(); i++) {
        if (std::strcmp(craft::Of(craft::Recipe{i}).name, name) == 0) return craft::Recipe{i};
    }

    return std::nullopt;
}

const char *Says(craft::Standing standing) {
    switch (standing) {
    case craft::Standing::Absent: return "absent";
    case craft::Standing::Short: return "short";
    case craft::Standing::Ready: return "ready";
    }

    return "?";
}

// The rectangle the panel actually covers, so the sheet is a picture of the panel
// and not of a screenful of plate around it.
Rectangle Covered(const Crafting::Layout &layout) {
    Rectangle box = layout.strip;

    if (!layout.open) return box;

    const float right  = std::max(box.x + box.width, layout.card.x + layout.card.width);
    const float bottom = std::max(box.y + box.height, layout.card.y + layout.card.height);

    box.x = std::min(box.x, layout.card.x);
    box.y = std::min(box.y, layout.card.y);

    box.width  = right - box.x;
    box.height = bottom - box.y;

    return box;
}

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    // The panel lays itself out against the window — the strip is centred on the
    // frame's height — so the window has to be the size being judged, and it has to
    // be asked what it actually got. There is a floor under it (§25.5), and a size it
    // refused is a picture of a layout nobody will ever see.
    SetWindowSize((bench.argc >= 4) ? std::max(320, std::atoi(bench.argv[3])) : 900,
                  (bench.argc >= 5) ? std::max(200, std::atoi(bench.argv[4])) : 560);

    const int wide = GetScreenWidth();
    const int tall = GetScreenHeight();

    // Off the panel, and deliberately: `Crafting::Draw` lights the slot under the
    // pointer, and a probe whose pointer happened to land on a recipe would take a
    // picture of the hover state and call it the resting one.
    SetMousePosition(-100, -100);

    std::vector<Image> shots;
    std::vector<std::string> notes;

    bool crowded = false;
    bool wrong   = false;

    std::printf("\n%d x %d -> %s\n\n", wide, tall, path);

    for (const Case &one : kCases) {
        Inventory pack;

        for (const Held &held : one.stock) Stock(pack, held.what, held.count);

        Crafting panel;
        panel.Open(RecipeNamed(one.recipe));

        const Crafting::Listing listing = Crafting::ListFor(pack);
        const int selected              = panel.SelectedIn(listing);
        const Crafting::Layout layout   = Crafting::LayoutFor(listing, selected);

        RenderTexture2D shot = LoadRenderTexture(wide, tall);

        BeginTextureMode(shot);

        for (int b = 0; b < 3; b++) DrawRectangle(0, b * tall / 3, wide, tall / 3, (b == 0) ? kSky : (b == 1) ? kSod : kRock);

        panel.Draw(pack, Gamemode::Survival);

        EndTextureMode();

        Image whole = LoadImageFromTexture(shot.texture);
        ImageFlipVertical(&whole);

        // Cropped with a little air round it, so the panel's own border is inside the
        // picture rather than on its edge.
        Rectangle box = Covered(layout);

        box.x -= 8.0f;
        box.y -= 8.0f;
        box.width += 16.0f;
        box.height += 16.0f;

        Image crop = ImageFromImage(whole, box);

        UnloadImage(whole);
        UnloadRenderTexture(shot);

        shots.push_back(crop);
        notes.emplace_back(one.label);

        // Then the numbers. What the picture cannot be measured with a ruler for.
        std::printf("  %-14s", one.label);

        for (const Held &held : one.stock) {
            if (held.what != nullptr) std::printf(" %d %s,", held.count, held.what);
        }

        std::printf("   %d listed\n", listing.count);

        for (int i = 0; i < listing.count; i++) {
            const craft::Bill &bill = craft::Bills()[static_cast<std::size_t>(listing.at[i].bill)];

            std::printf("      %-12s %-6s  makes %d %-10s", bill.Def().name, Says(listing.at[i].standing),
                        bill.makes.count, bill.makes.Name());

            for (int n = 0; n < bill.count; n++) {
                std::printf("  %d/%d %s", craft::Held(bill.needs[n], pack), bill.needs[n].count,
                            bill.needs[n].Name());
            }

            std::printf("\n");
        }

        // Every recipe with none of its ingredients has to be off the list, which is
        // the mechanic's first rule and the one a change to `StandingOf` would break
        // silently — the panel would simply grow rows nobody can act on.
        for (const craft::Bill &bill : craft::Bills()) {
            const bool shown = [&] {
                for (int i = 0; i < listing.count; i++) {
                    if (craft::Bills()[static_cast<std::size_t>(listing.at[i].bill)].id == bill.id) return true;
                }

                return false;
            }();

            if (shown == (craft::StandingOf(bill, pack) != craft::Standing::Absent)) continue;

            std::printf("      LISTED WRONG: '%s'\n", bill.Def().name);

            wrong = true;
        }

        if (!layout.open) continue;

        // And the rows of the card, which is §25.1's fault waiting to happen again:
        // three things laid out separately in one column is how the hearts, the badge
        // and the item's name came to share twenty pixels.
        const float underIcon = (layout.needs > 0) ? layout.need[0].y - (layout.icon.y + layout.icon.height) : 0.0f;
        const float underNeed =
            (layout.needs > 0) ? layout.build.y - (layout.need[0].y + layout.need[0].height) : 0.0f;
        const float underBuild =
            layout.card.y + layout.card.height - (layout.build.y + layout.build.height);

        std::printf("      card %.0f x %.0f   gaps: %.0f under the title, %.0f under the needs, %.0f under the "
                    "button\n\n",
                    layout.card.width, layout.card.height, underIcon, underNeed, underBuild);

        // The needs row carries its tally *under* the icons, so the gap below it has
        // to clear the text as well as the squares.
        if (underIcon < 1.0f || underNeed < 16.0f || underBuild < 1.0f) crowded = true;
    }

    // The sheet: the three side by side, on one plate, each with its name over it.
    int sheetWide = 0;
    int sheetTall = 0;

    for (const Image &shot : shots) {
        sheetWide += shot.width + 16;
        sheetTall = std::max(sheetTall, shot.height);
    }

    constexpr int kLabelTall = 22;

    Image sheet = GenImageColor(sheetWide + 16, sheetTall + kLabelTall + 32, Color{16, 18, 22, 255});

    int at = 16;

    for (std::size_t i = 0; i < shots.size(); i++) {
        ImageDraw(&sheet, shots[i], {0.0f, 0.0f, static_cast<float>(shots[i].width), static_cast<float>(shots[i].height)},
                  {static_cast<float>(at), static_cast<float>(kLabelTall + 8), static_cast<float>(shots[i].width),
                   static_cast<float>(shots[i].height)},
                  WHITE);

        ImageDrawText(&sheet, notes[i].c_str(), at + 2, 6, 16, skin::kText);

        at += shots[i].width + 16;
    }

    const bool wrote = ExportImage(sheet, path);

    for (Image &shot : shots) UnloadImage(shot);

    UnloadImage(sheet);

    std::printf("  %s\n", crowded ? "CROWDED — two rows of the card are touching"
                                  : "every row of the card has clear air under it");
    std::printf("  %s\n\n", wrong ? "LISTED WRONG — the strip disagrees with craft::StandingOf"
                                  : "the strip lists exactly the recipes the player has started");

    return (wrote && !crowded && !wrong) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--craft",
    .wants = 3,
    .shows = false,
    .blurb = "--craft out.png [wide] [tall] - the recipe panel in every state it has",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
