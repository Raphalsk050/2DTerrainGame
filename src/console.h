#pragma once

#include "raylib.h"

#include <array>
#include <string>

// The chat line, and the log above it.
//
// Two jobs that look like one and are worth keeping apart. This module is the
// *panel*: it knows how to take a line of typing, how to remember what was typed
// before, and how to show a scrollback that fades when nobody is looking at it. It
// does not know what any command means, and deliberately — what `/weather storm`
// does is a fact about the world, and a console that reached into the world would
// have to be handed the whole game to be compiled at all.
//
// So it hands the submitted line back to the caller and takes an answer. That is
// also what makes the answers uniform: every command reports through the same
// channel, so there is no command that quietly works and none that fails in
// silence.
namespace console {

// How a line is to be read. Only the colour changes, but the colour is the whole
// of the feedback — a player who typed a command needs to know at a glance whether
// the world took it.
enum class Tone { Said, Done, Failed, Note };

// Lines kept in the scrollback.
//
// Deep enough that a `/help` does not push away what came before it, shallow enough
// that the whole log is a fixed cost. Nothing here is worth growing without bound.
inline constexpr int kBacklog = 64;

// Entries kept for the up-arrow to walk back through.
inline constexpr int kRecalled = 24;

// How long a line stays on screen after the panel is closed, in seconds.
//
// Chat that vanishes with the panel is chat nobody reads: the answer to a command
// arrives at the moment the player is closing the box. Chat that never vanishes is
// a permanent bar across the world. Both are solved by this — long enough to read
// twice, short enough that a minute later the screen is the game again.
inline constexpr float kLinger = 9.0f;

class Console {
public:
    bool IsOpen() const { return open_; }

    // Opens the panel and swallows whatever key opened it.
    //
    // The swallow is not a nicety: raylib queues typed characters, so the `t` that
    // asked for the box is still in the queue when the box appears and lands in it
    // as the first character of every message.
    void Open();

    // Closes it and throws away whatever was half-typed. A line abandoned with
    // escape is a line the player decided against.
    void Close();

    // Reads the keyboard for one frame. Returns the line just submitted, or an
    // empty string where nothing was.
    //
    // Only call this while open; it is what consumes the typing, and anything else
    // reading keys in the same frame would act on what is being written.
    std::string Read();

    // Puts a line in the log.
    void Say(const std::string &text, Tone tone = Tone::Said);

    // Empties it. A log is a thing that fills up, so there has to be a way to see
    // the world again without waiting the lines out.
    void Wipe();

    // The panel, or the fading tail of it. Drawn in screen space, over everything.
    void Draw(float seconds) const;

    // Moves the clock the fade is measured on. Real seconds and not the weather's:
    // how long a line has been readable is a fact about the person reading it, and
    // running it forty times faster under F7 would blink the answer away.
    void Step(float dt) { clock_ += dt; }

private:
    struct Line {
        std::string text;
        Tone tone = Tone::Said;
        float at  = 0.0f;
    };

    std::array<Line, kBacklog> log_{};

    int lines_ = 0;
    int next_  = 0;

    std::array<std::string, kRecalled> recalled_{};

    int kept_ = 0;

    // How far back through the recalled lines the player has walked, or -1 while
    // they are still on the line they are writing.
    int walking_ = -1;

    std::string typing_;

    // What was being typed before the up-arrow was first pressed, so that walking
    // all the way back down returns it rather than an empty box.
    std::string held_;

    bool open_ = false;

    float clock_ = 0.0f;

    // How long backspace has been down, for the repeat. A key that deletes one
    // character per press makes correcting a typo a drum solo.
    float erasing_ = 0.0f;
};

} // namespace console
