#pragma once

#include "core/mode.h"
#include "craft/craft.h"
#include "item/inventory.h"
#include "raylib.h"

#include <optional>

// The crafting panel: a column of recipes down the left edge, and a card beside it
// for the one being looked at.
//
// **The arrangement is Don't Starve's**, and it is the reason this is not another
// page of the inventory. There, crafting is a strip that is simply *there* while
// you play: you gather, and the things you can now make appear at the edge of your
// eye without being asked for. Minecraft's is the opposite idea — a grid you go and
// stand in front of, which is a place rather than a readout. This game already has
// the second thing, the pack behind Tab, and putting recipes in it would mean the
// player only ever learns what they can make when they go looking.
//
// So: always on screen while playing, never in the way, and it says one thing —
// here is what the wood and stone in your bag are worth.
//
// What the wiki does and does not settle is worth writing down, because it was
// looked up and it was thin: it gives the tabs, that a tab shows "what items need
// to be unlocked and which resources are needed to unlock them", and that the
// button reads *Prototype* when the ingredients are not there. It does not describe
// the shading of an unaffordable recipe or the ingredient counts anywhere in prose.
// The three states below are therefore the game's behaviour as it is played, and
// they are the states the mechanic was asked for — see craft::Standing, which is
// where they are decided rather than drawn.
class Crafting {
public:
    // Side of one recipe icon, and of one ingredient icon in the card.
    //
    // The first is the hotbar's own slot size, deliberately: a picture of a thing is
    // the same size wherever the interface shows it, or the strip reads as a
    // different kind of object from the bar. The second is smaller because an
    // ingredient is a *quantity* being checked off and not a thing being handled.
    static constexpr float kSlotSide = 44.0f;
    static constexpr float kNeedSide = 32.0f;

    static constexpr float kPad    = 6.0f;
    static constexpr float kMargin = 12.0f;
    static constexpr float kGap    = 10.0f;
    static constexpr float kInset  = 12.0f;

    static constexpr float kCardWide = 276.0f;

    // What one frame of clicking on the panel came to.
    //
    // The panel spends the ingredients and puts the product away itself — that is
    // one transaction and splitting it would let half of it happen. What it cannot
    // do is put the overflow on the ground, which needs the world and the player's
    // position, so that is reported exactly as `Inventory::Gesture` reports a thrown
    // stack and for the same reason.
    struct Gesture {
        // Whether the click landed on the panel at all. The caller uses it to keep
        // the same press from also digging the hillside behind it.
        bool took = false;

        bool built = false;

        // What the pack had no room for.
        Stack overflow{};

        // What to say to the player, or nothing.
        const char *said = nullptr;
    };

    Gesture Update(Inventory &pack, Gamemode mode);

    void Draw(const Inventory &pack, Gamemode mode) const;

    // Whether a screen point is on the panel as it stands this frame.
    //
    // Takes the pack and the mode because the panel's extent is a pure function of
    // them and of the window — which recipes are listed decides how tall the strip
    // is — plus the one thing that is not, which is the card this object has open.
    // Computed again rather than remembered from the draw: §14's rule, and the same
    // one every menu screen is held to.
    bool Contains(Vector2 screen, const Inventory &pack, Gamemode mode) const;

    // Which recipes are listed, and where each of them stands.
    //
    // Public and static because the probe draws the panel through it, and because
    // it is the answer to "what does the player see" — a question worth being able
    // to ask without a window.
    struct Listing {
        struct Row {
            int bill = 0; // Index into craft::Bills().
            craft::Standing standing = craft::Standing::Absent;
        };

        // As many as this can ever hold. What is actually listed is however many the
        // *window* has room for, which is a smaller number and is worked out by
        // `Fits` — a strip taller than the frame is a strip with recipes off the end
        // of it, and this game's window has a floor of four hundred pixels
        // (`config::kMinScreenHeight`), which is room for seven.
        //
        // Nothing here overlaps the hotbar, and that is arithmetic rather than luck:
        // the bar is centred and is 456 px wide in the narrowest window, so it starts
        // at x 92 and this strip ends at x 68. Only the height ever binds.
        static constexpr int kMost = 16;

        Row at[kMost]{};
        int count = 0;

        // How many were left off the end. Reported rather than swallowed, on
        // `Inventory::kSlotsPerPage`'s reasoning: a list that quietly stops is a list
        // whose last entries look like rows nobody added.
        int cut = 0;
    };

    // How many rows this window has room for.
    static int Fits();

    static Listing ListFor(const Inventory &pack);

    // Where everything is, this frame. A pure function of the listing, the open
    // card and the window.
    struct Layout {
        Rectangle strip{};
        Rectangle slot[Listing::kMost]{};

        bool open = false;
        Rectangle card{};

        // Inside the card.
        Rectangle icon{};
        Rectangle need[craft::kMaxNeeds]{};
        int needs = 0;
        Rectangle build{};
    };

    static Layout LayoutFor(const Listing &listing, int selected);

    // Which row of the listing has its card open, or -1.
    //
    // The open recipe is remembered as a handle and not as a position, because the
    // listing changes under it: spending the last of an ingredient takes a recipe
    // off the strip and every row below it moves up. A remembered position would
    // quietly open the card of whatever slid into that slot.
    int SelectedIn(const Listing &listing) const;

    // Opens a recipe's card, or shuts whatever is open.
    //
    // The panel opens its own cards from a click; this is for the callers that are
    // not a click — the probe, which has to photograph a card without a mouse, and
    // whatever keyboard shortcut wants one later. It takes a handle rather than a
    // position for the reason SelectedIn gives.
    void Open(std::optional<craft::Recipe> recipe) { open_ = recipe; }

private:
    std::optional<craft::Recipe> open_;
};
