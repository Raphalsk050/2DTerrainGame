#include "ui/menu.h"

#include "core/config.h"
#include "save/record.h"

#include <algorithm>
#include <ctime>
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

// One layout for both, and the four buttons mean different things on each — see the
// draw. Two layouts would be two things to keep the same size, and the pause screen
// and the title screen looking alike is the point: it is the same screen, standing on
// a world instead of on nothing.
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

// How tall one row of the list is, and how much air is inside the box around them.
constexpr float kRowHigh = 58.0f;
constexpr float kListPad = 8.0f;

struct SavesAt {
    Rectangle list;
    Rectangle bar;
    Rectangle preview;

    // Under the picture: what the picked save is, in words the row has no room for.
    Rectangle facts;

    Rectangle fresh;
    Rectangle load;
    Rectangle edit;
    Rectangle drop;

    // How many rows the box has room for, and where one of them is.
    int rows = 0;

    Rectangle Row(int at) const {
        return {list.x + kListPad, list.y + kListPad + static_cast<float>(at) * kRowHigh,
                list.width - kListPad * 2.0f - 22.0f, kRowHigh - 4.0f};
    }
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

    // The picture box is the shape of the screen the picture was taken of, and that is
    // the whole of it: a preview is a screenshot, screenshots are the shape of the
    // window, and a box of some other shape shows one with a band of empty panel above
    // and below it. Fitted into a tall box it looked like a strip floating in a hole —
    // which is what it was.
    //
    // Taken from *this* window rather than from the picture, so the layout stays a pure
    // function of the frame and does not move when the selection does. A save written
    // at another shape is still fitted inside and still centred; it is simply the rare
    // case rather than every case.
    const float previewWide = std::floor(whole * 0.36f);

    at.preview = {std::floor(left + whole - previewWide), std::floor(top), previewWide,
                  std::floor(previewWide * High() / Wide())};

    // And the rest of the column is the facts, however much that turns out to be. It
    // was a fixed share of the height, which meant the two of them added up to the box
    // only at the one window shape they were written against.
    at.facts = {at.preview.x, std::floor(at.preview.y + at.preview.height + kGap), at.preview.width,
                std::floor(high - at.preview.height - kGap)};

    // Worked out from the box rather than fixed, so a tall window lists more saves
    // instead of listing the same six with a gap under them.
    at.rows = std::max(1, static_cast<int>((at.list.height - kListPad * 2.0f) / kRowHigh));

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
    Rectangle name;
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

    at.group = Middle(top, wide, High() * 0.36f + 46.0f + kGap);

    const float rowWide = std::floor(wide * 0.68f);
    const float rowHigh = 46.0f;

    // The name first, because it is the field a player fills in and the seed is the
    // one they mostly leave alone. A screen whose first row is the optional one reads
    // as a screen about seeds.
    at.name = Middle(top + 46.0f, rowWide, rowHigh);
    at.seed = Middle(top + 46.0f + 1.0f * (rowHigh + kGap), rowWide, rowHigh);
    at.mode = Middle(top + 46.0f + 2.0f * (rowHigh + kGap), rowWide, rowHigh);

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

// One row of the form: a word on the left, what has been typed on the right, and a
// caret at the end of it while it is taking keys.
//
// Written once and called three times — the name of a new world, its seed, and the
// renaming of an old one. The seed's row was here first and the other two were about
// to be copies of it, which is how three fields end up with three different amounts of
// padding and only one of them lining up with the box it is in.
void FieldRow(Rectangle at, const char *label, const std::string &text, const char *hint, bool taking) {
    const bool hot = taking || Over(at);

    DrawRectangleRec(at, taking ? kHot : kSlot);
    DrawRectangleLinesEx(at, 2.0f, hot ? kInk : kEdge);

    DrawText(label, static_cast<int>(at.x + 14.0f), static_cast<int>(at.y + (at.height - kBodyType) / 2.0f),
             kBodyType, kFaint);

    // The placeholder is set smaller than what is typed, and not only to fit: it is a
    // hint about the field rather than a value in it, and at the same size it reads as
    // one — a player who has not typed anything sees a name they did not choose.
    const bool empty = text.empty();

    const char *shown = empty ? hint : text.c_str();
    const int size    = empty ? 16 : kBodyType;

    const float from = at.x + 150.0f;

    DrawText(shown, static_cast<int>(from), static_cast<int>(at.y + (at.height - static_cast<float>(size)) / 2.0f),
             size, empty ? kFaint : kInk);

    if (!taking) return;

    const float caret = from + static_cast<float>(MeasureText(text.c_str(), kBodyType)) + 3.0f;

    DrawRectangleRec({caret, at.y + 10.0f, 2.0f, at.height - 20.0f}, kAccent);
}

// Letters into a field, and the two keys that let go of it.
//
// Returns whether the field still has the keyboard, so the caller's own state is set
// from the answer rather than in three places inside.
bool TakeKeys(std::string &into, std::size_t most) {
    for (int letter = GetCharPressed(); letter > 0; letter = GetCharPressed()) {
        if (into.size() >= most) break;
        if (letter < 32 || letter > 126) continue;

        into += static_cast<char>(letter);
    }

    if (IsKeyPressed(KEY_BACKSPACE) && !into.empty()) into.pop_back();

    return !IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_KP_ENTER) && !IsKeyPressed(KEY_ESCAPE);
}

