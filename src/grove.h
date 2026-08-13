#pragma once

#include "canopy.h"
#include "flora.h"
#include "raylib.h"
#include "terrain.h"
#include "weather.h"
#include "world.h"

#include "drop.h"
#include "item.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// The plants standing in the world right now.
//
// Everything about a plant that the noise can answer is answered by flora, and
// none of it is kept: this holds the plants under the view for the length of a
// frame and throws them away, the same way the world holds chunks. What it will
// come to own, and the reason it exists as an object at all, is the part the
// noise cannot answer — which tree has been cut, how far a planted one has grown
// — and that is a sparse record of the few the player has touched rather than a
// list of all of them.
//
// It reads the world but never writes to it. A tree stands on the surface and
// stops nothing, so nothing in the lattice has to know it is there.
class Grove {
public:
    // Bakes the sprites as well as settling the placement, so it needs a window
    // open. Costs a few milliseconds, once.
    // The sky is read once, for the averages an unwatched plant grows at. It is
    // asked here rather than every frame because they are properties of the
    // settings and not of the moment.
    void Configure(const flora::Settings &settings, const terrain::Settings &terrain, const weather::Sky &sky);
    void Unload();

    // Grows the plants covering `view` plus the margin a canopy can hang over,
    // then runs what has been done to them: trees finishing their fall give up
    // their wood, pickups fall and are gathered into `into`, and records with
    // nothing left to say are dropped.
    void Update(const World &world, Rectangle view, Vector2 player, float now, float dt, Inventory &into);

    // Offers every visible canopy to the light as shade, so a wood has a dark
    // floor. Re-offered each frame; see World::AddShade.
    void Shade(World &world, float now) const;

    // Drawn between the terrain and the character, and so before the light is
    // multiplied over the frame: a tree is lit by the same daylight as the ground
    // it stands on, and needs to know nothing about it to be.
    //
    // `now` is the weather clock, which is what the sway runs on: it speeds up
    // under F7 with the wind that drives it, rather than the two coming apart.
    void Draw(const weather::Sky &sky, flora::Season season, float now) const;

    // The fruit hanging on whatever bears it, at the time of year it bears.
    void DrawFruit(flora::Season season, float now) const;

