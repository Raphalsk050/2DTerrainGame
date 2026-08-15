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
#include <optional>
#include <unordered_map>
#include <vector>

// How far the top of a mature plant is held over by the wind, and how far it
// swings about wherever it is being held — both as a share of its own height, at
// the hardest this world can blow.
//
// Published rather than kept in grove.cpp because two things outside need them.
// sod.h mirrors this pair deliberately, so that a wood and the grass under it lean
// together, and a mirror that cannot see what it is mirroring drifts out of true.
// And a probe has to be able to measure the sway against the texel grid it is drawn
// on: below one plant pixel a crown does not move at all, and a calm afternoon that
// falls under that floor is a wood standing frozen — see ReportWind.
//
// The reasoning behind there being two terms rather than one, and behind the floor
// under the second, is with their definitions in grove.cpp.
inline constexpr float kSwayHold  = 0.055f;
inline constexpr float kSwaySwing = 0.016f;
inline constexpr float kSwayIdle  = 0.55f;

// How fast a shed leaf goes down, in world pixels per second, before the per-cell
// jitter that spreads it.
//
// How *far* it falls is not a constant beside this and deliberately: a leaf falls
// the height of the tree it came off and then wraps back to the crown. One distance
// for the whole world was set near the tallest crown there is, so on anything
// shorter most of every leaf's life was spent below the ground it had already
// landed on — half the field invisible, and the sideways travel, which is quadratic
// in how far through the fall a leaf is, only ever reaching the flat start of that
// curve. The wind was being thrown away exactly where it would have been seen.
//
// Published rather than kept in grove.cpp for the same reason as the sway pair
// above: the probe that reports how far a gale carries a leaf has to ask the real
// figure rather than keep its own copy.
inline constexpr float kLeafFall = 26.0f;

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
    //
    // Takes the sky for the same reason Draw does: fruit hangs on a branch, and the
    // branch is where the wind put it.
    void DrawFruit(const weather::Sky &sky, flora::Season season, float now) const;

    // How many leaves the last DrawDrift put on screen.
    //
    // The field is a pure function of the clock with nothing stored, so there is no
    // population to inspect and no way to tell a wood shedding nothing from one
    // whose leaves are all being drawn somewhere unexpected. Counting what was drawn
    // is the only honest answer to "is it shedding more in this gale", and that
    // question is the whole point of the field.
    int Drifting() const { return drifting_; }

    // Leaves coming off the crowns, over the plants and inside the light.
    //
    // Two kinds, drawn together because they are the same thing to look at. The
    // drift is a field rather than a particle system — see the constants beside it
    // — and the season decides how hard the wood is shedding. The bursts are what
    // an axe knocks loose, and they run whatever the season is: a tree taking a
    // blow in January still sheds.
    void DrawLeaves(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const;

    // The whole sheet, at one screen pixel per texel, for checking that what was
    // drawn is what was meant. Drawn in screen space.
    void DrawSheet() const;

    // What a plant *is* to everything that is not the eye: the box an axe has to
    // land in, the rectangle the sprite is drawn into, and the point the plant is
    // seated on. Drawn in world space, over the world.
    //
    // The three are separate answers to "where is this tree" and they come from
    // separate code: the strike box is built from the species table, the sprite
    // rectangle from the baked image and its anchor, and the base from the ground
    // the plant was scattered onto. Nothing makes them agree — they are only ever
    // as consistent as the arithmetic in three places happens to be — and when
    // they disagree what a player meets is a tree that cannot be hit where it is
    // drawn, which reads as the tree being in the wrong place rather than as the
    // box being in the wrong place.
    //
    // That is not a thing any report can print, because the question is where two
    // rectangles are relative to a picture. It has to be looked at, which is the
    // whole of why this exists.
    void DrawCollision(flora::Season season, float now) const;

    // Lands a blow. Reports what was collected outright — nothing yet, since a
    // tree gives up its wood when it hits the ground and not when it is struck.
    //
    // Called only on the frame a swing begins. Player::AttackHitbox is live for
    // the whole strike window, so a caller reading that instead lands nine blows
    // per swing.
    void Strike(Rectangle hitbox, float damage, Vector2 from, float now);

    // Whether there is anything here a swing would connect with — a standing
    // trunk, a sapling, or a stump left to clear.
    //
    // Asked by the hand before it decides which tool the click is, so that one
    // button can chop a tree and dig the ground without the player choosing. It
    // shares its geometry with Strike rather than describing it again: a cursor
    // that lit up on trees the axe could not reach would be worse than no cursor.
    bool TimberAt(Rectangle probe, float now) const;

    // Plants one by hand at `foot` — the top of the ground it is to stand on,
    // worked out by whoever is holding the sapling. Returns false if something is
    // already growing there.
    //
    // The spot is handed in rather than looked up here, and that is the fix for a
    // sapling landing under a ramp instead of on it. What this used to read was
    // flora::Ground, which is the skyline: the shape of the land, memoised per
    // column and deliberately blind to digging so that a wood does not rearrange
    // itself around a hole. Right for growing a forest, wrong for a hand — a
    // player who has built a ramp and clicks on it means the ramp, and the skyline
    // answers with the hillside underneath it.
    bool Plant(flora::Species species, Vector2 foot, float now);

    // What a sapling would look like standing there, drawn faded.
    //
    // The real sprite of the real species at the real size, rather than a marker:
    // what the player is asking is where the thing will be, and half of that
    // answer is how much room it takes. Cached under an id of its own per species,
    // so hovering costs one bake and then nothing.
    void DrawGhost(flora::Species species, Vector2 foot, flora::Season season, Color tint) const;

    int VisiblePlants() const { return static_cast<int>(plants_.size() + undergrowth_.size()); }
    int DrawnPlants() const { return sheet_.Held(); }

    // How many plants the world can no longer describe on its own.
    //
    // Reported for the reason World::RememberedEdits is: it is the price of the
    // wood remembering what was done to it, and it should be visible rather than
    // suspected.
    //
    // It goes back down for a plant that healed and stayed standing, and never
    // for one that came down: a felled cell has to be remembered for good or the
    // tree grows back in it. See Forget.
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

    // Whether a plant is standing in a snowfield, and so wears snow on whatever
    // of it faces the sky.
    //
    // Asked of the generator's own covers rather than of the world, for the same
    // reason placement is: what a tree looks like must not change because somebody
    // dug a hole beside it, and a crown that lost its snow when the ground under
    // the trunk was cleared would do exactly that. It is also what makes this
    // cheap enough to ask per plant per frame — no chunk, no lattice, two noise
    // fields and a climate.
    bool Snowy(const flora::Plant &plant) const;

    // What has happened to one plant, and the whole of what the world cannot
    // work out for itself.
    //
    // Only written when the player causes something. A plant nobody has touched
    // has no record at all, which is why an untouched wood costs nothing: **no
    // record means a mature, undamaged tree**, and that is the case for every
    // tree in the world at the start.
    //
    // It gives records back where it can: a tree that was struck and left to heal
    // ends up indistinguishable from one nobody touched, so its entry goes and the
    // procedural pass answers for it again. A felled one never can — the absence
    // of a tree is exactly what an absent record cannot say — so those are kept
    // for good, on the same terms as World::edits_.
    // How many blows a plant remembers for the sake of what came off it.
    //
    // Three. A burst of leaves lasts a little under a second and the swing itself
    // cannot repeat faster than about a third of one, so three is exactly as many
    // as can ever be in the air at once.
    //
    // A single timestamp was not enough, and the fault was plain to watch: each
    // blow overwrote the last, so the leaves already falling from the previous one
    // jumped back into the crown and started again. What a second blow does is add
    // leaves to the air, not replace the ones in it.
    static constexpr int kBlows = 3;

    struct TreeState {
        // Seconds on the weather clock. Negative means never.
        float plantedAt = -1.0f;
        float updatedAt = 0.0f;
        float struckAt  = -1.0f;
        float felledAt  = -1.0f;
        float fruitAt   = -1.0f;

        // When the last few blows landed, as a ring. `struckAt` above is the
        // newest of them and is what the wound and the wobble are measured from;
        // these are what is still in the air.
        float blowAt[kBlows] = {-1.0f, -1.0f, -1.0f};
        int blowSlot         = 0;

        // Towards mature, in [0,1]. One for anything the world grew itself.
        float growth = 1.0f;

        // Share of its toughness left, in [0,1].
        float health = 1.0f;

        // And of the stump's, once the tree is down. Kept apart from the trunk's
        // own because they are two things to cut through: felling the tree spends
        // the first and leaves the second untouched, which is what makes a stump
        // a second job rather than a leftover.
        float stumpHealth = 1.0f;

        // Which way it went over. Held rather than recomputed because the player
        // moves, and a tree halfway down must not change its mind.
        bool fallLeft = false;

        // Whether it has already given up its wood. An impact happens once.
        bool dropped = false;

        // Whether the stump has been cut out too, leaving bare ground.
        //
        // The record outlives it, and has to: a plant with no record is a mature
        // tree, so forgetting a cleared cell is the same as growing the tree back
        // in it. This is the one bit that has to be kept for good — see Forget.
        bool cleared = false;

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
        float fade         = 1.0f;  // What is left of the trunk as it goes.
        bool fallLeft      = false;
        bool stump         = false; // Down and gone; only the cut trunk is left.
        bool cleared       = false; // And the stump cut out after it: nothing there.

        // Seconds since the last blow landed, or negative if it has never been
        // struck. Distinct from `shake`, which is what that blow does to a crown:
        // a stump has no crown, and what a blow does to one is its own thing.
        float struck = -1.0f;

        // How far through the stump the axe has got, in [0,1]. What every blow so
        // far adds up to, as against the one jolt the last of them caused.
        float wear = 0.0f;
    };

    Standing Read(const flora::Plant &plant, float now) const;

    // How far along a plant the overlay has never heard of is, in [0,1].
    //
    // The world's own answer, and a pure function of the plant's cell and the
    // clock — so a wood of mixed ages costs exactly as many records as a wood of
    // mature trees, which is none. Most plants answer one outright; the rest
    // start somewhere short of it and close the gap at their own pace.
    //
    // It is what `Read` falls back on and what `Remember` seeds a fresh record
    // from, which is what keeps a young tree young when somebody finally hits it.
    float Aged(const flora::Plant &plant, float now) const;

    // What is left where a tree came down, as the rectangle it occupies.
    //
    // One answer for both the drawing and the axe. Two would be two of them
    // disagreeing the first time either changes, and a stump that cannot be hit
    // where it is drawn is a stump the player decides is unbreakable.
    // `stage` is how far along the tree was when it came down — a stump is the
    // foot of the tree that stood there, not of the tree it would have become.
    Rectangle StumpRect(const flora::Plant &plant, flora::Stage stage) const;

    // `standing` is what carries the blow: how long ago the axe landed, and how
    // far through the stump it has got.
    void DrawStump(const flora::Plant &plant, const Standing &standing) const;

    // The record for a plant, made if there is not one yet.
    TreeState &Remember(const flora::Plant &plant, float now);

    // Drops the records that have nothing left to say, so the overlay tracks what
    // is outstanding rather than everything that ever happened.
    void Forget(float now);

    // Throws a felled plant's drop table onto the ground. Resolved from the
    // plant's own cell, so the same tree always gives the same wood however many
    // times it is grown and cut.
    //
    // `share` scales every count, and `woodOnly` drops everything but the timber:
    // together they are what a cleared stump gives. A stump is the bottom of the
    // trunk and nothing else, so it cannot hand over the apples and the saplings
    // that were in the crown — those came down with the tree.
    void Yield(const flora::Plant &plant, const TreeState &state, float now, float share = 1.0f,
               bool woodOnly = false);

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

    // The two halves of DrawLeaves: the year's own shedding, and what has been
    // knocked out of a crown lately.
    void DrawDrift(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const;
    void DrawBurst(const weather::Sky &sky, flora::Season season, float now) const;

    // Records a blow against a plant, for the wound, the wobble and the leaves.
    void Blow(TreeState &state, float now) const;

    // The chips an axe throws out of the wood it lands in, `since` seconds after
    // the blow. Bark rather than leaves, and thrown from where the axe struck
    // rather than off the crown.
    void Chips(const flora::Plant &plant, Vector2 at, float since, int salt, float wind) const;

    // The rectangle a blow has to land in for this plant to feel it, or nothing
    // where the plant is not something to swing at. The single source of that
    // geometry — see TimberAt.
    std::optional<Rectangle> StrikeRect(const flora::Plant &plant, float now) const;

    // One burst of leaves off a crown, and the few things that separate an axe
    // landing in a trunk from a whole tree landing on the ground.
    struct Burst {
        float since = 0.0f; // Seconds since the leaves were knocked loose.
        int rounds  = 1;    // How many times over kBurstLeaves is thrown.

        // Keeps the two bursts one tree can throw from being the same leaves
        // twice: everything about a leaf is hashed out of the plant's id, so two
        // bursts with one salt are one burst drawn twice.
        int salt = 0;

        // Where the crown is when it sheds. Zero is a standing tree; a felled one
        // is laid over by the same angle the fall drew it at, so the leaves come
        // off the crown where the crown actually is rather than where the tree
        // used to be standing.
        float angle = 0.0f;

        // How hard they leave it. A blow shakes them loose; the ground throws
        // them.
        float vigour = 1.0f;

        // The air they were thrown into, in pixels per second — Sky::WindAt at the
        // moment the blow landed, not now and not a share of anything. A leaf off a
        // crown is in the same air as one the year shed, and the two have to be
        // carried by it alike or the burst reads as debris falling through a still
        // room.
        float wind = 0.0f;
    };

    void Spray(const flora::Plant &plant, flora::Season season, const Burst &burst) const;

    // The averages the off-screen rate is credited at. Measured from the sky once,
    // at Configure.
    float meanLight_ = 0.5f;
    float meanWater_ = 0.5f;

    // Counted by DrawDrift, which is const — see Drifting.
    mutable int drifting_ = 0;

    // One crown that has leaves to lose, reduced to the four numbers a falling leaf
    // needs: where the trunk stands, how far out from it the crown drops, the ground
    // it stands on, and how tall it is — the height being what the fall spans.
    struct Shedder {
        float at;
        float reach;
        float foot;
        float height;

        const flora::SpeciesDef *def;
    };

    // The deciduous crowns standing under the view, gathered once per frame by
    // DrawDrift so that its sweep does not ask the whole wood about every cell.
    // Scratch rather than state: it is rebuilt from `plants_` each call, and is a
    // member only so that filling it does not allocate every frame.
    mutable std::vector<Shedder> shedders_;

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