// How long ago, in the coarsest unit that is still true.
//
// "3 days ago" and never "3 days, 4 hours and 12 minutes ago": what the row is for is
// telling one save from another at a glance, and a precise answer is a longer string
// that answers the same question.
const char *Since(std::int64_t written) {
    if (written <= 0) return "never played";

    const std::int64_t now  = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t gone = now - written;

    // Singular where it is one, because "1 hours ago" is the sort of thing a player
    // reads once and stops trusting the rest of the screen over.
    const auto many = [](std::int64_t count, const char *unit) {
        return TextFormat("%lld %s%s ago", count, unit, (count == 1) ? "" : "s");
    };

    if (gone < 90) return "just now";
    if (gone < 3600) return many(gone / 60, "minute");
    if (gone < 86400) return many(gone / 3600, "hour");

    return many(gone / 86400, "day");
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

void menu::Menu::Open(Screen screen) {
    // Re-read on the way in rather than watched. A folder that changes while the menu
    // is up is not a case worth carrying machinery for, and every path that can change
    // it goes through here or through `Refresh` directly.
    if (screen == Screen::Saves) Refresh();

    stack_.push_back(screen);
}

Rectangle menu::Menu::Preview() const {
    return LaySaves().preview;
}

void menu::Menu::Refresh() {
    saves_ = save::List();

    asking_.clear();

    if (field_ == Field::Rename) Typing(Field::None);

    // The one that was picked, found again by id. By id and never by position: a save
    // that was deleted moves every row under it up one, and a remembered position would
    // quietly select whatever slid into it — which is the row the next click deletes.
    int found = -1;

    for (std::size_t i = 0; i < saves_.size(); i++) {
        if (saves_[i].id == shotOf_) found = static_cast<int>(i);
    }

    // Falling back to the first, which is the newest — and to nothing where there are
    // no saves at all.
    if (found < 0) found = saves_.empty() ? -1 : 0;

    picked_ = -1;

    Select(found);
}

void menu::Menu::Typing(Field field) {
    field_ = field;
}

int menu::Menu::RowAt(Vector2 where) const {
    const SavesAt at = LaySaves();

    if (!CheckCollisionPointRec(where, at.list)) return -1;

    for (int row = 0; row < at.rows; row++) {
        const int which = scroll_ + row;

        if (which >= static_cast<int>(saves_.size())) break;
        if (CheckCollisionPointRec(where, at.Row(row))) return which;
    }

    return -1;
}

void menu::Menu::Select(int row) {
    if (row == picked_) return;

    picked_ = row;

    Show(row);
}

void menu::Menu::Show(int row) {
    const bool valid = row >= 0 && row < static_cast<int>(saves_.size());

    const std::string wants = valid ? saves_[static_cast<std::size_t>(row)].id : std::string();

    if (wants == shotOf_ && shot_.id != 0) return;

    Unload();

    shotOf_ = wants;

    if (!valid || !saves_[static_cast<std::size_t>(row)].shot) return;

    shot_ = LoadTexture(saves_[static_cast<std::size_t>(row)].ShotPath().c_str());

    // Point sampling, like everything else drawn from a file in this project: the
    // preview is a picture of a pixel-art world, and bilinear turns it into a smear
    // at every scale but one.
    if (shot_.id != 0) SetTextureFilter(shot_, TEXTURE_FILTER_POINT);
}

void menu::Menu::Unload() {
    if (shot_.id != 0) UnloadTexture(shot_);

    shot_ = {};
}

void menu::Menu::Home() {
    stack_.clear();
    stack_.push_back(Screen::Title);

    playing_ = {};

    Typing(Field::None);

    dropped_ = false;
}

void menu::Menu::Back() {
    if (stack_.size() > 1) stack_.pop_back();

    // Whatever was half done on the screen being left does not survive it. A field
    // still taking keystrokes after the player has walked away from it is how a
    // menu ends up eating the movement keys.
    Typing(Field::None);

    asking_.clear();

    dropped_ = false;
}

void menu::Menu::Play() {
    stack_.clear();
    stack_.push_back(Screen::World);

    Typing(Field::None);

    dropped_ = false;
}

menu::Wish menu::Menu::Update() {
    Wish wish;

    // Escape means back on every screen it can mean anything on, and the arrow in
    // the corner means the same thing. Asked once, here, rather than per screen —
    // the one rule this module exists to make true.
    const bool back =
        (Pressed(BackAt()) && CanBack()) || (IsKeyPressed(KEY_ESCAPE) && CanBack() && field_ == Field::None);

    switch (Top()) {
    case Screen::Title: {
        const TitleAt at = LayTitle();

        // Paused, the same four buttons mean four different things.
        //
        // One screen and not two, because it *is* one screen: the title standing on a
        // world rather than on nothing, which is exactly what the stack already says
        // (see `Paused`). Two screens would be two layouts to keep the same size and a
        // second place to add a button to.
        if (Paused()) {
            if (Pressed(at.play)) Back();

            if (Pressed(at.party)) wish.save = true;

            if (Pressed(at.options)) Open(Screen::Options);

            // Saved *and* left, in that order and reported together, so the loop
            // writes the world before the menu forgets which save it was — see
            // Wish::leaving.
            if (Pressed(at.quit)) {
                wish.save    = true;
                wish.leaving = true;
            }

            if (back) Back();
            break;
        }

        if (Pressed(at.play)) Open(Screen::Saves);
        if (Pressed(at.party)) Open(Screen::Multiplayer);
        if (Pressed(at.options)) Open(Screen::Options);
        if (Pressed(at.quit)) wish.quit = true;

        if (back) Back();
        break;
    }

    case Screen::Saves: {
        const SavesAt at = LaySaves();

        const bool any    = picked_ >= 0 && picked_ < static_cast<int>(saves_.size());
        const save::Slot &one = any ? saves_[static_cast<std::size_t>(picked_)] : playing_;

        // The question about deleting, asked over the screen rather than on one of its
        // own. It answers first and swallows everything under it, because a click that
        // reached the list while a confirm is up would move the selection out from
        // under the very question being asked.
        if (!asking_.empty()) {
            const Rectangle box  = Middle(High() * 0.42f, std::min(Wide() * 0.6f, 620.0f), 150.0f);
            const float half     = std::floor((box.width - kGap * 3.0f) / 2.0f);
            const Rectangle sure = {box.x + kGap, box.y + box.height - kButtonHigh - kGap, half, kButtonHigh};
            const Rectangle keep = {sure.x + half + kGap, sure.y, half, kButtonHigh};

            if (Pressed(sure)) {
                save::Erase(asking_);

                asking_.clear();
                shotOf_.clear();

                Refresh();
            }

            if (Pressed(keep) || back) asking_.clear();

            break;
        }

        // Renaming happens in the row itself rather than on a screen of its own. What
        // is being renamed is the row the player is looking at, and moving them
        // somewhere else to type would take it off the screen.
        if (field_ == Field::Rename) {
            if (!TakeKeys(rename_, 32)) {
                if (any && !rename_.empty()) save::Rename(one.id, rename_);

                Typing(Field::None);
                Refresh();
            }

            // A click anywhere else finishes it too, which is what a field in a list
            // does everywhere else and is the only way out that needs no key.
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !Over(at.Row(picked_ - scroll_))) {
                if (any && !rename_.empty()) save::Rename(one.id, rename_);

                Typing(Field::None);
                Refresh();
            }

            break;
        }

        // The wheel over the list, bounded by what there is. Bounded rather than
        // wrapped: a list that scrolls past its end and comes back round is a list
        // whose position tells the player nothing.
        if (CheckCollisionPointRec(GetMousePosition(), at.list)) {
            const int most = std::max(0, static_cast<int>(saves_.size()) - at.rows);

            scroll_ = std::clamp(scroll_ - static_cast<int>(GetMouseWheelMove()), 0, most);
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const int row = RowAt(GetMousePosition());

            if (row >= 0) Select(row);
        }

        if (Pressed(at.fresh)) Open(Screen::NewWorld);

        if (Pressed(at.load, any)) {
            wish.load = true;
            wish.slot = one.id;
        }

        if (Pressed(at.edit, any)) {
            rename_ = one.name;

            Typing(Field::Rename);
        }

        if (Pressed(at.drop, any)) asking_ = one.id;

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
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Typing(Over(at.name) ? Field::Name : (Over(at.seed) ? Field::Seed : Field::None));
        }

        if (field_ == Field::Name && !TakeKeys(name_, 32)) Typing(Field::None);
        if (field_ == Field::Seed && !TakeKeys(seed_, 24)) Typing(Field::None);

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

            // A world nobody named is called after its seed, which is the one thing
            // about it that is already true and is what a player would have typed. An
            // empty name would list as an empty row.
            wish.name = name_.empty() ? std::string("world ") + std::to_string(wish.seed) : name_;

            // The form is left empty behind them, because the next world made is a
            // different world: a name still sitting in the box is a save called after
            // the last one, which is exactly the pair of worlds a player cannot tell
            // apart afterwards.
            name_.clear();
            seed_.clear();
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
        Label(Paused() ? playing_.name.c_str() : config::kGameName, at.name, kTitleType, kInk);

        if (Paused()) {
            Button(at.play, "back to game");
            Button(at.party, "save");
            Button(at.options, "options");
            Button(at.quit, "save and quit");

            // What saving will write to, under the buttons. A player with three worlds
            // on the go is owed the name of the one they are about to overwrite, and the
            // row above says it in the size a title is rather than the size an answer is.
            const Rectangle says = {at.quit.x, at.quit.y + at.quit.height + kGap, at.quit.width, 22.0f};

            Label(playing_.id.empty() ? "this world has nowhere to be saved" : "saving keeps everything you built",
                  says, 16, kFaint);

            DrawBack(CanBack());
            break;
        }

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

        if (saves_.empty()) Waiting(at.list, "no worlds yet", "start a new one below");

        for (int row = 0; row < at.rows; row++) {
            const int which = scroll_ + row;

            if (which >= static_cast<int>(saves_.size())) break;

            const save::Slot &slot = saves_[static_cast<std::size_t>(which)];

            const Rectangle box = at.Row(row);

            const bool picked = which == picked_;
            const bool hot    = Over(box);

            DrawRectangleRec(box, picked ? kHot : (hot ? kSlot : kPanel));
            DrawRectangleLinesEx(box, 2.0f, picked ? kInk : kEdge);

            // The name on top and everything else under it, smaller and quieter. What
            // the eye is doing here is finding one name in a column of them, and a row
            // where every word carries the same weight is a row that has to be read.
            const bool renaming = picked && field_ == Field::Rename;

            const std::string &shown = renaming ? rename_ : slot.name;

            DrawText(shown.c_str(), static_cast<int>(box.x + 12.0f), static_cast<int>(box.y + 8.0f), kBodyType, kInk);

            if (renaming) {
                const float caret = box.x + 12.0f + static_cast<float>(MeasureText(rename_.c_str(), kBodyType)) + 3.0f;

                DrawRectangleRec({caret, box.y + 8.0f, 2.0f, static_cast<float>(kBodyType)}, kAccent);
            }

            DrawText(TextFormat("%s  -  seed %d  -  %s", slot.creative ? "creative" : "survival", slot.seed,
                                Since(slot.written)),
                     static_cast<int>(box.x + 12.0f), static_cast<int>(box.y + 8.0f + kBodyType + 4.0f), 15, kFaint);
        }

        // The scrollbar: its ground, and a thumb as long a share of it as the rows on
        // screen are of the rows there are. A bar that is always full says nothing, and
        // one that is always the same length lies about how much is under it.
        DrawRectangleRec(at.bar, kPanel);
        DrawRectangleLinesEx(at.bar, 1.0f, kEdge);

        {
            const int total = std::max(1, static_cast<int>(saves_.size()));
            const float has = std::min(1.0f, static_cast<float>(at.rows) / static_cast<float>(total));

            const float span = std::max(18.0f, at.bar.height * has);
            const int most   = std::max(1, total - at.rows);

            const float down = (at.bar.height - span) * (static_cast<float>(scroll_) / static_cast<float>(most));

            DrawRectangleRec({at.bar.x + 2.0f, at.bar.y + down + 2.0f, at.bar.width - 4.0f, span - 4.0f}, kSlot);
        }

        Box(at.preview, kPanel);

        if (shot_.id != 0) {
            // Fitted inside the box and centred, keeping its shape. A preview stretched
            // to fill the panel is a picture of a world nobody is looking at.
            const float scale = std::min(at.preview.width / static_cast<float>(shot_.width),
                                         at.preview.height / static_cast<float>(shot_.height));

            const float wide = static_cast<float>(shot_.width) * scale;
            const float high = static_cast<float>(shot_.height) * scale;

            DrawTexturePro(shot_, {0.0f, 0.0f, static_cast<float>(shot_.width), static_cast<float>(shot_.height)},
                           {std::floor(at.preview.x + (at.preview.width - wide) / 2.0f),
                            std::floor(at.preview.y + (at.preview.height - high) / 2.0f), wide, high},
                           {0.0f, 0.0f}, 0.0f, WHITE);
        } else if (picked_ >= 0) {
            Waiting(at.preview, "no picture of this one", "it was written before there was one");
        } else {
            Waiting(at.preview, "pick a world", "and it is shown here");
        }

        Box(at.facts, kPanel);

        if (picked_ >= 0 && picked_ < static_cast<int>(saves_.size())) {
            const save::Slot &slot = saves_[static_cast<std::size_t>(picked_)];

            // The clock as an hour of the day. A world is left at a time and comes back
            // at it, and the number of seconds it has been running is not a thing
            // anybody wants to read.
            const int hour   = static_cast<int>(slot.clock / 3600.0f) % 24;
            const int minute = static_cast<int>(slot.clock / 60.0f) % 60;

            DrawText(TextFormat("world  %d", slot.seed), static_cast<int>(at.facts.x + 12.0f),
                     static_cast<int>(at.facts.y + 10.0f), 16, kFaint);

            DrawText(TextFormat("left at  %02d:%02d", hour, minute), static_cast<int>(at.facts.x + 12.0f),
                     static_cast<int>(at.facts.y + 32.0f), 16, kFaint);

            DrawText(slot.creative ? "creative" : "survival", static_cast<int>(at.facts.x + 12.0f),
                     static_cast<int>(at.facts.y + 54.0f), 16, kAccent);
        } else {
            Waiting(at.facts, "nothing picked", "");
        }

        const bool any = picked_ >= 0 && picked_ < static_cast<int>(saves_.size());

        Button(at.fresh, "new");
        Button(at.load, "load", any);
        Button(at.edit, "rename", any);
        Button(at.drop, "delete", any);

        // Last of all, over everything, because a question about the screen has to be
        // in front of the screen it is about.
        if (asking_.empty()) break;

        DrawRectangleRec({0.0f, 0.0f, Wide(), High()}, {0, 0, 0, 170});

        const Rectangle box = Middle(High() * 0.42f, std::min(Wide() * 0.6f, 620.0f), 150.0f);

        Box(box, kPanel);

        Label("delete this world?", {box.x, box.y + 18.0f, box.width, 26.0f}, kHeadType, kInk);
        Label("it cannot be got back", {box.x, box.y + 52.0f, box.width, 22.0f}, 16, kFaint);

        const float half = std::floor((box.width - kGap * 3.0f) / 2.0f);

        Button({box.x + kGap, box.y + box.height - kButtonHigh - kGap, half, kButtonHigh}, "delete");
        Button({box.x + kGap + half + kGap, box.y + box.height - kButtonHigh - kGap, half, kButtonHigh}, "keep it");
        break;
    }

    case Screen::NewWorld: {
        const NewAt at = LayNew();

        DrawHead("NEW WORLD");
        DrawBack(true);

        Box(at.group, kPanel);

        FieldRow(at.name, "name", name_, "what to call it", field_ == Field::Name);
        FieldRow(at.seed, "seed", seed_, "anything, or leave it empty", field_ == Field::Seed);

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
