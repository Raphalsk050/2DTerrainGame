#include "core/registry.h"
#include "core/picture.h"
#include "core/stack.h"
#include "craft/craft.h"
#include "entity/fixture.h"
#include "item/inventory.h"
#include "item/slots.h"
#include "probes/report.h"
#include "ui/pack.h"
#include "ui/skin.h"
#include "ui/store.h"
#include "world/element.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// `--chest out.png [wide] [tall]` — the store, at every size it joins to.
//
// It exists for the reason `--craft` does (§28.7): **no single screenshot of a session
// shows the mechanic**. Getting a bank of three on screen means felling a wood, making
// twenty-four planks, crafting three chests and standing them in a row with room to
// spare — which is exactly the check nobody runs, and the sizes it exercises are the
// ones that break. Ninety-six slots at the bar's own forty-four-pixel square is six
// hundred and fifty pixels of grid, and the window opens six hundred tall.
//
// Four verdicts, and each is about a fault a picture of the others would not show.
//
//   - **Every slot is inside the frame.** The whole reason `pack::Of` derives a texel
//     rather than using the bar's. A row of a chest off the bottom of the screen is
//     things the player put away and cannot get back, and it is invisible in a
//     screenshot that was taken at the developer's window size.
//   - **Nothing overlaps anything.** §25.1's lesson, which this layout has more chances
//     to repeat than any other in the game: two panels, a bin, a field, three buttons
//     and a column of headers, all laid out against each other.
//   - **Sorting loses nothing.** Counted per kind either side, because "it looks tidy"
//     is not the question — a sort that dropped the odd stack into a ruled row it had
//     no room for would look perfect and cost the player a diamond.
//   - **Breaking one unit takes only its own.** Rule six of the chest, checked through
//     `Fixtures` itself rather than by reasoning about it: three joined, one filled per
//     unit, the middle one dug up, and the other two still holding exactly what they
//     held.
namespace {

// The plate behind the panels. Bands rather than a wash, standing in for the sky, the
// ground and the dark: these are drawn over a moving world and the one thing they have
// to survive is the background changing under them.
constexpr Color kSky  = {126, 168, 214, 255};
constexpr Color kSod  = {96, 142, 78, 255};
constexpr Color kRock = {58, 54, 62, 255};

struct Case {
    const char *label;
    int units;
    bool ruling;
    const char *seeking = nullptr;

