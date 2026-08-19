#include "probes/report.h"

#include "raylib.h"

const probes::Report *probes::Named(int argc, char **argv) {
    if (argc < 2 || argv == nullptr) return nullptr;

    for (const Report *def : Table().All()) {
        if (!TextIsEqual(argv[1], def->name)) continue;

        // Named but underfed. Reported as "not this probe" rather than as an error,
        // so that the caller prints its usage instead of a probe running against
        // arguments it does not have.
        if (argc < def->wants) return nullptr;

        return def;
    }

    return nullptr;
}
