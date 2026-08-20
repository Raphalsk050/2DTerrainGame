#pragma once

#include "core/mode.h"
#include "save/record.h"
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
//
// The same goes for saving, and there it is not merely tidiness: a save is a
// picture of six subsystems taken at one instant, and a screen that could reach
// them could take that picture halfway through a frame.
struct Wish {
    bool quit = false;

    // Make a world of this seed, in this mode, and start playing it.
    bool create = false;

    // Read `slot` back and start playing it.
    bool load = false;

    // Write the world being played into the save it came from.
    bool save = false;

    // Whether the player is on their way out of the world after it — see the pause
    // screen. Reported beside `save` rather than instead of it, so the loop writes
    // once and the menu decides what happens next.
    bool leaving = false;

    int seed      = 0;
    Gamemode mode = Gamemode::Survival;

    // What a new world is to be called.
    std::string name;

    // Which save to read, for `load`.
    std::string slot;
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

    void Open(Screen screen);

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

    // The title with nothing under it — the stack a game that has been left comes
    // down to. Distinct from `Back` because leaving a world drops it: there is no
    // longer a world under the title to return to, and an arrow that offered one
    // would be pointing at a world the loop has already replaced.
    void Home();

    Screen Top() const { return stack_.empty() ? Screen::Title : stack_.back(); }

    bool Playing() const { return Top() == Screen::World; }

    // Whether a world is standing under the menu — a game paused rather than a
    // game not yet started.
    bool Paused() const { return !Playing() && !stack_.empty() && stack_.front() == Screen::World; }

    // Re-reads the folder of saves. Called when the screen is opened and after
    // anything that changes what is in it.
    void Refresh();

    // Gives the preview texture back.
    //
    // Called on the way out beside `Grove::Unload`, and for its reason: a texture
    // that outlives the window it was made in is a crash on exit that only ever
    // happens on somebody else's machine.
    void Unload();

    // Which save is being played, so that "save" knows where to write and the
    // pause screen can say what it is about to write to.
    void Playing(const save::Slot &slot) { playing_ = slot; }

    const save::Slot &Slot() const { return playing_; }

    // The two states of the saves screen that are reached by a click, for a caller
    // that has no mouse.
    //
    // `Crafting::Open` exists for exactly this and says why: a probe cannot press a
    // button, and a probe that reproduced the renaming row or the confirm box would be
    // photographing the reproduction rather than the screen (§25.5). They are the only
    // way in from outside; nothing in the game calls either.
    void Renaming(const std::string &text) {
        rename_ = text;

        Typing(text.empty() ? Field::None : Field::Rename);
    }

    void Asking(const std::string &id) { asking_ = id; }

    // Which save is picked, so a sheet can be taken of a row that is selected rather
    // than of a list nobody has touched.
    void Pick(int row) { Select(row); }

    int Picked() const { return picked_; }

    int Listed() const { return static_cast<int>(saves_.size()); }

    // Where the preview picture goes, this frame.
    //
    // Public for the one check that cannot be made from outside without it: the box has
    // to be the shape a screenshot is, or a picture fitted into it is letterboxed by
    // construction — which it was, and it read as a strip floating in a hole. The
    // layout is private to the screen and this is the one rectangle worth asking about.
    Rectangle Preview() const;

private:
    bool CanBack() const { return stack_.size() > 1; }

    // Which field, if any, is taking keys.
    //
    // One enum rather than a bool per field. Three of them take typing now — the
    // seed, the name of a new world and the renaming of an old one — and three
    // bools is a state where two of them are true, which is two carets and every
    // keystroke going into both.
    enum class Field { None, Seed, Name, Rename };

    // Puts the keyboard into one field and takes it out of every other.
    void Typing(Field field);

    // The row of the list the pointer is over, or -1.
    int RowAt(Vector2 where) const;

    void Select(int row);

    // Loads the picture beside the selected save, or drops the one that was there.
    // Called when the selection moves rather than while drawing, because `Draw` is
    // const and because a texture loaded in a draw is a texture loaded again every
    // frame the pointer rests on a row.
    void Show(int row);

    // The seed as it is typed, rather than as a number.
    //
    // Kept as text because that is what the player is editing: a field that stored
    // an int could not hold "-" on the way to "-12", could not hold the empty
    // string that means "surprise me", and could not take a word. Minecraft takes
    // a word and hashes it, which is what Seeded does.
    std::string seed_;

    // What a new world is to be called, and what an old one is being renamed to.
    std::string name_;
    std::string rename_;

    Field field_ = Field::None;

    Gamemode mode_ = Gamemode::Survival;

    // Whether the gamemode list is hanging open under its row.
    bool dropped_ = false;

    // The saves as they were last read off the disk, and which of them is picked.
    //
    // Read rather than watched: a folder that changes under the menu is not a case
    // worth carrying machinery for, and `Refresh` runs on every path that could
    // have changed it.
    std::vector<save::Slot> saves_;

    int picked_ = -1;
    int scroll_ = 0;

    // The save being asked about before it is deleted, or empty.
    //
    // A field rather than a screen of its own, and that is the exception to this
    // module's own rule. A confirm is not a place the player has gone — it is a
    // question about the screen they are on, and pushing it on the stack would make
    // the back arrow mean "cancel" on one screen and "go up" on every other.
    std::string asking_;

    // The preview picture, and which save it belongs to.
    Texture2D shot_{};
    std::string shotOf_;

    // Which save the world in the loop came from. Empty before there is one.
    save::Slot playing_;

    // What the loading screen is showing. A buffer rather than a pointer, because
    // the line is composed by whoever is doing the work and must not outlive the
    // frame they composed it in.
    char made_[96] = "";
    float share_   = 0.0f;

    std::vector<Screen> stack_{Screen::Title};
};

} // namespace menu