    // Creative replaces the grid with the palette and hangs a row of tabs off the
    // panel's top edge — which is the same edge the bin sits on. Two things claiming one
    // strip is the fault this whole layout exists to stop (§25.1), and it is a fault no
    // survival picture can show, so one case is drawn in the other mode.
    Gamemode mode = Gamemode::Survival;
};

// One, two and three, because the layout is a different shape at each and the third is
// the one that does not fit at the bar's own slot size. The fourth is the same three
// with a search running, which is a state of the *drawing* and leaves no trace anywhere
// else — so this is the only place it can be looked at.
constexpr Case kCases[] = {
    {.label = "one chest", .units = 1, .ruling = false},
    {.label = "two joined", .units = 2, .ruling = false},
    {.label = "three joined, rows set aside", .units = 3, .ruling = true},
    {.label = "searching for wood", .units = 3, .ruling = false, .seeking = "wood"},
    {.label = "creative, tabs and bin", .units = 1, .ruling = false, .mode = Gamemode::Creative},
};

std::optional<Stack> Some(const char *name, int count) {
    return craft::Named(name, count);
}

// Enough of enough different things to fill the grid past one screenful and to give
// the sort something to do.
void Fill(slots::Bank &bank) {
    static const char *kSpread[] = {"cobblestone", "rock",  "soil",       "sand",   "coal", "wood",
                                    "wood plank",  "stick", "wood shovel", "torch", "iron", "apple"};

    int n = 0;

    for (const char *what : kSpread) {
        std::optional<Stack> stack = Some(what, 7);
        if (!stack.has_value()) continue;

        // Held to what one slot of that thing may hold, because writing straight into a
        // slot goes round the limit `Fill` keeps — and a stack of seven wood shovels is
        // a state the game cannot reach. The sort splits them back out into sevens of
        // one, which made the sheet a wall of shovels and said nothing about anything.
        stack->count = std::min(stack->count, stack->Limit());

        // Written straight into slots rather than poured in through `Add`, and that is
        // the whole point of this function: `Add` tops up an alike stack before it fills
        // an empty one, so three sevens of cobblestone poured in come out as one
        // twenty-one and the sort has nothing to gather. What a played chest looks like
        // is partial stacks of one thing lying in several places, and this is the only
        // way to build that state on purpose.
        //
        // Seven apart, which is coprime with every bank size a chest joins to (32, 64 and
        // 96) — so the stride visits a fresh slot every time instead of landing back on
        // one it has already used and quietly writing over a kind.
        for (int i = 0; i < 3; i++, n++) {
            if (n >= bank.Size()) return;

            bank.At((n * 7) % bank.Size()) = *stack;
        }
    }
}

bool Overlaps(Rectangle a, Rectangle b) {
    // A hair of slack, because two boxes sharing an edge are not overlapping and float
    // arithmetic can put them a thousandth over one.
    a.x += 0.5f;
    a.y += 0.5f;
    a.width -= 1.0f;
    a.height -= 1.0f;

    return a.width > 0.0f && a.height > 0.0f && CheckCollisionRecs(a, b);
}

// Every box on the screen, named, so a clash can be reported as two names rather than
// as two rectangles.
struct Box {
    std::string name;
    Rectangle at{};
};

void Boxes(const pack::Layout &laid, bool ruling, bool creative, std::vector<Box> &out) {
    out.clear();

    out.push_back({"pack", laid.panel});
    out.push_back({"bin", laid.trash});

    // The tabs hang off the same top edge the bin sits on, so they are boxes here rather
    // than being trusted to be somewhere else.
    if (creative) {
        for (int tab = 0; tab < Inventory::kTabs; tab++) {
            out.push_back({std::string("tab ") + Inventory::NameOf(static_cast<Inventory::Tab>(tab)), laid.Tab(tab)});
        }
    }

    if (!laid.Storing()) return;

    out.push_back({"store", laid.store});
    out.push_back({"hint", laid.hint});
    out.push_back({"search", laid.search});
    out.push_back({"find", laid.find});
    out.push_back({"sort", laid.sort});
    out.push_back({"rows", laid.rules});

    if (!ruling) return;

    for (int row = 0; row < laid.storeRows; row++) {
        out.push_back({"row " + std::to_string(row), laid.Row(row)});
    }
}

// The two panels' own rectangles are what must not touch; everything inside one of
// them is expected to be inside it.
bool Nested(const std::string &inner, const std::string &outer) {
    if (outer == "store") return inner == "search" || inner == "find" || inner == "sort" || inner == "rows";

    // A tab overlaps the panel's top edge by a hair on purpose, so the one in front reads
    // as part of the panel rather than as a button floating over it.
    if (outer == "pack") return inner.rfind("tab ", 0) == 0;

    return false;
}

// Where the whole arrangement reaches, so the sheet is a picture of it and not of a
// screenful of plate around it.
Rectangle Covered(const pack::Layout &laid, bool ruling, bool creative) {
    std::vector<Box> boxes;

    Boxes(laid, ruling, creative, boxes);

    Rectangle box = boxes.front().at;

    float right  = box.x + box.width;
    float bottom = box.y + box.height;

    for (const Box &one : boxes) {
        box.x  = std::min(box.x, one.at.x);
        box.y  = std::min(box.y, one.at.y);
        right  = std::max(right, one.at.x + one.at.width);
        bottom = std::max(bottom, one.at.y + one.at.height);
    }

    box.width  = right - box.x;
    box.height = bottom - box.y;

    return box;
}

// The chests as they stand on the hillside, at one, two and three joined.
//
// Drawn through `fixture::FaceOf` — the same call the world's own draw makes — because
// what is being looked at is whether a bank reads as *one* chest. Three boxes with two
// seams down them is what it looked like before the faces existed, and it is a fault a
// picture of the panel cannot show: the panel is the same however many units are
// behind it.
//
// It also reports the seam as a number. A picture says "that looks joined"; what makes
// it checkable is that the border texel is gone from the edges that continue, and that
// is countable.
Image Faces(int zoom, int &seams) {
    const fixture::Def &def = fixture::Of(fixture::Kind::Chest);

    constexpr int kRuns[] = {1, 2, 3};
    constexpr int kPad    = 6;

    int wide = kPad;

    for (const int run : kRuns) wide += run * kPictureSide * zoom + kPad;

    Image sheet = GenImageColor(wide, kPictureSide * zoom + kPad * 2, Color{58, 54, 62, 255});

    int at = kPad;

    seams = 0;

    for (const int run : kRuns) {
        for (int unit = 0; unit < run; unit++) {
            const fixture::Piece piece = (run <= 1)          ? fixture::Piece::Alone
                                       : (unit == 0)        ? fixture::Piece::Left
                                       : (unit == run - 1)  ? fixture::Piece::Right
                                                            : fixture::Piece::Middle;

            const Picture &face = fixture::FaceOf(def, piece);

            for (int row = 0; row < kPictureSide; row++) {
                for (int column = 0; column < kPictureSide; column++) {
                    const Color *tone = ToneAt(face, face.art[row][column]);
                    if (tone == nullptr) continue;

                    ImageDrawRectangle(&sheet, at + column * zoom, kPad + row * zoom, zoom, zoom, *tone);
                }
            }

            // The seam this unit's right edge makes with the next.
            //
            // Not simply "dark texels touching", which was the first version and was
            // wrong: the iron band is the darkest tone and is *meant* to run straight
            // through the join. What a border is, is dark at the edge with something
            // lighter beside it — a vertical line rather than part of a horizontal one.
            // So a pair is a seam only where the texels just inside them are not dark
            // too.
            if (unit + 1 < run) {
                const fixture::Piece nextPiece = (unit + 1 == run - 1) ? fixture::Piece::Right
                                                                      : fixture::Piece::Middle;

                const Picture &next = fixture::FaceOf(def, nextPiece);

                for (int row = 0; row < kPictureSide; row++) {
                    const bool mine = face.art[row][kPictureSide - 1] == 'd';
                    const bool them = next.art[row][0] == 'd';

                    if (!mine || !them) continue;

                    const bool runs = face.art[row][kPictureSide - 2] == 'd' && next.art[row][1] == 'd';

                    if (!runs) seams++;
                }
            }

            at += kPictureSide * zoom;
        }

        at += kPad;
    }

    return sheet;
}

// Rule six, checked through the fixtures themselves.
//
// A probe that reasoned about it from the design would be checking the design. What
// makes this worth its runtime is that it goes through `Place`, `Run`, `Store` and
// `Remove` — the same four calls the hand makes — so a join rule written one way and a
// break rule written another is caught here rather than by a player who lost a chest.
bool Breaking(int &joined, int &refused, int &keptBefore, int &keptAfter) {
    fixture::Fixtures chests;

    constexpr int kCy = 0;

    // Three in a row, then a fourth beside them — which must be refused, or "up to
    // three" is not a rule.
    for (int i = 0; i < 3; i++) chests.Place(fixture::Kind::Chest, i, kCy);

    refused = chests.Place(fixture::Kind::Chest, 3, kCy) ? 0 : 1;

    const fixture::Joined run = chests.Run(1, kCy);

    joined = run.count;

    // One kind per unit, so what comes back can be told apart by name rather than by
    // counting.
    const char *kPer[] = {"coal", "iron", "gold"};

    slots::Bank whole = chests.Store(run);

    const int per = fixture::Of(fixture::Kind::Chest).slots;

    for (int i = 0; i < run.count && i < 3; i++) {
        // Through the *bank*, at the first slot of each unit's stretch of it, which is
        // how the panel writes and is therefore the thing worth checking. Asking the
        // fixtures for one unit's store on its own is not a thing that exists: `Run`
        // answers with the whole joined run for any cell of it, which is the point of it
        // — a store is what the player opened, and they opened all three.
        const std::optional<Stack> stack = Some(kPer[i], 9);

        if (stack.has_value() && i * per < whole.Size()) whole.Put(i * per, *stack);
    }

    keptBefore = whole.Held();

    // The middle one, which is the case that would go wrong if a bank were one buffer:
    // taking it out has to leave a run of one either side, each with its own things
    // still in it.
    fixture::Placed taken{};

    if (!chests.Remove(1, kCy, taken)) return false;

    int left = 0;

    for (const Stack &stack : taken.kept) left += stack.count;

    // What the two survivors still hold, asked afresh — they are two runs of one now.
    keptAfter = 0;

    for (const int cx : {0, 2}) {
        slots::Bank one = chests.Store(chests.Run(cx, kCy));

        keptAfter += one.Held();
    }

    // Nine came out with the chest that was dug, and the other eighteen are still
    // standing.
    return left == 9 && keptAfter == keptBefore - 9;
}

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    // The layout is a pure function of the window, so the window has to be the size
    // being judged — and it has to be *asked* what it actually got, because there is a
    // floor under it (§25.5) and a size it refused is a picture of a layout nobody will
    // ever see.
    SetWindowSize((bench.argc >= 4) ? std::max(config::kMinScreenWidth, std::atoi(bench.argv[3])) : 1000,
                  (bench.argc >= 5) ? std::max(config::kMinScreenHeight, std::atoi(bench.argv[4])) : 600);

