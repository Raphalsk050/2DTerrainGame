#include "menu.h"

#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ------------------------------------------------------------------ the paint
//
// The bar's palette and not a second one. A menu drawn in colours of its own
// would be a different program's front end bolted to this one, and the first
// thing a player sees should be made of the same things the game is.
constexpr Color kGround = {18, 20, 26, 255};  // behind everything
constexpr Color kPanel  = {30, 34, 42, 255};  // a box standing on it
constexpr Color kSlot   = {60, 66, 78, 255};  // a button at rest
constexpr Color kHot    = {84, 92, 108, 255}; // and under the pointer
constexpr Color kEdge   = {12, 14, 18, 255};
constexpr Color kInk    = {238, 243, 250, 255};
constexpr Color kFaint  = {150, 158, 172, 255};
constexpr Color kAccent = {150, 214, 120, 255};

constexpr float kButtonWide = 260.0f;
constexpr float kButtonHigh = 46.0f;
constexpr float kGap        = 14.0f;

// Type sizes, and there are three of them on purpose: the name of the game, the
// name of a screen, and everything else. A fourth would be a decision to make
// every time something is added.
constexpr int kTitleType = 44;
constexpr int kHeadType  = 26;
constexpr int kBodyType  = 20;

float Wide() {
    return static_cast<float>(GetScreenWidth());
}

float High() {
    return static_cast<float>(GetScreenHeight());
}

// Floored, always. Text on a half pixel is text with a soft edge, and the whole
// screen is text.
Rectangle Middle(float y, float wide, float high) {
    return {std::floor((Wide() - wide) / 2.0f), std::floor(y), wide, high};
}

void Label(const char *text, Rectangle box, int size, Color colour) {
    const int wide = MeasureText(text, size);

    DrawText(text, static_cast<int>(box.x + (box.width - static_cast<float>(wide)) / 2.0f),
             static_cast<int>(box.y + (box.height - static_cast<float>(size)) / 2.0f), size, colour);
}

bool Over(Rectangle box) {
    return CheckCollisionPointRec(GetMousePosition(), box);
}

