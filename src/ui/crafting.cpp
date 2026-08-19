#include "ui/crafting.h"

#include "core/picture.h"
#include "ui/hotbar.h"
#include "ui/skin.h"

#include <algorithm>
#include <cmath>

namespace {

// The rows of the card, top to bottom, in screen pixels.
//
// Named rather than added up in place, because the one thing a card like this gets
// wrong is two rows landing in the same twenty pixels — which is exactly what
// `bottom::Of` exists to stop happening along the foot of the screen. `--craft`
// prints these and the gaps between them, so "the information is too close
// together" is a claim about numbers rather than an argument about a screenshot.
constexpr float kTitleTall = Crafting::kSlotSide;

// The blurb, in two lines of text.
//
// Two rather than one, and it is a fault this shipped with for one render: a single
// line held the card's width to whatever the shortest sentence in the recipe table
// happened to be, and the first blurb longer than that ran straight off the right
// edge and over the border. A card has to be able to hold what the table can say,
// so it wraps and the row is as tall as the wrap allows.
constexpr int kBlurbSize  = 12;
constexpr float kBlurbLine = 14.0f;
constexpr int kBlurbLines  = 2;
constexpr float kBlurbTall = kBlurbLine * kBlurbLines;
constexpr float kTallyTall = 16.0f;
constexpr float kBuildTall = 34.0f;
constexpr float kRowGap    = 10.0f;

constexpr float kNeedRow = Crafting::kNeedSide + kTallyTall;

constexpr float kCardTall = Crafting::kInset + kTitleTall + kRowGap + kBlurbTall + kRowGap + kNeedRow + kRowGap
                            + kBuildTall + Crafting::kInset;

// Room between two ingredient icons. Wide enough that two counts under them do not
// run together, which at three digits a side is what decides it.
constexpr float kNeedGap = 14.0f;

// How dark an unaffordable recipe is drawn.
//
// A scrim over the finished icon rather than a second, greyer picture: the shape
// stays exactly the shape, so a player learns what the thing looks like before they
// can make it, which is the whole reason it is on the strip at all.
//
// And that is also what puts a ceiling on it. At two thirds the axe went to a dark
// smudge and the shape it was supposed to be teaching was gone — which is a scrim
// doing the job of a blank slot. Just over half is enough to say *not yet* while
// leaving the silhouette to be read.
constexpr float kShaded = 0.54f;

// Text broken onto lines that fit a width, drawn from `at` downward.
//
// Broken between words and never inside one, and it stops rather than overflowing:
// a card is a fixed shape and the last thing it may do is grow to fit its contents,
// since everything under the blurb is laid out from a row that would then move.
void DrawWrapped(const char *text, Vector2 at, float wide, int size, int lines, Color colour) {
    const int length = TextLength(text);

    int from = 0;

    for (int line = 0; line < lines && from < length; line++) {
        int upto = from;
        int fits = from;

        // The longest run of whole words that measures inside the width. Measured
        // rather than counted in characters — the font is not fixed width, and a
        // count is right for one sentence and wrong for the next.
        while (upto <= length) {
            const bool boundary = upto == length || text[upto] == ' ';

            if (boundary) {
                if (MeasureText(TextSubtext(text, from, upto - from), size) <= static_cast<int>(wide)) fits = upto;
                else break;
            }

            upto++;
        }

        // One word longer than the whole line. Drawn anyway and allowed to be
        // clipped by nothing but the reader's patience, because the alternative is
        // an empty line and then an infinite loop.
        if (fits == from) fits = std::min(length, from + 1);

        DrawText(TextSubtext(text, from, fits - from), static_cast<int>(at.x),
                 static_cast<int>(at.y + static_cast<float>(line) * kBlurbLine), size, colour);

        from = fits;

        while (from < length && text[from] == ' ') from++;
    }
}

void Shadowed(const char *text, float x, float y, int size, Color colour) {
    DrawText(text, static_cast<int>(x) + 1, static_cast<int>(y) + 1, size, skin::kShadow);
    DrawText(text, static_cast<int>(x), static_cast<int>(y), size, colour);
}

// The picture of a stack, centred in a rectangle, at whatever whole number of
// pixels per texel fits.
//
// A whole number always, and never a fraction: a six-texel picture drawn at four
// and a half pixels a texel comes out with its columns alternating four wide and
// five, which is the one thing that must never happen to art of this size — the
// same rule `hotbar::kIconPixel` is fixed for.
void DrawInside(const Stack &stack, Rectangle at) {
    if (stack.Empty()) return;

    const float pixel = std::floor(std::min(at.width, at.height) * 0.82f / kPictureSide);
    const float drawn = pixel * kPictureSide;

    DrawPicture(PictureOf(stack),
                {std::floor(at.x + (at.width - drawn) / 2.0f), std::floor(at.y + (at.height - drawn) / 2.0f)}, pixel);
}

void DrawWell(Rectangle at, Color frame, float thick) {
    DrawRectangleRec(at, skin::kSlot);
    DrawRectangleLinesEx(at, thick, frame);
}

// The name of a recipe's product, beside the cursor.
//
// The *product's* name and never the recipe's own: one is a label a player reads
// and the other is a key in a table, and they are only the same string today.
void DrawTip(const char *text, Vector2 mouse) {
    const float width = static_cast<float>(MeasureText(text, 14));

    Rectangle box = {mouse.x + 14.0f, mouse.y + 14.0f, width + 16.0f, 24.0f};

    box.x = std::floor(std::min(box.x, static_cast<float>(GetScreenWidth()) - box.width - 4.0f));
    box.y = std::floor(std::min(box.y, static_cast<float>(GetScreenHeight()) - box.height - 4.0f));

    DrawRectangleRec(box, skin::kTip);
    DrawRectangleLinesEx(box, 1.0f, skin::kEdge);
    DrawText(text, static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 5.0f), 14, skin::kText);
}

} // namespace

