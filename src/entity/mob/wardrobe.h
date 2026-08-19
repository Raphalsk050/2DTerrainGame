#pragma once

#include "core/sheet.h"

namespace mob {

struct Def;

// The strips one creature is drawn from, loaded once and kept.
//
// A cache on the model of `canopy::Sheet`, and for the same reason that one gives: what
// a creature looks like is settled by its row, and this only remembers the answer. The
// first draw of the first boar loads three files; every boar after that costs nothing.
//
// **It is asked for by row and never by path.** A creature names a folder under
// `assets/mobs/` and the clips inside it are always called the same three things, so
// there is no filename anywhere outside this file and nothing to misspell in a table.
// Adding a creature with art is a folder and one field.
struct Wardrobe {
    sheet::Strip idle;
    sheet::Strip walk;
    sheet::Strip run;

    // Whether the load has been attempted. Distinct from whether it worked: a creature
    // with no art, and one whose art is missing, must both stop trying — a load
    // retried every frame is a file opened sixty times a second for as long as the
    // game is running.
    bool tried = false;

    bool Any() const { return idle.Ready() || walk.Ready() || run.Ready(); }
};

// What this creature wears, loading it on the first ask.
//
// Needs a window, so it cannot be called before one is open — which is why it is asked
// for at draw time rather than at startup, where `content::Open` runs and there is no
// graphics device yet.
const Wardrobe &Dressed(const Def &def);

// Gives back every texture. Called beside `Grove::Unload` on the way out, and for the
// same reason: a texture outliving the window it was made in is a crash on exit that
// only ever happens on somebody else's machine.
void Undress();

} // namespace mob