    const int wide = GetScreenWidth();
    const int tall = GetScreenHeight();

    // Off both panels, deliberately: a slot under the pointer draws its tooltip, and a
    // probe whose pointer happened to land on one would photograph the hover state and
    // call it the resting one.
    SetMousePosition(-100, -100);

    std::printf("\n%d x %d -> %s\n\n", wide, tall, path);

    std::vector<Image> shots;
    std::vector<std::string> notes;

    bool offscreen = false;
    bool crowded   = false;
    bool lost      = false;

    for (const Case &one : kCases) {
        const int rows = one.units * fixture::Of(fixture::Kind::Chest).Rows();

        const pack::Layout laid = pack::Of(rows);

        // The store's own storage, standing in for the chests. The panel takes a bank
        // and a rule per row and knows nothing about fixtures, which is what lets this
        // run with no world anywhere near it.
        std::vector<Stack> kept(static_cast<std::size_t>(one.units * fixture::Of(fixture::Kind::Chest).slots));
        std::vector<slots::Kind> rows_(static_cast<std::size_t>(rows));
        std::vector<slots::Kind *> rules;

        for (slots::Kind &rule : rows_) rules.push_back(&rule);

        slots::Bank bank(kept.data(), static_cast<int>(kept.size()));

        Fill(bank);

        // A row set aside, so the third sheet shows what the headers look like with
        // something in them.
        if (one.ruling) {
            const std::optional<Stack> cobble = Some("cobblestone", 1);

            if (cobble.has_value()) rows_[1] = slots::Kind::Of(*cobble);
        }

        Inventory pack;

        pack.Add(*Some("wood plank", 40));
        pack.Add(*Some("chest", 3));

        // Sorting, before anything is drawn, and counted either side.
        //
        // Per kind and not as one total, because a total is blind to the fault that
        // actually happens: two kinds swapped, or one turned into another, both leave
        // the count where it was.
        std::vector<std::pair<slots::Kind, int>> before;

        for (int slot = 0; slot < bank.Size(); slot++) {
            const Stack &at = bank.At(slot);
            if (at.Empty()) continue;

            const slots::Kind kind = slots::Kind::Of(at);

            auto found = std::find_if(before.begin(), before.end(),
                                      [&kind](const auto &pair) { return pair.first.Same(kind); });

            if (found == before.end()) before.push_back({kind, at.count});
            else found->second += at.count;
        }

        slots::Sort(bank, pack::kStoreColumns, rows_.data(), static_cast<int>(rows_.size()));

        for (const auto &[kind, count] : before) {
            const Stack like = {.holds = kind.holds, .what = kind.what, .count = 1};

            if (bank.Tally(like) != count) lost = true;
        }

        // And that the rule was honoured: what a row was set aside for is what is in it.
        int strayed = 0;

        for (int row = 0; row < rows; row++) {
            if (!rows_[static_cast<std::size_t>(row)].Any()) continue;

            for (int col = 0; col < pack::kStoreColumns; col++) {
                const Stack &at = bank.At(row * pack::kStoreColumns + col);

                if (!at.Empty() && !rows_[static_cast<std::size_t>(row)].Is(at)) strayed++;
            }
        }

        // Every slot inside the frame, which is the verdict this probe was written for.
        int outside = 0;

        for (int slot = 0; slot < laid.StoreSlots(); slot++) {
            const Rectangle box = laid.StoreSlot(slot);

            if (box.x < 0.0f || box.y < 0.0f || box.x + box.width > static_cast<float>(wide)
                || box.y + box.height > static_cast<float>(tall)) {
                outside++;
            }
        }

        for (int slot = 0; slot < Inventory::kSlots; slot++) {
            const Rectangle box = laid.Slot(slot);

            if (box.x < 0.0f || box.y < 0.0f || box.x + box.width > static_cast<float>(wide)
                || box.y + box.height > static_cast<float>(tall)) {
                outside++;
            }
        }

        if (laid.trash.y < 0.0f) outside++;

        // And the line of words under the store, which is the one part of this layout
        // that is text rather than a box and is therefore the one most easily left
        // hanging off the bottom edge — see `pack::Layout::hint`.
        if (laid.Storing() && laid.hint.y + laid.hint.height > static_cast<float>(tall)) outside++;

        if (outside > 0) offscreen = true;
        if (strayed > 0) lost = true;

        // Nothing over anything.
        std::vector<Box> boxes;

        Boxes(laid, one.ruling, one.mode == Gamemode::Creative, boxes);

        std::string clash;

        for (std::size_t a = 0; a < boxes.size(); a++) {
            for (std::size_t b = a + 1; b < boxes.size(); b++) {
                if (Nested(boxes[b].name, boxes[a].name) || Nested(boxes[a].name, boxes[b].name)) continue;
                if (!Overlaps(boxes[a].at, boxes[b].at)) continue;

                if (!clash.empty()) clash += ", ";

                clash += boxes[a].name + "/" + boxes[b].name;
            }
        }

        if (!clash.empty()) crowded = true;

        // The picture.
        Store panel;

        RenderTexture2D shot = LoadRenderTexture(wide, tall);

        BeginTextureMode(shot);

        for (int b = 0; b < 3; b++) {
            DrawRectangle(0, b * tall / 3, wide, tall / 3, (b == 0) ? kSky : (b == 1) ? kSod : kRock);
        }

        // Through the panels themselves and never a copy of either — §25.5's rule, and
        // the reason a sheet like this is worth having at all. `Store::Ruling` is set by
        // a click, so the click is made rather than the state reached round the side.
        panel.Ruling(one.ruling);
        panel.Seeking(one.seeking);

        pack.Draw(one.mode, laid);
        panel.Draw(pack, bank, rules, laid);

        EndTextureMode();

        Image whole = LoadImageFromTexture(shot.texture);
        ImageFlipVertical(&whole);

        Rectangle box = Covered(laid, one.ruling, one.mode == Gamemode::Creative);

        box.x = std::max(0.0f, box.x - 10.0f);
        box.y = std::max(0.0f, box.y - 10.0f);

        box.width  = std::min(static_cast<float>(wide) - box.x, box.width + 20.0f);
        box.height = std::min(static_cast<float>(tall) - box.y, box.height + 20.0f);

        Image crop = ImageFromImage(whole, box);

        UnloadImage(whole);
        UnloadRenderTexture(shot);

        shots.push_back(crop);
        notes.emplace_back(one.label);

        std::printf("  %-30s %2d rows, %3d slots   texel %.0f px, slot %.0f px\n", one.label, laid.storeRows,
                    laid.StoreSlots(), laid.metric.pixel, laid.metric.side);
        std::printf("      pack %.0f x %.0f at (%.0f, %.0f)   store %.0f x %.0f at (%.0f, %.0f)\n", laid.panel.width,
                    laid.panel.height, laid.panel.x, laid.panel.y, laid.store.width, laid.store.height, laid.store.x,
                    laid.store.y);
        std::printf("      %d slots outside the frame, %d strayed into a row set aside%s%s\n\n", outside, strayed,
                    clash.empty() ? "" : ", clash: ", clash.c_str());
    }