int Crafting::Fits() {
    // One row is a slot and the padding over it; the strip's own bottom padding and
    // the margins at both ends are what is left over.
    const float usable = static_cast<float>(GetScreenHeight()) - 2.0f * kMargin - kPad;

    return std::clamp(static_cast<int>(usable / (kSlotSide + kPad)), 1, Listing::kMost);
}

Crafting::Listing Crafting::ListFor(const Inventory &pack) {
    Listing listing{};

    const std::vector<craft::Bill> &bills = craft::Bills();

    const int room = Fits();

    for (std::size_t i = 0; i < bills.size(); i++) {
        const craft::Standing standing = craft::StandingOf(bills[i], pack);

        // A recipe none of whose ingredients the player holds is not on the strip.
        // That is the mechanic and not a tidying: the list is meant to be an answer
        // to "what is what I am carrying worth", and a wall of things needing
        // materials that do not exist yet is not that answer.
        if (standing == craft::Standing::Absent) continue;

        if (listing.count >= room) {
            listing.cut++;
            continue;
        }

        listing.at[listing.count] = {.bill = static_cast<int>(i), .standing = standing};
        listing.count++;
    }

    return listing;
}

Crafting::Layout Crafting::LayoutFor(const Listing &listing, int selected) {
    Layout layout{};

    if (listing.count <= 0) return layout;

    const auto rows = static_cast<float>(listing.count);

    const float wide = kSlotSide + 2.0f * kPad;
    const float tall = rows * kSlotSide + (rows + 1.0f) * kPad;

    // Down the left edge and centred on it. Centred rather than pinned to the top,
    // so a strip that grows by a row grows about the middle instead of walking down
    // the screen — the eye keeps its place.
    layout.strip = {kMargin, std::floor((static_cast<float>(GetScreenHeight()) - tall) / 2.0f), wide, tall};

    for (int i = 0; i < listing.count; i++) {
        layout.slot[i] = {layout.strip.x + kPad, layout.strip.y + kPad + static_cast<float>(i) * (kSlotSide + kPad),
                          kSlotSide, kSlotSide};
    }

    if (selected < 0 || selected >= listing.count) return layout;

    layout.open = true;

    const float top = std::clamp(layout.slot[selected].y - kInset, kMargin,
                                 static_cast<float>(GetScreenHeight()) - kCardTall - kMargin);

    layout.card = {layout.strip.x + layout.strip.width + kGap, std::floor(top), kCardWide, kCardTall};

    layout.icon = {layout.card.x + kInset, layout.card.y + kInset, kSlotSide, kSlotSide};

    const float needsY = layout.card.y + kInset + kTitleTall + kRowGap + kBlurbTall + kRowGap;

    const craft::Bill &bill = craft::Bills()[static_cast<std::size_t>(listing.at[selected].bill)];

    layout.needs = bill.count;

    for (int i = 0; i < bill.count && i < craft::kMaxNeeds; i++) {
        layout.need[i] = {layout.card.x + kInset + static_cast<float>(i) * (kNeedSide + kNeedGap), needsY, kNeedSide,
                          kNeedSide};
    }

    layout.build = {layout.card.x + kInset, needsY + kNeedRow + kRowGap, kCardWide - 2.0f * kInset, kBuildTall};

    return layout;
}