    // Leaves coming off the crowns, over the plants and inside the light.
    //
    // A field rather than a particle system: see the constants beside it. The
    // season decides how hard the wood is shedding.
    void DrawLeaves(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const;

    // The whole sheet, at one screen pixel per texel, for checking that what was
    // drawn is what was meant. Drawn in screen space.
    void DrawSheet() const;

    // Lands a blow. Reports what was collected outright — nothing yet, since a
    // tree gives up its wood when it hits the ground and not when it is struck.
    //
    // Called only on the frame a swing begins. Player::AttackHitbox is live for
    // the whole strike window, so a caller reading that instead lands nine blows
    // per swing.
    void Strike(Rectangle hitbox, float damage, Vector2 from, float now);

    // Plants one by hand, if the ground will take it.
    bool Plant(flora::Species species, Vector2 world, float now);

    // The species the climate at a position favours most — what the world would
    // have grown there if it had grown anything.
    flora::Species Suited(float worldX) const;

    int VisiblePlants() const { return static_cast<int>(plants_.size() + undergrowth_.size()); }
    int DrawnPlants() const { return sheet_.Held(); }

    // How many plants the world can no longer describe on its own.
    //
    // Reported for the reason World::RememberedEdits is: it is the price of the
    // wood remembering what was done to it, and it should be visible rather than
    // suspected. Unlike the edits, it goes back down — see TreeState.
    int RememberedPlants() const { return static_cast<int>(remembered_.size()); }

    const Drops &Fallen() const { return drops_; }
    Drops &Fallen() { return drops_; }

    const std::vector<flora::Plant> &Plants() const { return plants_; }
    const flora::Settings &Settings() const { return settings_; }

private:
    // Fills the surface buffer and points `ground_` at it. The scatter asks about
    // columns either side of what it is placing — the lie of the land, and the
    // footing under a trunk — so the run prepared is wider than the view by more
    // than a canopy.
    void ReadGround(const World &world, Rectangle view);

    // What has happened to one plant, and the whole of what the world cannot
    // work out for itself.
    //
    // Only written when the player causes something. A plant nobody has touched
    // has no record at all, which is why an untouched wood costs nothing: **no
    // record means a mature, undamaged tree**, and that is the case for every
    // tree in the world at the start.
    //
    // It also gives records back, which World::edits_ does not: once a felled
    // tree has regrown there is nothing to remember about it, so the entry is
    // dropped and the procedural pass answers for it again.
    struct TreeState {
        // Seconds on the weather clock. Negative means never.
        float plantedAt = -1.0f;
        float updatedAt = 0.0f;
        float struckAt  = -1.0f;
        float felledAt  = -1.0f;
        float fruitAt   = -1.0f;

        // Towards mature, in [0,1]. One for anything the world grew itself.
        float growth = 1.0f;

        // Share of its toughness left, in [0,1].
        float health = 1.0f;

        // Which way it went over. Held rather than recomputed because the player
        // moves, and a tree halfway down must not change its mind.
        bool fallLeft = false;

        // Whether it has already given up its wood. An impact happens once.
        bool dropped = false;

        // Only meaningful for a plant the overlay asserts rather than modifies —
        // one somebody put there, which the procedural pass knows nothing about.
        bool planted         = false;
        std::uint8_t species = 0;

        // Where a planted one stands. The procedural pass knows nothing about it,
        // so unlike every other plant in the world its position has to be kept.
        Vector2 at{};
    };

    // Where a plant is in its life, as the drawing needs it.
    struct Standing {
        flora::Stage stage = flora::Stage::Mature;
        float shake        = 0.0f;  // Horizontal wobble from a recent blow, in world px.
        float felling      = -1.0f; // Seconds into its fall, or negative if upright.
        float fade         = 1.0f;  // What is left of it while it lies there.
        bool fallLeft      = false;
        bool stump         = false; // Down and gone; only the cut trunk is left.
    };

    Standing Read(const flora::Plant &plant, float now) const;

    // What is left where a tree came down.
    void DrawStump(const flora::Plant &plant) const;

    // The record for a plant, made if there is not one yet.
    TreeState &Remember(const flora::Plant &plant, float now);

    // Drops the records that have nothing left to say, so the overlay tracks what
    // is outstanding rather than everything that ever happened.
    void Forget(float now);

    // Throws a felled plant's drop table onto the ground. Resolved from the
    // plant's own cell, so the same tree always gives the same wood however many
    // times it is grown and cut.
    void Yield(const flora::Plant &plant, const TreeState &state, float now);

    // Advances every growing plant in view to `now`.
    //
    // Time the player was elsewhere is credited at the average the place gives,
    // and time they were here at what it is actually giving. That split is forced
    // rather than chosen: the light field does not exist outside the region last
    // solved and reads as dark there, and the sky only remembers a quarter of an
    // hour of rain — so integrating the real thing over an absence would credit
    // every unwatched tree with a moonless drought.
    //
    // What it buys is the property worth having: a tree grows at the same average
    // rate whether it was watched or not, and watching it in the rain visibly
    // speeds it up.
    void Ripen(const World &world, float now, float dt);

    // Adds the plants somebody put there to the ones the world grew.
    void Planted(Rectangle view);

    // Fells anything left standing on ground that has been dug out from under it.
    void Undermine(const World &world, float now);

    // Clears the undergrowth out of the trunks and thins it by the shade over it.
    void Thin();

    // The averages the off-screen rate is credited at. Measured from the sky once,
    // at Configure.
    float meanLight_ = 0.5f;
    float meanWater_ = 0.5f;

    // A cache, so drawing from it is a const operation on this object even
    // though it fills itself in as it goes: what a plant looks like is settled
    // by the plant, and the sheet only remembers the answer.
    mutable canopy::Sheet sheet_;

    std::unordered_map<std::int64_t, TreeState> remembered_;

    // How many trees the player has planted, which is also the next one's key.
    //
    // Planted trees live in the same map as everything else, keyed from a base no
    // cell index can reach — see kPlantedBase. One map means Remember, Read,
    // Strike, Yield and Forget go on answering for a planted tree exactly as they
    // do for a grown one, and none of them has to know which it is holding.
    std::int64_t nextPlanted_ = 0;

    Drops drops_;

    flora::Settings settings_{};
    terrain::Settings terrain_{};

    std::vector<flora::Plant> plants_;

    // The floor of the wood, scattered on its own much finer lattice.
    //
    // A second pass rather than a term in the first: the two are an order apart
    // in size, and one pass spanning both would have to size its cell to the
    // widest tree and then waste most of them on a fern with a tree's worth of
    // ground around it.
    std::vector<flora::Plant> undergrowth_;

    // The skyline under one frame's plants. Kept between frames only so that
    // updating does not allocate; nothing in it survives the call that fills it.
    std::vector<float> surface_;
    std::vector<float> sunk_;
    flora::Ground ground_{};
};
