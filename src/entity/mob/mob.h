#pragma once

#include "entity/body/body.h"
#include "entity/life/blow.h"
#include "entity/life/health.h"
#include "entity/mob/brain.h"
#include "entity/mob/life.h"
#include "entity/mob/mob_def.h"
#include "entity/mob/wits.h"

#include <cstdint>

class World;

namespace mob {

// One creature, alive in the world.
//
// It is composed rather than inherited from, and the three pieces are the whole
// design: a `body::Body` that walks, a `Brain` that wants, and a `life::Health`
// that can be spent. None of the three knows about the other two, and none of them
// knows what a boar is.
//
// What is *not* here is as much of the point as what is. There is no per-creature
// behaviour code, no virtual `Update`, no `class Boar : public Mob`. A subclass per
// creature is the obvious shape and it is the wrong one: it puts the difference
// between two animals in a vtable rather than in a table, so the difference cannot
// be printed, cannot be checked at startup, and cannot be authored by anybody who
// is not editing C++.
class Mob {
public:
    // Brings a slot to life. Everything about the creature is read off its row, so
    // this is the only place a `Kind` turns into a thing that exists.
    void Wake(Kind kind, Vector2 at, std::uint32_t seed);

    void Sleep() { live_ = false; }

    // The two halves of being put away and brought back.
    //
    // `Remember` is everything about this creature that cannot be worked out again —
    // where it got to, what it has left, and what it was in the middle of doing. The
    // rest is rebuilt from its row, which is why there is no body and no brain in a
    // `Life`.
    //
    // `Restore` is `Wake` plus that history. They are separate because a creature put
    // down by hand has no history and must not be handed a blank one that reads as
    // "full health, calm, standing still" — it *is* that, and `Wake` says so on its
    // own.
    Life Remember() const;
    void Restore(const Life &life);

    bool Live() const { return live_; }

    Kind Which() const { return kind_; }
    const Def &Made() const { return kinds::Of(kind_); }

    Rectangle Bounds() const { return body_.Bounds(); }
    Vector2 At() const { return body_.Position(); }
    Vector2 Centre() const { return body_.Centre(); }

    int Facing() const { return facing_; }

    const life::Health &Vigour() const { return health_; }

    // A frame of being alive: think, move, and report whether it struck.
    //
    // The strike is *reported* rather than applied, because what a blow does to the
    // player is the player's business and this class must not know there is one. The
    // herd is what joins the two.
    //
    // `watched` is whether it is near enough to be looked at. It does not change what
    // the creature does — only how often it decides, which is the one thing here with
    // a cost worth managing. See `kFarThink`.
    bool Update(const World &world, Vector2 quarry, bool quarryVisible, bool watched, float now, float dt);

    // Something hit it. Returns whether the blow landed — a refusal is the mercy
    // window doing its job (see `life::Health`), and a caller that ignored the
    // answer would knock a creature back sixty times a second while dealing no
    // damage at all.
    bool Take(const life::Blow &blow);

    void Draw() const;

    // A short word for what it is doing, for `--mobs` and the debug overlay. See
    // `Brain::Mood` for why a behaviour needs one and a picture does not.
    const char *Mood() const;

private:
    Kind kind_{};

    body::Body body_{};
    Wits wits_{};
    life::Health health_{};

    // Resolved once, when the creature is woken, from the name on its row.
    // `Verify` has already established at startup that the name is a behaviour
    // that exists, so this is never null on a live creature.
    const Brain *brain_ = nullptr;

    int facing_ = 1;
    bool live_  = false;

    // The last wish its brain produced, and how long until it is asked again.
    //
    // A creature the player cannot see thinks a few times a second instead of sixty,
    // and keeps walking on the wish it last had in between. Nothing about its motion
    // changes — the body still steps every frame — so it does not drift, fall
    // differently or walk through anything.
    //
    // It exists because the navigator is the one expensive thing a creature does:
    // reading the ground ahead is up to twenty-eight world lookups, and a waking box
    // at full screen can hold two hundred creatures. Sixty times a second for all of
    // them is several milliseconds of frame spent on animals nobody is looking at.
    body::Intent wish_{};
    float thinkIn_ = 0.0f;

    // Whether something hurt it since the brain was last asked, and where from.
    //
    // Held for exactly one think and then cleared. It is the creature's whole
    // memory of pain, and it lives here rather than in the brain because the brain
    // is shared by every creature of its temper.
    bool stung_ = false;
    Vector2 stungFrom_{};
};

} // namespace mob
