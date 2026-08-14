#include "console.h"

#include <algorithm>
#include <cmath>

namespace console {
namespace {

// Longest line the box will take. Past this the keystroke is simply dropped, which
// is the honest answer — a box that scrolled sideways would hide the start of what
// was being written.
constexpr std::size_t kMostTyped = 96;

// Where the panel sits and how it is laid out, in screen pixels.
constexpr int kMargin  = 10;
constexpr int kLeading = 16;
constexpr int kType    = 13;
constexpr int kPadding = 5;

// How much of the screen height the log may take before older lines are dropped
// off the top. A chat that fills the window is a chat that has stopped being an
// overlay.
constexpr float kMostHeight = 0.45f;

// Seconds before a held backspace starts repeating, and seconds between repeats
// once it does.
constexpr float kEraseDelay  = 0.38f;
constexpr float kEraseRepeat = 0.045f;

Color Ink(Tone tone) {
    switch (tone) {
    case Tone::Done: return {150, 226, 150, 255};
    case Tone::Failed: return {245, 132, 122, 255};
    case Tone::Note: return {196, 200, 214, 255};
    default: return {236, 240, 248, 255};
    }
}

} // namespace

void Console::Open() {
    open_    = true;
    walking_ = -1;
    typing_.clear();
    held_.clear();
    erasing_ = 0.0f;

    // Whatever is already in raylib's queue is thrown away, which is what stops the
    // key that asked for the box from being the first character in it.
    while (GetCharPressed() != 0) {
    }
}

void Console::Close() {
    open_    = false;
    walking_ = -1;
    typing_.clear();
    held_.clear();
}

std::string Console::Read() {
    if (!open_) return {};

    // Printable characters, however many arrived this frame. Reading the queue dry
    // rather than one a frame is what keeps fast typing from being dropped.
    for (int glyph = GetCharPressed(); glyph != 0; glyph = GetCharPressed()) {
        if (glyph < 32 || glyph > 126) continue;
        if (typing_.size() >= kMostTyped) continue;

        typing_.push_back(static_cast<char>(glyph));

        // Typing leaves the recalled line and makes it the player's own.
        walking_ = -1;
    }

    if (IsKeyDown(KEY_BACKSPACE)) {
        const bool first = IsKeyPressed(KEY_BACKSPACE);

        if (first) erasing_ = 0.0f;

        erasing_ += GetFrameTime();

        // One on the press, and then a run once the key has been held past the
        // delay. Without the repeat, clearing a mistyped command is a drum solo.
        const bool again = erasing_ > kEraseDelay && std::fmod(erasing_ - kEraseDelay, kEraseRepeat) < GetFrameTime();

        if ((first || again) && !typing_.empty()) typing_.pop_back();
    } else {
        erasing_ = 0.0f;
    }

    // Walking back through what was sent before. Minecraft's arrows, and worth
    // having for the same reason: a command that failed is nearly always a command
    // that needs one character changed.
    if (kept_ > 0 && IsKeyPressed(KEY_UP)) {
        // The line being written is put aside on the first step back, so walking
        // all the way down again returns it rather than an empty box.
        if (walking_ < 0) held_ = typing_;

        walking_ = std::min(walking_ + 1, kept_ - 1);
        typing_  = recalled_[static_cast<std::size_t>(walking_)];
    }

    if (IsKeyPressed(KEY_DOWN) && walking_ >= 0) {
        walking_--;

        typing_ = (walking_ < 0) ? held_ : recalled_[static_cast<std::size_t>(walking_)];
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        Close();
        return {};
    }

    if (!IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_KP_ENTER)) return {};

    std::string sent = typing_;

    typing_.clear();
    walking_ = -1;
    held_.clear();

    // Enter on an empty box closes it, which is the shortest way out and what every
    // chat box does.
    if (sent.empty()) {
        Close();
        return {};
    }

    // Kept for the arrows, newest first, and never twice in a row: repeating a
    // command should not fill the recall with copies of it.
    if (kept_ == 0 || recalled_[0] != sent) {
        for (int i = std::min(kept_, kRecalled - 1); i > 0; i--) {
            recalled_[static_cast<std::size_t>(i)] = recalled_[static_cast<std::size_t>(i - 1)];
        }

        recalled_[0] = sent;
        kept_        = std::min(kept_ + 1, kRecalled);
    }

    Close();

    return sent;
}

void Console::Say(const std::string &text, Tone tone) {
    log_[static_cast<std::size_t>(next_)] = {text, tone, clock_};

    next_  = (next_ + 1) % kBacklog;
    lines_ = std::min(lines_ + 1, kBacklog);
}

void Console::Wipe() {
    log_   = {};
    lines_ = 0;
    next_  = 0;
}

void Console::Draw(float seconds) const {
    (void)seconds;

    const int width  = GetScreenWidth();
    const int height = GetScreenHeight();

    // How many lines there is room for, and never more than have been said.
    const int room = std::max(static_cast<int>(static_cast<float>(height) * kMostHeight) / kLeading, 1);

    const int shown = std::min(lines_, room);

    // The bottom of the log sits just above the box while it is open, and where the
    // box would have been while it is closed — so lines do not jump up the screen at
    // the moment the panel is dismissed.
    const int floorY = height - kMargin - kLeading - kPadding * 2;

    int drawn = 0;

    for (int i = 1; i <= shown; i++) {
        const Line &line = log_[static_cast<std::size_t>((next_ - i + kBacklog * 2) % kBacklog)];

        if (line.text.empty()) continue;

        // Closed, only what is recent is worth the screen. Faded over the last
        // second of that rather than switched off, or the log flickers away a line
        // at a time.
        float alpha = 1.0f;

        if (!open_) {
            const float age = clock_ - line.at;

            if (age > kLinger) continue;

            alpha = std::clamp(kLinger - age, 0.0f, 1.0f);
        }

        const int y = floorY - (drawn + 1) * kLeading;

        if (y < kMargin) break;

        const int span = MeasureText(line.text.c_str(), kType);

        DrawRectangle(kMargin, y - 2, span + kPadding * 2, kLeading, Fade({12, 14, 20, 190}, alpha));
        DrawText(line.text.c_str(), kMargin + kPadding, y, kType, Fade(Ink(line.tone), alpha));

        drawn++;
    }

    if (!open_) return;

    // The box itself, the full width of the window: a line being typed has no
    // length yet, and a box that grew with the text would move under the caret.
    const int boxY = height - kMargin - kLeading - kPadding * 2;
    const int boxH = kLeading + kPadding * 2;

    DrawRectangle(kMargin, boxY, width - kMargin * 2, boxH, {12, 14, 20, 225});
    DrawRectangleLines(kMargin, boxY, width - kMargin * 2, boxH, {120, 132, 158, 255});

    const std::string shownLine = "> " + typing_;

    DrawText(shownLine.c_str(), kMargin + kPadding, boxY + kPadding, kType, {236, 240, 248, 255});

    // A caret that blinks on the wall clock, so it keeps its rhythm whatever the
    // weather is doing.
    if (std::fmod(clock_, 1.0f) < 0.5f) {
        const int caret = MeasureText(shownLine.c_str(), kType);

        DrawRectangle(kMargin + kPadding + caret + 1, boxY + kPadding, 2, kType, {236, 240, 248, 255});
    }
}

} // namespace console