// A press is the click *and* the pointer being on the thing, asked in one place
// so that no screen can test one and forget the other.
bool Pressed(Rectangle box, bool enabled = true) {
    return enabled && Over(box) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void Box(Rectangle at, Color fill) {
    DrawRectangleRec(at, fill);
    DrawRectangleLinesEx(at, 2.0f, kEdge);
}

void Button(Rectangle at, const char *label, bool enabled = true) {
    const bool hot = enabled && Over(at);

    DrawRectangleRec(at, enabled ? (hot ? kHot : kSlot) : kPanel);
    DrawRectangleLinesEx(at, 2.0f, hot ? kInk : kEdge);

    Label(label, at, kBodyType, enabled ? kInk : kFaint);
}

// The arrow every screen but the first one wears in its top left corner.
//
// Drawn rather than written, because it is the one control that means the same
// thing on every screen and a word would have to be the right word in whatever
// language the game is eventually in. Two strokes and a head: back along the
// top, and down and round.
Rectangle BackAt() {
    return {24.0f, 24.0f, 46.0f, 46.0f};
}

void DrawBack(bool shown) {
    if (!shown) return;

    const Rectangle at = BackAt();
    const bool hot     = Over(at);

    DrawRectangleRec(at, hot ? kHot : kSlot);
    DrawRectangleLinesEx(at, 2.0f, hot ? kInk : kEdge);

    const Color ink = hot ? kInk : kFaint;

    const Vector2 middle = {at.x + at.width / 2.0f, at.y + at.height / 2.0f};

    const float reach = at.width * 0.26f;

    // The shaft, and the two barbs of the head. Straight lines rather than an
    // arc: at this size a curve is three pixels of aliasing and reads as a smudge.
    DrawLineEx({middle.x - reach, middle.y}, {middle.x + reach, middle.y}, 3.0f, ink);
    DrawLineEx({middle.x - reach, middle.y}, {middle.x, middle.y - reach * 0.8f}, 3.0f, ink);
    DrawLineEx({middle.x - reach, middle.y}, {middle.x, middle.y + reach * 0.8f}, 3.0f, ink);
}

// The name of the screen, in a box across the top. Every screen but the title has
// one, in the same place, so that the eye never has to look for where it is.
Rectangle HeadAt() {
    return Middle(High() * 0.06f, 360.0f, 56.0f);
}

void DrawHead(const char *name) {
    const Rectangle at = HeadAt();

    Box(at, kPanel);
    Label(name, at, kHeadType, kInk);
}

// ------------------------------------------------------------- the layouts
//
// One function per screen, each a pure function of the window size, and both the
// input pass and the draw pass call it. Nothing is remembered between them: a
// layout held from Update and drawn a frame later is a button that has moved and
// a click that lands where it used to be, which is the same fault the editor's
// cursor is written to avoid one frame further out.

struct TitleAt {
    Rectangle name;
    Rectangle play;
    Rectangle party;
    Rectangle options;
    Rectangle quit;
};

TitleAt LayTitle() {
    TitleAt at{};

    at.name = Middle(High() * 0.13f, std::min(Wide() * 0.62f, 620.0f), 104.0f);

    // The stack of buttons is hung from the middle of what is left under the name
    // rather than from a fraction of the screen, so a tall window spreads the gap
    // instead of pushing the buttons off the bottom of a short one.
    const float first = std::floor(High() * 0.46f);

    at.play    = Middle(first + 0.0f * (kButtonHigh + kGap), kButtonWide, kButtonHigh);
    at.party   = Middle(first + 1.0f * (kButtonHigh + kGap), kButtonWide, kButtonHigh);
    at.options = Middle(first + 2.0f * (kButtonHigh + kGap), kButtonWide, kButtonHigh);
    at.quit    = Middle(first + 3.0f * (kButtonHigh + kGap), kButtonWide, kButtonHigh);

    return at;
}

struct SavesAt {
    Rectangle list;
    Rectangle bar;
    Rectangle preview;

    Rectangle fresh;
    Rectangle load;
    Rectangle edit;
    Rectangle drop;
};

SavesAt LaySaves() {
    SavesAt at{};

    const float top   = High() * 0.26f;
    const float high  = High() * 0.44f;
    const float whole = std::min(Wide() * 0.82f, 900.0f);

    const float left = std::floor((Wide() - whole) / 2.0f);

    // The list takes the greater share and the preview the rest, which is the
    // shape of the question: the player is choosing from a column of names and
    // glancing at the picture, not the other way about.
    const float listWide = std::floor(whole * 0.58f);

    at.list = {left, std::floor(top), listWide, std::floor(high)};

    // The scrollbar inside the list's own right edge rather than beside it, so the
    // box is one thing to look at.
    at.bar = {at.list.x + at.list.width - 22.0f, at.list.y + 8.0f, 14.0f, at.list.height - 16.0f};

    at.preview = {std::floor(left + whole - whole * 0.36f), std::floor(top), std::floor(whole * 0.36f),
                  std::floor(high * 0.72f)};

    // Four across, under both, sharing the width the two of them cover.
    const float wide = std::floor((whole - 3.0f * kGap) / 4.0f);
    const float row  = std::floor(top + high + kGap * 2.0f);

    at.fresh = {left + 0.0f * (wide + kGap), row, wide, kButtonHigh};
    at.load  = {left + 1.0f * (wide + kGap), row, wide, kButtonHigh};
    at.edit  = {left + 2.0f * (wide + kGap), row, wide, kButtonHigh};
    at.drop  = {left + 3.0f * (wide + kGap), row, wide, kButtonHigh};

    return at;
}

struct NewAt {
    Rectangle group;
    Rectangle seed;
    Rectangle mode;
    Rectangle survival;
    Rectangle creative;
    Rectangle create;
};

NewAt LayNew() {
    NewAt at{};

    const float wide = std::min(Wide() * 0.66f, 700.0f);
    const float top  = High() * 0.26f;

    at.group = Middle(top, wide, High() * 0.36f);

    const float rowWide = std::floor(wide * 0.68f);
    const float rowHigh = 46.0f;

    at.seed = Middle(top + 46.0f, rowWide, rowHigh);
    at.mode = Middle(top + 46.0f + rowHigh + kGap, rowWide, rowHigh);

    // The list hangs *under* the row it belongs to and overlaps whatever is below,
    // which is what a dropped list does and why it is drawn last.
    at.survival = {at.mode.x, at.mode.y + rowHigh, at.mode.width, rowHigh};
    at.creative = {at.mode.x, at.mode.y + 2.0f * rowHigh, at.mode.width, rowHigh};

    at.create = Middle(at.group.y + at.group.height + kGap * 2.0f, kButtonWide, kButtonHigh);

    return at;
}

struct PartyAt {
    Rectangle list;
    Rectangle add;
    Rectangle join;
};

PartyAt LayParty() {
    PartyAt at{};

    const float wide = std::min(Wide() * 0.66f, 700.0f);
    const float top  = High() * 0.26f;

    at.list = Middle(top, wide, High() * 0.40f);

    const float half = std::floor((wide - kGap) / 2.0f);

    at.add  = {at.list.x, std::floor(at.list.y + at.list.height + kGap * 2.0f), half, kButtonHigh};
    at.join = {at.list.x + half + kGap, at.add.y, half, kButtonHigh};

    return at;
}

struct LoadingAt {
    Rectangle bar;
    Rectangle line;
    Rectangle why;
};

LoadingAt LayLoading() {
    LoadingAt at{};

    const float wide = std::min(Wide() * 0.66f, 700.0f);

    at.bar  = Middle(High() * 0.46f, wide, 26.0f);
    at.line = Middle(High() * 0.46f - 34.0f, wide, 24.0f);
    at.why  = Middle(High() * 0.46f + 46.0f, wide, 22.0f);

    return at;
}

// Where a screen says the thing it cannot do yet.
//
// Said out loud, in the middle of the box that would hold it, rather than left as
// an empty frame. A blank list is indistinguishable from a list that failed to
// load, and the player is owed the difference.
void Waiting(Rectangle box, const char *first, const char *second) {
    const Rectangle top = {box.x, box.y + box.height / 2.0f - 22.0f, box.width, 22.0f};
    const Rectangle low = {box.x, box.y + box.height / 2.0f + 2.0f, box.width, 22.0f};

    Label(first, top, 16, kFaint);
    Label(second, low, 16, kFaint);
}

// What the typed seed comes to.
//
// Empty is a world nobody chose, which is the commonest thing a player wants from
// this field and would otherwise be spelled by typing a number at random. A run of
// digits is that number. Anything else is hashed, which is Minecraft's rule and is
// what makes a seed a thing you can tell somebody over the table.
int Seeded(const std::string &text) {
    if (text.empty()) return GetRandomValue(1, 1000000);

    char *end        = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);

    if (end != nullptr && *end == '\0') return static_cast<int>(value);

    // FNV-1a, and the top bit taken off so the seed is the same on a machine that
    // reads an int as signed and one that does not.
    std::uint32_t hash = 2166136261u;

    for (const char letter : text) {
        hash ^= static_cast<unsigned char>(letter);
        hash *= 16777619u;
    }

    return static_cast<int>(hash & 0x7fffffffu);
}

} // namespace

