#pragma once

#include "core/mode.h"
#include "save/record.h"
#include "raylib.h"

class World;
class Grove;
class Inventory;
class Player;

namespace fixture {
class Fixtures;
}

namespace mob {
class Herd;
}

// A whole world, written down and read back.
//
// `save/record.h` is how a save is spelled; this is what is in one. It owns the order
// of the sections and nothing else — every section is written by the module that
// holds it, for the reason the head of that file gives.
//
// ---
//
// **What is saved is what cannot be worked out again.** The generator is a pure
// function of position and seed, so a world is its seed plus the journal of what the
// player changed; a wood is its scatter plus the trees that have been touched; a
// county's animals are a function of the cell, plus the record of which cells have
// been asked and what has happened to what was in them. Everything else — chunks,
// silhouettes, the light, the pictures, the water — is derived, and derived state is
// not history. Writing it would make the file large, make it version-brittle, and
// make it possible for a save to hold two answers to one question.
//
// **What is not saved is state that is a fraction of a second old.** A swing halfway
// through, a creature mid-leap, a hurt flash, a stack on the cursor. A world you load
// is a world you arrive in, standing still — and every one of those fields would be
// the game telling you about a moment you did not see.
namespace save {

// Everything one save is made of, by reference.
//
// A bundle rather than a parameter list, on `probes::Bench`'s model and for its
// reason: adding something a save covers must not mean editing every call.
//
// The pointers are not owned and are all required. There is no such thing as saving
// half a world — a file holding the ground but not the chests is a world that loads
// with the player's things gone.
struct Game {
    World *world              = nullptr;
    Grove *grove              = nullptr;
    fixture::Fixtures *chests = nullptr;
    mob::Herd *herd           = nullptr;
    Inventory *pack           = nullptr;
    Player *player            = nullptr;

    bool Whole() const {
        return world != nullptr && grove != nullptr && chests != nullptr && herd != nullptr && pack != nullptr
               && player != nullptr;
    }
};

// What the save is called and what world it is, beside the state itself.
//
// Kept apart from `Game` because they are answers to different questions: the game is
// what is in the world, and this is which world it is. The seed in particular is not
// the world's to report — `World` is handed settings and does not keep the number it
// was made from.
struct Stamp {
    std::string name;

    int seed      = 0;
    Gamemode mode = Gamemode::Survival;
};

// Writes one, and the picture beside it where there is one.
//
// The picture is optional and the save is not held up by it: a world written without
// a preview lists perfectly well, and the alternative — refusing to save because a
// screenshot could not be taken — is losing a session over a thumbnail.
//
// Written to a temporary file and moved into place. A save is the one thing in this
// program that a player cannot make again, and a write interrupted halfway — the
// window closed, the disk full — must not be allowed to leave the old one destroyed
// and the new one half there.
bool Write(const Slot &slot, const Stamp &stamp, const Game &game, const Image *shot);

// Reads one back into a game that has already been rebuilt for its seed.
//
// **The order matters and it is the caller's to get right.** The world has to be
// rebuilt first: `World::Reset` clears the journal, so a load into an unrebuilt world
// would leave the last country's edits standing in the new one. `save::Read` refuses
// nothing about that — it cannot tell — which is why the loop does it in the stage
// before.
//
// Returns false on a save this build cannot read, having changed as little as it
// could: a name that resolves to nothing stops the read where it stands rather than
// filling the world with holes.
bool Read(const Slot &slot, Stamp &stamp, const Game &game);

} // namespace save