    // And the rules the panel cannot show: joining, and what a break pays out.
    int joined     = 0;
    int refused    = 0;
    int keptBefore = 0;
    int keptAfter  = 0;

    const bool broke = Breaking(joined, refused, keptBefore, keptAfter);

    std::printf("  joining: %d stood together, a fourth was %s\n", joined, (refused == 1) ? "refused" : "ALLOWED");
    std::printf("  breaking: %d held, the middle one dug, %d still standing in the other two\n\n", keptBefore,
                keptAfter);

    // The chests themselves, as they stand in the world.
    int seams = 0;

    // Drawn large. It is a six-texel picture and the question being asked of it is
    // whether a join is visible, which at one pixel a texel it is not — for anybody.
    Image faces = Faces(14, seams);

    std::printf("  joined: %d border texels left on a seam (0 is one chest, not three)\n\n", seams);

    // The sheet: the panels side by side, on one plate, each with its name over it, and
    // the chests along the foot of it.
    int sheetWide = 0;
    int sheetTall = 0;

    for (const Image &shot : shots) {
        sheetWide += shot.width + 16;
        sheetTall = std::max(sheetTall, shot.height);
    }

    constexpr int kLabelTall = 22;

    Image sheet = GenImageColor(sheetWide + 16, sheetTall + kLabelTall + 32 + faces.height + 16,
                                Color{16, 18, 22, 255});