int Crafting::SelectedIn(const Listing &listing) const {
    if (!open_) return -1;

    const std::vector<craft::Bill> &bills = craft::Bills();

    for (int i = 0; i < listing.count; i++) {
        if (bills[static_cast<std::size_t>(listing.at[i].bill)].id == *open_) return i;
    }

    // The recipe is no longer listed, which happens the moment the last of an
    // ingredient is spent — including by the very build that just succeeded. The
    // card goes with it rather than hanging over a strip it is not attached to.
    return -1;
}

bool Crafting::Contains(Vector2 screen, const Inventory &pack, Gamemode mode) const {
    if (mode != Gamemode::Survival) return false;

    const Listing listing = ListFor(pack);
    const Layout layout   = LayoutFor(listing, SelectedIn(listing));

    if (listing.count <= 0) return false;

    if (CheckCollisionPointRec(screen, layout.strip)) return true;

    return layout.open && CheckCollisionPointRec(screen, layout.card);
}

Crafting::Gesture Crafting::Update(Inventory &pack, Gamemode mode) {
    Gesture gesture{};

    // Creative has no crafting, and the test is here rather than at the call site
    // for §25.4's reason: the rule about which mode a piece of the interface belongs
    // to has one home, and it is the piece itself. A player in creative already has
    // every item in the palette, so a recipe there would be a longer way of taking
    // one.
    if (mode != Gamemode::Survival) return gesture;

    const Listing listing = ListFor(pack);
    if (listing.count <= 0) return gesture;

    const Layout layout = LayoutFor(listing, SelectedIn(listing));

    const Vector2 mouse = GetMousePosition();

    const bool on = CheckCollisionPointRec(mouse, layout.strip)
                    || (layout.open && CheckCollisionPointRec(mouse, layout.card));

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return gesture;
    if (!on) return gesture;

    // Anything landing on the panel belongs to the panel, whether or not it lands on
    // a control. A click on the card's background that fell through to the world
    // would dig a hole behind a window the player is reading.
    gesture.took = true;

    const int selected = SelectedIn(listing);

    if (layout.open && CheckCollisionPointRec(mouse, layout.build)) {
        const craft::Bill &bill = craft::Bills()[static_cast<std::size_t>(listing.at[selected].bill)];

        // Refused by `craft::Make` as well, which is the one that matters: the panel
        // draws the button dark and the click is turned away by the rule rather than
        // by the drawing. A guard that lived only in the interface would be a guard
        // a second caller could walk past.
        if (listing.at[selected].standing != craft::Standing::Ready) {
            gesture.said = "not enough for that yet";

            return gesture;
        }

        const craft::Made made = craft::Make(bill, pack);

        gesture.built    = made.done;
        gesture.overflow = made.overflow;

        if (made.done && !made.overflow.Empty()) gesture.said = "no room — the rest is on the ground";

        return gesture;
    }

    for (int i = 0; i < listing.count; i++) {
        if (!CheckCollisionPointRec(mouse, layout.slot[i])) continue;

        const craft::Recipe id = craft::Bills()[static_cast<std::size_t>(listing.at[i].bill)].id;

        // Clicking the one that is already open shuts it, which is what a strip of
        // toggles is expected to do and is the only way back to a bare strip.
        if (open_ && *open_ == id) open_.reset();
        else open_ = id;

        return gesture;
    }

    return gesture;
}

