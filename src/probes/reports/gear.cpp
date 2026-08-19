#include "core/registry.h"
#include "core/stack.h"
#include "entity/player/player.h"
#include "entity/player/player_config.h"
#include "entity/player/player_input.h"
#include "item/icon.h"
#include "item/inventory.h"
#include "item/item_def.h"
#include "probes/report.h"
#include "ui/hotbar.h"
#include "ui/skin.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// `--gear out.png [zoom]` — every item as a slot draws it, the durability bar over its
// whole range, and the tool in the character's hand.
//
// It exists for §25.5's reason, which this project has now paid for twice: everything
// drawn here has to have a way of being looked at without playing, or the only way to
// find a fault is to launch the game, get into the state that shows it, and take a
// screenshot. Item art is the worst case of that — the pictures are the one part of the
// game a player looks at every single second, and until now the only way to see them all
// at once was to switch to creative and open the palette.
//
// Three things on one sheet, and each of them is a thing a picture of the other two
// would not show:
//
//   - **The slot.** An authored file and a hand-drawn `Picture` have to come out the
//     same size, or a bar of tools is a row of things at different scales. Drawn through
//     `hotbar::DrawSlot` itself and never a copy of it, so the frame, the picture, the
//     count and the bar are laid out by the code the game lays them out with.
//   - **The bar.** Its whole job is to be different at different amounts, and one still
//     of one tool says nothing about that. Six of the same pickaxe at six amounts does,
//     and the shrinking is *measured* off the finished picture rather than trusted.
//   - **The hand.** This is the one §24.4 is about. Everything else on the sheet draws
//     an icon directly, which proves the files are right without proving the game will
//     ever show them: what puts a tool in the character's hand is `Player::Draw`, and a
//     sheet that reproduced the pose would go on looking perfect while the game drew
//     nothing at all. So the character is drawn by `Player::Draw`, aimed by
//     `Player::Update`, and the tool comes off the slot the bar would have handed it.
namespace {

// The plate. Bands rather than a wash, for `--hud`'s reason: these are drawn over a
// moving world and the one thing they have to survive is the background changing under
// them, so a sheet judged against a neutral grey is judged against the one background
// they will never have.
constexpr Color kInk  = {16, 18, 22, 255};
constexpr Color kSky  = {126, 168, 214, 255};
constexpr Color kSod  = {96, 142, 78, 255};
constexpr Color kRock = {58, 54, 62, 255};

constexpr int kMargin = 16;
constexpr int kLabel  = 24;

// One item's cell on the sheet: a slot with room for its name under it. Wide enough for
// "diamond pickaxe" at ten point, which is the longest name in the table and the one
// that decides this number.
constexpr int kCellWide = 88;
constexpr int kCellTall = 60;
constexpr int kColumns  = 7;

constexpr int kPoses = 6;

// How wide a pose's cell has to be at a given zoom.
//
// Worked out from the character's own reach rather than picked, and that is a lesson
// this sheet taught on its first render: at a cell of 120 and a zoom of 4 the arm ran
// off the edge of its own box and the tool was drawn entirely outside it — which looks
// exactly like a tool that is not drawn at all, and was read that way for a while.
//
// The furthest anything gets from the body's centre is the arm's length plus the tool
// hanging off the end of it, and both of those are `player_config`'s to say. Twelve
// pixels of air either side so the outline of a cell never touches what is in it.
int PoseSide(float zoom) {
    const float reach = player_config::kAttackReach + player_config::kHeldTool;

    return static_cast<int>(2.0f * reach * zoom) + 24;
}

// How much of the tool is gone, at each step of the ramp. Ending at "one blow left"
// rather than at nothing, because a spent tool does not exist — the slot is emptied on
// the blow that spends it, so the last thing a player ever sees is this.
constexpr float kRamp[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

constexpr int kRampSteps = static_cast<int>(sizeof(kRamp) / sizeof(kRamp[0]));

// Where a pose aims, and what it is holding. A tool points where the cursor does, so the
// aim is half of what there is to look at: an axe swung overhead and an axe held out at
// arm's length are the same picture rotated, and only a sheet with both on it shows that
// the rotation is right.
struct Pose {
    const char *item; // Nothing for a bare hand.
    float angle;      // Degrees, clockwise from due right.
};

const Pose kRow1[kPoses] = {
    {"iron pickaxe", 0.0f},  {"iron pickaxe", 45.0f},   {"iron pickaxe", 90.0f},
    {"iron pickaxe", 180.0f}, {"iron pickaxe", 225.0f}, {"iron pickaxe", 270.0f},
};

// The second row ends on the pair the third verdict is measured from: the same pose, the
// same aim, one with a tool and one without. Anything but the tool being identical is
// what makes the count of differing pixels mean "the tool is drawn" and nothing else.
const Pose kRow2[kPoses] = {
    {"wood axe", 0.0f},  {"wood axe", 180.0f},   {"gold shovel", 0.0f},
    {"diamond axe", 180.0f}, {nullptr, 0.0f},    {"copper pickaxe", 0.0f},
};

Stack Carrying(const char *name) {
    if (name == nullptr) return {};

    const std::optional<Item> found = item::Find(name);

    // A pose naming an item that is not there would draw an empty hand and look like a
    // tool that failed to load. Said out loud instead.
    if (!found.has_value()) {
        std::printf("  MISSING: no item called '%s'\n", name);

        return {};
    }

    return ItemsOf(*found, 1);
}

// A stack of `def` worn to `share` of the way through its life.
Stack Worn(Item id, float share) {
    Stack stack = ItemsOf(id, 1);

    const int lasts = stack.Lasts();

    // One short of the whole, never the whole: a tool worn all the way through is a tool
    // that no longer exists, and drawing an empty bar would be drawing a thing the game
    // cannot show.
    stack.wear = static_cast<std::uint16_t>(std::min(static_cast<int>(share * lasts), std::max(lasts - 1, 0)));

    return stack;
}

// How much of a rectangle of the finished picture is drawn in a saturated colour.
//
// Which is the bar and nothing else: the slot is {60,66,78} and the groove under the bar
// is near black, both of them within a few points of grey, while the fill runs from a
// full green to a full red. Measured off the exported image rather than recomputed from
// `Stack::Whole`, because what is being checked is the *drawing* — arithmetic that is
// right and a bar that is drawn at a fixed width would pass any check of the arithmetic.
int Coloured(const Image &image, Rectangle box) {
    int count = 0;

    for (int y = static_cast<int>(box.y); y < static_cast<int>(box.y + box.height); y++) {
        for (int x = static_cast<int>(box.x); x < static_cast<int>(box.x + box.width); x++) {
            if (x < 0 || y < 0 || x >= image.width || y >= image.height) continue;

            const Color at = GetImageColor(image, x, y);

            const int high = std::max({at.r, at.g, at.b});
            const int low  = std::min({at.r, at.g, at.b});

            if (high - low > 40) count++;
        }
    }

    return count;
}

// How many pixels two equally sized rectangles of the picture disagree about.
int Apart(const Image &image, Rectangle one, Rectangle two) {
    int count = 0;

    for (int y = 0; y < static_cast<int>(one.height); y++) {
        for (int x = 0; x < static_cast<int>(one.width); x++) {
            const Color a = GetImageColor(image, static_cast<int>(one.x) + x, static_cast<int>(one.y) + y);
            const Color b = GetImageColor(image, static_cast<int>(two.x) + x, static_cast<int>(two.y) + y);

            if (a.r != b.r || a.g != b.g || a.b != b.b) count++;
        }
    }

    return count;
}

void Title(const char *text, int x, int y) {
    DrawText(text, x, y, 16, skin::kText);
}

// One character, aimed and holding whatever the pose says, inside its own cell.
//
// Aimed through `Player::Update` rather than by reaching into the character, and that is
// not ceremony: which way a tool is mirrored comes off `Player::facing_`, which is set by
// the aim and by nothing else, so a sheet that set the direction some other way would be
// a sheet in which the mirroring is never exercised.
void Stand(Player &player, const World &world, const Pose &pose, Rectangle cell, float zoom) {
    // The character stands at the origin of its own cell, in world units, and the camera
    // puts that where the cell is. Placed afresh each time so that no pose inherits
    // whatever the last one left the body doing.
    player.PlaceAt({0.0f, 0.0f});

    const float radians = pose.angle * DEG2RAD;

    PlayerInput input{};

    // Far enough out that the direction is the direction and not a rounding of it.
    input.aimWorld = {player.Centre().x + std::cos(radians) * 200.0f,
                      player.Centre().y + std::sin(radians) * 200.0f};

    // No time passes. The aim is set, the state is worked out, and the body does not
    // fall out of the cell while the picture is being taken.
    player.Update(input, world, 0.0f);

    const Stack held = Carrying(pose.item);

    // Clipped to its own cell, so a tool swung out of one pose does not appear in the
    // next one's picture and read as that pose being wrong.
    BeginScissorMode(static_cast<int>(cell.x), static_cast<int>(cell.y), static_cast<int>(cell.width),
                     static_cast<int>(cell.height));

    DrawRectangleRec(cell, kRock);

    Camera2D camera{};

    camera.target   = player.Centre();
    camera.offset   = {cell.x + cell.width * 0.5f, cell.y + cell.height * 0.5f};
    camera.zoom     = zoom;
    camera.rotation = 0.0f;

    BeginMode2D(camera);

    player.Draw(held);

    EndMode2D();

    EndScissorMode();

    DrawRectangleLinesEx(cell, 1.0f, skin::kEdge);
}

// Whether a tool's lifetime survives being used and being carried about.
//
// Nothing in a picture can show either, and both are the kind of fault that is invisible
// until a player says "my pickaxe repaired itself". Driven through the inventory's own
// methods rather than by writing `wear` and reading it back: what is being checked is
// that `Take`, `Put` and `Add` carry a field they were all written before there was one,
// and a check that set the field itself would be checking `Stack` against `Stack`.
//
// The digging hand cannot be driven from here — `Editor::Update` reads the mouse — so
// what is exercised is the one function the spade, the axe and the fist all go through.
bool LifetimeHolds(const char *name) {
    const std::optional<Item> found = item::Find(name);

    if (!found.has_value()) {
        std::printf("  MISSING: no item called '%s'\n", name);

        return false;
    }

    const int lasts = Def(*found).tool.lasts;

    Inventory pack;

    pack.Select(0);
    pack.Put(0, ItemsOf(*found, 1));

    // Every blow but the last leaves it in the hand, and the last one takes it away. Off
    // by one in either direction is a tool that outlives its own figure or one that goes
    // a blow early, and only counting all the way to the end catches it.
    for (int blow = 1; blow < lasts; blow++) {
        if (pack.WearHeld()) {
            std::printf("  EARLY: '%s' broke on blow %d of %d\n", name, blow, lasts);

            return false;
        }
    }

    if (!pack.WearHeld()) {
        std::printf("  ENDLESS: '%s' survived all %d of its blows\n", name, lasts);

        return false;
    }

    if (!pack.Held().Empty()) {
        std::printf("  LEFTOVER: '%s' was spent and its slot still holds something\n", name);

        return false;
    }

    // And then the fault this was really written for: a worn tool put down and picked
    // back up is a worn tool. Rebuilt field by field — which is what `Fill`, `Take` and
    // `Put` all used to do — it comes back as good as new, and dropping a pickaxe on the
    // ground is a free repair.
    pack.Put(0, ItemsOf(*found, 1));

    for (int blow = 0; blow < lasts / 2; blow++) pack.WearHeld();

    const int worn = pack.Held().wear;

    const Stack lifted = pack.Take(0, 1);

    if (lifted.wear != worn) {
        std::printf("  RESET: lifting '%s' off its slot forgot %d of %d blows\n", name, worn - lifted.wear, worn);

        return false;
    }

    Inventory other;

    other.Add(lifted);

    if (other.At(0).wear != worn) {
        std::printf("  RESET: putting '%s' away forgot %d of %d blows\n", name, worn - other.At(0).wear, worn);

        return false;
    }

    return true;
}

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    // How much the hand section is blown up by. The character is 26 pixels tall and the
    // tool 22, which at one to one is a thumbnail — and what is being judged here is
    // where the haft sits in the hand, which is a question about single pixels.
    const float zoom = (bench.argc >= 4) ? std::max(1.0f, static_cast<float>(std::atof(bench.argv[3]))) : 3.0f;

    const std::vector<const ItemDef *> &rows = item::Table().All();

    const int itemRows = (static_cast<int>(rows.size()) + kColumns - 1) / kColumns;

    const int pose = PoseSide(zoom);

    const int wide = kMargin * 2 + std::max(kColumns * kCellWide, kPoses * pose);

    const int tall = kMargin * 2 + kLabel + itemRows * kCellTall + 12 + kLabel + kCellTall + 12 + kLabel
                     + 2 * pose;

    RenderTexture2D shot = LoadRenderTexture(wide, tall);

    // Which of the items are drawn from a file, gathered while the sheet is drawn so the
    // list is of what actually loaded rather than of what the table asked for.
    std::vector<std::string> missing;

    // Where each step of the ramp put its bar, for measuring afterwards.
    Rectangle bars[kRampSteps]{};

    Rectangle bare{};
    Rectangle armed{};

    std::printf("\n");

    BeginTextureMode(shot);

    ClearBackground(kInk);

    int y = kMargin;

    // ---- every item, as a slot draws it --------------------------------------------

    Title("every item, drawn the way a slot draws it", kMargin, y);

    y += kLabel;

    for (std::size_t i = 0; i < rows.size(); i++) {
        const ItemDef &def = *rows[i];

        const int column = static_cast<int>(i) % kColumns;
        const int row    = static_cast<int>(i) / kColumns;

        const float x = static_cast<float>(kMargin + column * kCellWide);
        const float top = static_cast<float>(y + row * kCellTall);

        // The three grounds under the row of slots, so the frame and the picture are
        // seen against sky, grass and rock in the one sheet.
        const Color ground = (row % 3 == 0) ? kSky : (row % 3 == 1) ? kSod : kRock;

        DrawRectangle(static_cast<int>(x), static_cast<int>(top), kCellWide, kCellTall, ground);

        const Rectangle slot = {x + (kCellWide - hotbar::kSlotSide) / 2.0f, top + 2.0f, hotbar::kSlotSide,
                                hotbar::kSlotSide};

        const Stack stack = ItemsOf(static_cast<Item>(static_cast<int>(i)), def.stack);

        hotbar::DrawSlot(stack, slot, false);

        const int width = MeasureText(def.name, 10);

        DrawText(def.name, static_cast<int>(x + (kCellWide - width) / 2.0f),
                 static_cast<int>(top + hotbar::kSlotSide + 4.0f), 10, skin::kShadow);
        DrawText(def.name, static_cast<int>(x + (kCellWide - width) / 2.0f) - 1,
                 static_cast<int>(top + hotbar::kSlotSide + 3.0f), 10, skin::kText);

        if (def.art != nullptr && icon::Art(def) == nullptr) missing.emplace_back(def.name);
    }

    y += itemRows * kCellTall + 12;

    // ---- the bar, over its whole range ----------------------------------------------

    Title("the durability bar, from full to the last blow", kMargin, y);

    y += kLabel;

    // A diamond pickaxe, because it is the longest-lived thing in the game: if the bar
    // can be drawn at a fifth of fifteen hundred and sixty-one it can be drawn at a fifth
    // of anything.
    const std::optional<Item> shown = item::Find("diamond pickaxe");

    for (int step = 0; step < kRampSteps; step++) {
        const float x   = static_cast<float>(kMargin + step * kCellWide);
        const float top = static_cast<float>(y);

        DrawRectangle(static_cast<int>(x), static_cast<int>(top), kCellWide, kCellTall, kRock);

        const Rectangle slot = {x + (kCellWide - hotbar::kSlotSide) / 2.0f, top + 2.0f, hotbar::kSlotSide,
                                hotbar::kSlotSide};

        const Stack stack = shown.has_value() ? Worn(*shown, kRamp[step]) : Stack{};

        hotbar::DrawSlot(stack, slot, false);

        // The picture's own box inside the slot, which is where `icon::DrawWear` put the
        // bar. Worked out the way `hotbar::DrawSlot` works it out, so a change to either
        // moves both.
        const float drawn  = hotbar::kIconPixel * kPictureSide;
        const float corner = std::floor(slot.y + (slot.height - drawn) / 2.0f);

        bars[step] = {std::floor(slot.x + (slot.width - drawn) / 2.0f), corner + drawn - 6.0f, drawn, 6.0f};

        const char *note = TextFormat("%d/%d", stack.Left(), stack.Lasts());
        const int width  = MeasureText(note, 10);

        DrawText(note, static_cast<int>(x + (kCellWide - width) / 2.0f),
                 static_cast<int>(top + hotbar::kSlotSide + 4.0f), 10, skin::kText);
    }

    y += kCellTall + 12;

    // ---- in the hand ----------------------------------------------------------------

    Title("in the hand, through Player::Draw itself", kMargin, y);

    y += kLabel;

    Player player({0.0f, 0.0f});

    for (int i = 0; i < kPoses; i++) {
        const Rectangle cell = {static_cast<float>(kMargin + i * pose), static_cast<float>(y),
                                static_cast<float>(pose), static_cast<float>(pose)};

        Stand(player, *bench.world, kRow1[i], cell, zoom);
    }

    for (int i = 0; i < kPoses; i++) {
        const Rectangle cell = {static_cast<float>(kMargin + i * pose), static_cast<float>(y + pose),
                                static_cast<float>(pose), static_cast<float>(pose)};

        Stand(player, *bench.world, kRow2[i], cell, zoom);

        if (kRow2[i].item == nullptr) bare = cell;
        if (i == kPoses - 1) armed = cell;
    }

    EndTextureMode();

    Image out = LoadImageFromTexture(shot.texture);

    ImageFlipVertical(&out);

    const bool wrote = ExportImage(out, path);

    // ---- the verdicts ----------------------------------------------------------------

    std::printf("%d x %d, %d items, zoom %.0f -> %s\n\n", wide, tall, static_cast<int>(rows.size()), zoom, path);

    int authored = 0;

    for (const ItemDef *def : rows) {
        if (def->art != nullptr) authored++;
    }

    std::printf("  %d of %d items are drawn from a file", authored, static_cast<int>(rows.size()));

    for (const std::string &name : missing) std::printf("\n  MISSING: '%s' names art that would not load", name.c_str());

    std::printf("\n\n  %-10s %10s %10s\n", "worn", "left", "bar px");

    bool ramped = true;

    int last = -1;

    for (int step = 0; step < kRampSteps; step++) {
        const int drawn = Coloured(out, bars[step]);

        std::printf("  %-10.0f%% %10.0f%% %10d\n", kRamp[step] * 100.0f, (1.0f - kRamp[step]) * 100.0f, drawn);

        // Strictly shorter every time, and never nothing. The first is the whole of what
        // the bar is for; the second is the case a player meets last and most often, and
        // a bar that has rounded away is a tool they believe is already gone.
        if (last >= 0 && drawn >= last) ramped = false;
        if (drawn <= 0) ramped = false;

        last = drawn;
    }

    std::printf("\n  %s\n", ramped ? "the bar is shorter at every step, and never nothing"
                                   : "FLAT — the bar does not shrink as the tool wears");

    // The one this sheet is really for. Everything above draws an icon; only this says
    // the *game* puts it in the character's hand.
    const int shows = Apart(out, bare, armed);

    std::printf("  %d px between an empty hand and the same hand holding a pickaxe\n", shows);

    // A number rather than "not zero", because the arm and the body are identical in
    // both cells and a stray pixel of antialiasing would pass a test written that way. A
    // tool 22 pixels long at this zoom covers some hundreds.
    const bool inHand = shows > 200;

    std::printf("  %s\n", inHand ? "the tool is drawn in the hand"
                                 : "EMPTY-HANDED — Player::Draw shows nothing of what is held");

    // And the figure the bar is drawn from, which is the one thing on this sheet that is
    // not a picture. Both ends of the ladder, because 59 blows and 1561 are far enough
    // apart that an off-by-one can hide in one of them and not in the other.
    const bool lasts = LifetimeHolds("wood pickaxe") && LifetimeHolds("diamond pickaxe");

    std::printf("  %s\n\n", lasts ? "a tool lasts exactly its own figure, and stays worn when it is carried"
                                  : "the lifetime does not survive being used or being carried");

    UnloadImage(out);
    UnloadRenderTexture(shot);

    return (wrote && missing.empty() && ramped && inHand && lasts) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--gear",
    .wants = 3,
    .shows = false,
    .blurb = "--gear out.png [zoom] - every item as a slot draws it, the wear bar, and the tool in hand",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