void menu::Menu::Back() {
    if (stack_.size() > 1) stack_.pop_back();

    // Whatever was half done on the screen being left does not survive it. A field
    // still taking keystrokes after the player has walked away from it is how a
    // menu ends up eating the movement keys.
    typing_  = false;
    dropped_ = false;
}

void menu::Menu::Play() {
    stack_.clear();
    stack_.push_back(Screen::World);

    typing_  = false;
    dropped_ = false;
}

menu::Wish menu::Menu::Update() {
    Wish wish;

    // Escape means back on every screen it can mean anything on, and the arrow in
    // the corner means the same thing. Asked once, here, rather than per screen —
    // the one rule this module exists to make true.
    const bool back = (Pressed(BackAt()) && CanBack()) || (IsKeyPressed(KEY_ESCAPE) && CanBack() && !typing_);

    switch (Top()) {
    case Screen::Title: {
        const TitleAt at = LayTitle();

        if (Pressed(at.play)) Open(Screen::Saves);
        if (Pressed(at.party)) Open(Screen::Multiplayer);
        if (Pressed(at.options)) Open(Screen::Options);
        if (Pressed(at.quit)) wish.quit = true;

        if (back) Back();
        break;
    }

    case Screen::Saves: {
        const SavesAt at = LaySaves();

        if (Pressed(at.fresh)) Open(Screen::NewWorld);

        // Load, edit and delete need saves to act on, and there are none: the store
        // is a system of its own and is deliberately not being invented here. They
        // are drawn and refused rather than left out, because what they are for is
        // plain and leaving them out would make the screen look finished.

        if (back) Back();
        break;
    }

    case Screen::NewWorld: {
        const NewAt at = LayNew();

        // The field takes the keys while it is being typed into, and gives them
        // back the moment the player clicks elsewhere or presses return. Nothing
        // else on this screen reads a key, so there is nothing to fight over — but
        // the loop behind it does, which is why Escape is spoken for above only
        // when the field is quiet.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) typing_ = Over(at.seed);

        if (typing_) {
            for (int letter = GetCharPressed(); letter > 0; letter = GetCharPressed()) {
                if (seed_.size() >= 24) break;
                if (letter < 32 || letter > 126) continue;

                seed_ += static_cast<char>(letter);
            }

            if (IsKeyPressed(KEY_BACKSPACE) && !seed_.empty()) seed_.pop_back();
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) typing_ = false;
            if (IsKeyPressed(KEY_ESCAPE)) typing_ = false;
        }

        // The list, which is only ever two rows and could have been a click that
        // toggles. It is a list because the mock asks for one and because a third
        // mode is a row rather than a rethink.
        if (dropped_) {
            if (Pressed(at.survival)) {
                mode_    = Gamemode::Survival;
                dropped_ = false;
            } else if (Pressed(at.creative)) {
                mode_    = Gamemode::Creative;
                dropped_ = false;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !Over(at.mode)) {
                dropped_ = false;
            }
        }

        if (Pressed(at.mode)) dropped_ = !dropped_;

        if (Pressed(at.create)) {
            wish.create = true;
            wish.seed   = Seeded(seed_);
            wish.mode   = mode_;
        }

        if (back) Back();
        break;
    }

    case Screen::Multiplayer: {
        // Nothing here answers yet. The screen is on the stack so the button leads
        // somewhere honest rather than nowhere, and so the back arrow it is drawn
        // with is the same back arrow as everywhere else.
        if (back) Back();
        break;
    }

    case Screen::Loading: {
        // Nothing. The work is the loop's and the screen is a report on it — see
        // Screen::Loading.
        break;
    }

    case Screen::Options:
    case Screen::World: {
        if (back) Back();
        break;
    }
    }

    return wish;
}