void Crafting::Draw(const Inventory &pack, Gamemode mode) const {
    if (mode != Gamemode::Survival) return;

    const Listing listing = ListFor(pack);
    if (listing.count <= 0) return;

    const int selected  = SelectedIn(listing);
    const Layout layout = LayoutFor(listing, selected);

    const std::vector<craft::Bill> &bills = craft::Bills();

    const Vector2 mouse = GetMousePosition();

    DrawRectangleRec(layout.strip, skin::kPanel);
    DrawRectangleLinesEx(layout.strip, 2.0f, skin::kEdge);

    const char *hovered = nullptr;

    for (int i = 0; i < listing.count; i++) {
        const Listing::Row &row  = listing.at[i];
        const craft::Bill &bill  = bills[static_cast<std::size_t>(row.bill)];
        const bool ready         = row.standing == craft::Standing::Ready;
        const bool showing       = i == selected;
        const Rectangle at       = layout.slot[i];
        const bool under         = CheckCollisionPointRec(mouse, at);

        DrawWell(at, skin::kSlot, 1.0f);
        DrawInside(bill.makes, at);

        // The scrim, over the picture and under the frame. Under the frame on
        // purpose: the frame is the part that says *which* of the two states this
        // is, and shading it as well would leave the eye nothing crisp to read.
        if (!ready) DrawRectangleRec(at, Fade(skin::kPanel, kShaded));

        // How many come out, where a slot would put how many are held. One recipe
        // hands over eight sticks and another one axe, and that is worth knowing
        // from the strip rather than only from the card.
        if (bill.makes.count > 1) {
            const char *many = TextFormat("%d", bill.makes.count);

            Shadowed(many, at.x + at.width - static_cast<float>(MeasureText(many, 10)) - 4.0f,
                     at.y + at.height - 12.0f, 10, ready ? skin::kAccent : skin::kDim);
        }

        const Color frame = showing  ? skin::kOutline
                            : ready  ? (under ? skin::kAccent : Fade(skin::kAccent, 0.55f))
                                     : Fade(skin::kEdge, under ? 0.9f : 0.45f);

        DrawRectangleLinesEx(at, showing ? 3.0f : 1.0f, frame);

        if (under && !showing) hovered = bill.makes.Name();
    }

    if (layout.open && selected >= 0) {
        const Listing::Row &row = listing.at[selected];
        const craft::Bill &bill = bills[static_cast<std::size_t>(row.bill)];
        const bool ready        = row.standing == craft::Standing::Ready;

        DrawRectangleRec(layout.card, skin::kPanel);
        DrawRectangleLinesEx(layout.card, 2.0f, skin::kEdge);

        DrawWell(layout.icon, Fade(skin::kEdge, 0.6f), 1.0f);
        DrawInside(bill.makes, layout.icon);

        const float textX = layout.icon.x + layout.icon.width + kGap;

        DrawText(bill.makes.Name(), static_cast<int>(textX), static_cast<int>(layout.icon.y + 4.0f), 18, skin::kText);

        DrawText(bill.makes.count > 1 ? TextFormat("makes %d", bill.makes.count) : "makes one",
                 static_cast<int>(textX), static_cast<int>(layout.icon.y + 27.0f), 12, skin::kMuted);

        DrawWrapped(bill.Def().blurb, {layout.card.x + kInset, layout.card.y + kInset + kTitleTall + kRowGap},
                    kCardWide - 2.0f * kInset, kBlurbSize, kBlurbLines, skin::kDim);

        for (int i = 0; i < layout.needs; i++) {
            const Stack &need = bill.needs[i];
            const Rectangle at = layout.need[i];

            const int held   = craft::Held(need, pack);
            const bool met   = held >= need.count;

            DrawWell(at, Fade(met ? skin::kMet : skin::kShort, 0.7f), 1.0f);
            DrawInside(need, at);

            if (!met) DrawRectangleRec(at, Fade(skin::kPanel, kShaded * 0.7f));

            // Held over wanted, and both of them, which is the whole of what the
            // player needs to decide what to go and get. A required count alone —
            // which is what the game this borrows from prints — answers "what does
            // it cost" and leaves "how much more" to be worked out against a bag
            // that is not on screen.
            const char *tally = TextFormat("%d/%d", held, need.count);

            Shadowed(tally, at.x + (at.width - static_cast<float>(MeasureText(tally, 11))) / 2.0f,
                     at.y + at.height + 3.0f, 11, met ? skin::kMet : skin::kShort);
        }

        // The button. Amber and legible when it can be pressed, sunk into the panel
        // when it cannot — which is the whole of the state, said twice over, in the
        // face and in the label.
        const Rectangle build = layout.build;
        const bool under      = CheckCollisionPointRec(mouse, build);

        const Color face = ready ? (under ? skin::kAccent : Fade(skin::kAccent, 0.86f)) : Color{40, 44, 54, 255};

        DrawRectangleRec(build, face);
        DrawRectangleLinesEx(build, 2.0f, ready ? Fade(skin::kOutline, 0.5f) : Fade(skin::kEdge, 0.5f));

        const char *label = ready ? "BUILD" : "NOT ENOUGH";
        const int width   = MeasureText(label, 16);

        DrawText(label, static_cast<int>(build.x + (build.width - static_cast<float>(width)) / 2.0f),
                 static_cast<int>(build.y + (build.height - 16.0f) / 2.0f), 16,
                 ready ? Color{28, 24, 16, 255} : skin::kDim);
    }

    if (hovered != nullptr) DrawTip(hovered, mouse);
}
