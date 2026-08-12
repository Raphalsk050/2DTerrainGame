#pragma once

#include "flora.h"
#include "raylib.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Plants, drawn into pixels one tree at a time and kept while they are in sight.
//
// The one module in the project that makes an image rather than issuing draw
// calls, and it exists because a tree is the first thing here with an inside. A
// material is a field and one colour, so it can be rasterised from that field
// every frame for nothing; a canopy is four greens, a dark accent under every
// mass of leaves, notched bark, and limbs showing through the gaps, and none of
// that can be recovered from a threshold test.
//
// Every tree is its own drawing. That is the whole point and it was worth
// getting wrong once to learn: a fixed set of variants baked at startup turned a
// screenful of birches into three pictures repeated, which is the one thing a
// procedural wood must never look like. A plant's shape is a pure function of the
// cell it grew in, so there is no reason for there to be a small number of them
// — a tree is rasterised as it comes into view and its slot is recycled when it
// leaves, and the world can hold as many distinct trees as it has cells.
//
// Because a tree is baked at its own size, its texels are square whatever size
// it is. That is why the size can be continuous: what puts fractional texels on
// screen is stretching a sprite after the fact, not building it large.
//
// What is baked is the *form* and never the hour. The light reaching a tree
// arrives from the same full-screen multiply that lights the ground it stands on,
// and the sun swings through the day; a sprite carrying its own sun would be lit
// from the right at the moment every cloud in the sky was lit from the left. So
// the shading here is the depth of a mass into its own crown, which is true at
// every hour, and the direction is left to the light solve.
namespace canopy {

// One drawn plant: where it sits in the sheet, and where the foot of its trunk
// is inside that rectangle.
//
// The anchor is what a plant is positioned by. A tree grows out of the ground at
// one point and everything else hangs off it, so a sprite is placed by its trunk
// foot rather than by a corner — which is also the point it turns about when it
// comes down.
struct Sprite {
    Rectangle source{};
    Vector2 anchor{};
};

// One plant's pixels, with no texture and no window involved.
//
// The whole of the drawing, exposed on its own because it is the half of this
// module that can be looked at: in the world a tree is thirty texels tall behind
// a hillside at whatever the light is doing, and whether a notch came out as a
// notch is a question about the image. `pixels` is filled row by row, `width` and
// `height` are the size it came out, and `anchor` is where the trunk foot sits
// inside it.
void Render(const flora::Plant &plant, flora::Stage stage, flora::Season season, std::vector<Color> &pixels,
            int &width, int &height, Vector2 &anchor);

// The largest a plant can come out, in texels. What a slot has to hold.
int SlotWidth();
int SlotHeight();

class Sheet {
public:
    // Allocates the sheet. Needs a window open.
    void Create();
    void Unload();

    // Starts a frame. Everything not asked for between one call and the next has
    // left the view and its slot may be taken.
    void Begin();

    // The plant, drawn if it is not already held.
    //
    // Never null while the plant has ever been drawn. When the frame's budget is
    // spent — which is what keeps a hundred plants arriving at once from costing
    // one long frame — this hands back whatever that same plant already has, of
    // any stage or season, rather than nothing.
    //
    // That fallback is not a nicety. Turning the season changes the key of every
    // plant on screen at once, so without it half the wood vanished for two
    // frames every time; and a caller that reads this to decide where a canopy
    // casts its shade would otherwise shade trees it had not drawn, which put
    // grey blobs in an empty sky.
    const Sprite *Acquire(const flora::Plant &plant, flora::Stage stage, flora::Season season);

    Texture2D Texture() const { return texture_; }
    bool Ready() const { return texture_.id != 0; }

    // Whether this plant has a drawing at all, of any stage or season.
    //
    // A question rather than a request: it takes no budget and draws nothing.
    // What it is for is the caller that has to know whether a plant is on screen
    // before acting on it — a canopy may only cast shade if it was drawn, or the
    // shadow arrives before the tree.
    bool Holds(std::int64_t cell) const;

    int Held() const { return static_cast<int>(lookup_.size()); }
    int Capacity() const;

private:
    // Identity of what a slot holds. A cell index, which of the stages it is at
    // and which season it wears — the three things that change what a tree looks
    // like.
    static std::uint64_t Key(std::int64_t cell, flora::Stage stage, flora::Season season);

    struct Slot {
        std::uint64_t key = 0;

        // The plant it belongs to, kept apart from the key so that a plant can be
        // found again whatever stage or season it was last drawn at.
        std::int64_t cell = 0;

        Sprite sprite{};

        // The frame it was last asked for. A slot older than the frame before
        // this one is out of sight and can be taken.
        std::uint64_t used = 0;

        bool taken = false;
    };

    void Draw(const flora::Plant &plant, flora::Stage stage, flora::Season season, Slot &slot, int column, int row);

    Texture2D texture_{};

    std::vector<Slot> slots_;
    std::unordered_map<std::uint64_t, int> lookup_;

    // Cell to slot, ignoring stage and season. What the fallback above is found
    // through.
    std::unordered_map<std::int64_t, int> byPlant_;

    std::uint64_t frame_ = 0;
    int drawnThisFrame_  = 0;

    // Scratch, kept between plants so that drawing one does not allocate.
    std::vector<Color> pixels_;
};

} // namespace canopy
