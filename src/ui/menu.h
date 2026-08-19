#pragma once

#include "core/mode.h"
#include "raylib.h"

#include <string>
#include <vector>

// The screens in front of the game, and the way back out of them.
//
// The whole of this module is one idea: **a stack**. A screen is pushed when the
// player goes into it and popped when they come back, so "back" is one operation
// that works from anywhere and needs to know nothing about where it is. The
// alternative — each screen remembering which one to return to — is the same
// answer written down once per screen, and it is wrong the first time two paths
// reach the same screen. New world is reached from saves today and will be
// reached from a world list tomorrow; neither of them has to say so.
//
// The world is a screen too, and that is what makes pausing fall out for free.
// Playing is a stack of exactly one, `{World}`; escape pushes the title onto it,
// `{World, Title}`; back pops it and the game is running again. A fresh start is
// `{Title}` with nothing under it, which is the same stack minus a world — so the
// title knows whether to offer a way back by asking how deep it is standing, and
// nothing has to be told whether a game is in progress.
namespace menu {

enum class Screen {
    Title,
    Saves,
    NewWorld,
    Multiplayer,
    Options,

    // The world being made, and the one screen nothing can be done on. It has no
    // back arrow and answers no key: there is no half-made world to go back to,
    // and a cancel would have to unwind a measurement that is already half
    // written into the world it is measuring.
    Loading,

    // Not drawn by anything here. It is on the stack so that the loop can ask one
    // question — what is on top — instead of keeping a second flag beside it that
    // says whether a menu is up.
    World,
};

// What one frame of the menu asks the loop to do.
//
// The menu cannot do any of it itself: making a world means the world, the wood,
// the character and the camera, all of which belong to the loop and none of which
// a screen should be able to reach. So it reports and the loop acts — the same
// arrangement Inventory::Gesture uses, and for the same reason.
struct Wish {
    bool quit = false;

    // Make a world of this seed, in this mode, and start playing it.
    bool create = false;

    int seed      = 0;
    Gamemode mode = Gamemode::Survival;
};

class Menu {
public:
    // Reads the mouse and the keys over whichever screen is on top, and moves the
    // stack. Called only while a menu is up; the loop's own input is what runs
    // while the world is.
    Wish Update();

    // Draws that screen. Inside the frame, and it clears it: a menu is the whole
    // of what is on screen while it is up.
    //
    // Every screen is laid out by a pure function of the window size, called again
    // here rather than remembered from Update. Two answers to one layout is a
    // button that is drawn where it cannot be clicked — the same rule the editor's
    // cursor is written to, one frame further out.
    void Draw() const;

    void Open(Screen screen) { stack_.push_back(screen); }

    // What the loading screen says and how far along its bar is.
    //
    // Pushed in by the loop rather than read out by the screen, because what is
    // being waited for is the loop's business: the menu is not allowed to know
    // that a world has a wood in it, and the world is not allowed to know that
    // anything is watching.
    void Working(const char *what, float share);

    // Down one, unless there is nothing under it. The title with nothing beneath
    // it is where the game starts, and a stack that can empty itself is a black
    // screen with no way out.
    void Back();

    // The world, and nothing over it. What a created world starts from and what
    // back-from-pause comes down to, so the two cannot end up meaning different
    // stacks.
    void Play();

    Screen Top() const { return stack_.empty() ? Screen::Title : stack_.back(); }

    bool Playing() const { return Top() == Screen::World; }

    // Whether a world is standing under the menu — a game paused rather than a
    // game not yet started.
    bool Paused() const { return !Playing() && !stack_.empty() && stack_.front() == Screen::World; }

private:
    bool CanBack() const { return stack_.size() > 1; }

    // The seed as it is typed, rather than as a number.
    //
    // Kept as text because that is what the player is editing: a field that stored
    // an int could not hold "-" on the way to "-12", could not hold the empty
    // string that means "surprise me", and could not take a word. Minecraft takes
    // a word and hashes it, which is what Seeded does.
    std::string seed_;
    bool typing_ = false;

    Gamemode mode_ = Gamemode::Survival;

    // Whether the gamemode list is hanging open under its row.
    bool dropped_ = false;

    // What the loading screen is showing. A buffer rather than a pointer, because
    // the line is composed by whoever is doing the work and must not outlive the
    // frame they composed it in.
    char made_[96] = "";
    float share_   = 0.0f;

    std::vector<Screen> stack_{Screen::Title};
};

} // namespace menu