void menu::Menu::Draw() const {
    ClearBackground(kGround);

    switch (Top()) {
    case Screen::Title: {
        const TitleAt at = LayTitle();

        Box(at.name, kPanel);
        Label(config::kGameName, at.name, kTitleType, kInk);

        Button(at.play, "play");
        Button(at.party, "multiplayer");
        Button(at.options, "options");
        Button(at.quit, "quit");

        // Only where there is a world under this to go back to — see the head of
        // menu.h. On a fresh start there is nothing behind the title and an arrow
        // pointing at nothing is a promise the screen cannot keep.
        DrawBack(CanBack());
        break;
    }

    case Screen::Saves: {
        const SavesAt at = LaySaves();

        DrawHead("SAVES");
        DrawBack(true);

        Box(at.list, kPanel);
        Waiting(at.list, "no saves yet - the store is not written", "start a new world below");

        // The scrollbar, drawn full and quiet. It is the shape of the screen rather
        // than a working control: there is nothing to scroll past until there is
        // something to scroll.
        DrawRectangleRec(at.bar, kSlot);
        DrawRectangleLinesEx(at.bar, 1.0f, kEdge);

        Box(at.preview, kPanel);
        Waiting(at.preview, "screenshot of the", "selected save");

        Button(at.fresh, "new");
        Button(at.load, "load", false);
        Button(at.edit, "edit", false);
        Button(at.drop, "delete", false);
        break;
    }

    case Screen::NewWorld: {
        const NewAt at = LayNew();

        DrawHead("NEW WORLD");
        DrawBack(true);

        Box(at.group, kPanel);

        // The seed row: the word on the left of the box and what has been typed on
        // the right of it, with a bar at the end while it is taking keys.
        const bool hot = typing_ || Over(at.seed);

        DrawRectangleRec(at.seed, typing_ ? kHot : kSlot);
        DrawRectangleLinesEx(at.seed, 2.0f, hot ? kInk : kEdge);

        DrawText("seed", static_cast<int>(at.seed.x + 14.0f),
                 static_cast<int>(at.seed.y + (at.seed.height - kBodyType) / 2.0f), kBodyType, kFaint);

        // The placeholder is set smaller than what is typed, and not only to fit: it
        // is a hint about the field rather than a value in it, and at the same size
        // it reads as one — a player who has not typed anything sees a seed they did
        // not choose.
        const bool empty = seed_.empty();

        const char *typed = empty ? "anything you like, or leave it empty" : seed_.c_str();
        const int size    = empty ? 16 : kBodyType;

        DrawText(typed, static_cast<int>(at.seed.x + 90.0f),
                 static_cast<int>(at.seed.y + (at.seed.height - static_cast<float>(size)) / 2.0f), size,
                 empty ? kFaint : kInk);

        if (typing_) {
            const float caret = at.seed.x + 90.0f + static_cast<float>(MeasureText(seed_.c_str(), kBodyType)) + 3.0f;

            DrawRectangleRec({caret, at.seed.y + 10.0f, 2.0f, at.seed.height - 20.0f}, kAccent);
        }

        // The gamemode row, which is the same shape wearing a mark on the right to
        // say it opens.
        const bool overMode = Over(at.mode);

        DrawRectangleRec(at.mode, dropped_ ? kHot : kSlot);
        DrawRectangleLinesEx(at.mode, 2.0f, (overMode || dropped_) ? kInk : kEdge);

        DrawText("gamemode", static_cast<int>(at.mode.x + 14.0f),
                 static_cast<int>(at.mode.y + (at.mode.height - kBodyType) / 2.0f), kBodyType, kFaint);

        DrawText(NameOf(mode_), static_cast<int>(at.mode.x + 150.0f),
                 static_cast<int>(at.mode.y + (at.mode.height - kBodyType) / 2.0f), kBodyType, kInk);

        // The little wedge that says a list is under it, pointing the way it will
        // move when it is pressed.
        const Vector2 wedge = {at.mode.x + at.mode.width - 24.0f, at.mode.y + at.mode.height / 2.0f};

        if (dropped_) {
            DrawTriangle({wedge.x - 7.0f, wedge.y + 4.0f}, {wedge.x + 7.0f, wedge.y + 4.0f},
                         {wedge.x, wedge.y - 5.0f}, kInk);
        } else {
            DrawTriangle({wedge.x - 7.0f, wedge.y - 4.0f}, {wedge.x, wedge.y + 5.0f},
                         {wedge.x + 7.0f, wedge.y - 4.0f}, kInk);
        }

        // What the two modes actually mean, under the group. The choice decides how
        // the whole world is played and the words on the row are not enough to make
        // it, so the screen says it rather than leaving the player to find out.
        const Rectangle says = {at.group.x, at.mode.y + at.mode.height + 18.0f, at.group.width, 22.0f};

        Label((mode_ == Gamemode::Creative) ? "blocks come away at a touch, and every material is yours"
                                            : "a block costs the work of breaking it, and you place what you dug",
              says, 16, kFaint);

        Button(at.create, "create");

        // Last, over everything, because a list that hangs open hangs over what is
        // under it.
        if (dropped_) {
            Button(at.survival, "survival");
            Button(at.creative, "creative");
        }
        break;
    }

    case Screen::Multiplayer: {
        const PartyAt at = LayParty();

        DrawHead("MULTIPLAYER");
        DrawBack(true);

        Box(at.list, kPanel);
        Waiting(at.list, "no servers - there is no networking yet", "this screen is the place it will go");

        Button(at.add, "add server", false);
        Button(at.join, "join", false);
        break;
    }

    case Screen::Options: {
        DrawHead("OPTIONS");
        DrawBack(true);

        const Rectangle box = Middle(High() * 0.28f, std::min(Wide() * 0.66f, 700.0f), High() * 0.36f);

        Box(box, kPanel);
        Waiting(box, "nothing to set yet", "the keys in the game are listed on screen while you play");
        break;
    }

    case Screen::Loading: {
        const LoadingAt at = LayLoading();

        DrawHead("CREATING WORLD");

        Label(made_, at.line, kBodyType, kInk);

        // The bar, and the share it has reached. Drawn as its own ground and a fill
        // inside it, so an empty bar is still a bar rather than a gap in the screen.
        DrawRectangleRec(at.bar, kPanel);

        const float share = std::clamp(share_, 0.0f, 1.0f);

        if (share > 0.0f) {
            DrawRectangleRec({at.bar.x + 3.0f, at.bar.y + 3.0f, (at.bar.width - 6.0f) * share, at.bar.height - 6.0f},
                             kAccent);
        }

        DrawRectangleLinesEx(at.bar, 2.0f, kEdge);

        // And why any of this takes a moment.
        //
        // Written out rather than left as a spinner because the honest answer is
        // interesting and short: the ore in this world is measured rather than
        // declared, so making one means sampling it. A player who knows that is
        // waiting for something; one who does not is watching a program hang.
        Label("ore is measured, not guessed: each seam is sampled across this world", at.why, 16, kFaint);

        Label("so that the rarity in the table is the rarity you will dig for",
              {at.why.x, at.why.y + 22.0f, at.why.width, at.why.height}, 16, kFaint);

        break;
    }

    case Screen::World: break;
    }
}

void menu::Menu::Working(const char *what, float share) {
    std::snprintf(made_, sizeof(made_), "%s", (what != nullptr) ? what : "");

    share_ = share;
}
