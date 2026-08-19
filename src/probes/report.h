#pragma once

#include "core/registry.h"

#include <optional>

class World;
class Grove;

namespace terrain {
struct Settings;
}

namespace weather {
struct Settings;
}

// One way of measuring the world, as a row.
//
// The dispatch used to be two walls of `TextIsEqual`, one in `Headless` and one in
// `Run`, and they had to agree: a probe named in the second and forgotten in the
// first is a report that works and flashes a window up for a second, which nobody
// notices until they try to run it in a loop. That is the same duplication this
// project has been removing everywhere else, and it is worse here because the two
// halves are four hundred lines apart.
//
// A probe is a row now. It says its own flag, how many arguments it needs and
// whether it wants a window, and both halves of the dispatch read the one list.
namespace probes {

// What a probe is handed. A struct rather than five parameters, so that adding
// something a probe can see does not mean editing every probe that cannot.
struct Bench {
    int argc          = 0;
    char **argv       = nullptr;
    World *world      = nullptr;
    Grove *grove      = nullptr;
    terrain::Settings *settings = nullptr;
    const weather::Settings *sky = nullptr;
};

struct Report {
    // The flag, with its dashes: "--mobs".
    const char *name;

    // Smallest `argc` this probe can work with, counting the program name and the
    // flag. A run with fewer is not this probe.
    int wants = 2;

    // Whether this one wants a window on screen.
    //
    // Almost none of them do, and that includes the ones that draw: `--probe`
    // renders through the real draw path into a file, and it does that behind a
    // hidden window because what it needs is a GL context and not a viewer. A window
    // flashing up for a report that writes a PNG is a report nobody can run in a
    // loop. `--profile` is the exception — the draw is half of what it measures.
    bool shows = false;

    // One line for `--help`, in the register the rest of the reports use.
    const char *blurb = "";

    // What it does, and the status the program leaves with. A status and not a
    // flag, because several of these report a *verdict*, and a check that cannot
    // fail a build is a check nobody runs twice.
    int (*run)(const Bench &bench) = nullptr;

    static constexpr const char *kLabel = "probes";
};

inline registry::Table<Report> &Table() {
    return registry::Table<Report>::The();
}

// The probe these arguments name, if any.
//
// One function, asked by both halves of the dispatch, which is the whole point of
// the file: there is no second list to fall out of step with the first.
const Report *Named(int argc, char **argv);

} // namespace probes
