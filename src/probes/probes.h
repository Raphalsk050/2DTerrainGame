#pragma once

#include "flora/grove.h"
#include "world/terrain.h"
#include "weather/weather.h"
#include "world/world.h"

#include <optional>

// The world measured rather than played.
//
// Every one of these opens a window, builds the world the settings describe, prints
// or draws what it was asked about, and leaves. They are the only way most of this
// project is judged: what a cave setting does, whether an ore generates at the rate
// its row claims, what a material's paint divides into, where the wind carries a
// leaf. See CLAUDE.md section 3.
//
// A module of its own because they are a *tool*, not a game: fifteen hundred lines
// of reporting sat in front of the loop, and the loop is thirty lines of it. What
// they share with the game is the world they measure, which is handed in.
namespace probes {

// Whether this run is one of these, and so wants a window that never shows.
//
// Asked before the window opens, which is why it is a function of the arguments
// alone: the probe needs a GL context and nothing else, and a window flashing up
// for a report that prints numbers is a report nobody can run in a loop.
bool Headless(int argc, char **argv);

// Runs whichever probe the arguments name.
//
// Nothing where this run is not a probe at all; otherwise the status the program
// should leave with. A status and not a flag, because several of these report a
// *verdict* — the grass memo agreeing with itself, the block exchange being exact —
// and a check that cannot fail a build is a check nobody runs twice.
//
// The window is left open: closing it is the caller's, which is what keeps the
// order of a probe's own teardown out of this file.
std::optional<int> Run(int argc, char **argv, World &world, Grove &grove, terrain::Settings &settings,
                       const weather::Settings &sky);

} // namespace probes