    int at = 16;

    for (std::size_t i = 0; i < shots.size(); i++) {
        ImageDraw(&sheet, shots[i],
                  {0.0f, 0.0f, static_cast<float>(shots[i].width), static_cast<float>(shots[i].height)},
                  {static_cast<float>(at), static_cast<float>(kLabelTall + 8), static_cast<float>(shots[i].width),
                   static_cast<float>(shots[i].height)},
                  WHITE);

        ImageDrawText(&sheet, notes[i].c_str(), at + 2, 6, 16, skin::kText);

        at += shots[i].width + 16;
    }

    ImageDraw(&sheet, faces, {0.0f, 0.0f, static_cast<float>(faces.width), static_cast<float>(faces.height)},
              {16.0f, static_cast<float>(kLabelTall + 8 + sheetTall + 12), static_cast<float>(faces.width),
               static_cast<float>(faces.height)},
              WHITE);

    ImageDrawText(&sheet, "one, two and three joined", 16, kLabelTall + 8 + sheetTall - 6, 16, skin::kMuted);

    const bool wrote = ExportImage(sheet, path);

    for (Image &shot : shots) UnloadImage(shot);

    UnloadImage(faces);
    UnloadImage(sheet);

    std::printf("  %s\n", offscreen ? "OFF THE SCREEN — a slot of the store is outside the frame"
                                    : "every slot of every store is inside the frame");
    std::printf("  %s\n", crowded ? "CROWDED — two parts of the layout are over one another"
                                  : "no two parts of the layout touch");
    std::printf("  %s\n", lost ? "SORTED WRONG — the store does not hold what it held"
                               : "sorting keeps every count and honours every row set aside");
    std::printf("  %s\n", broke ? "breaking one unit takes its own things and leaves the rest"
                                : "BROKE WRONG — a unit dug up took more or less than its own");
    std::printf("  %s\n\n", seams == 0 ? "joined chests are drawn as one chest, with no seam down them"
                                       : "SEAMED — a bank is drawn as separate boxes standing in a row");

    return (wrote && !offscreen && !crowded && !lost && broke && seams == 0) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--chest",
    .wants = 3,
    .shows = false,
    .blurb = "--chest out.png [wide] [tall] - the store at every size it joins to",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
