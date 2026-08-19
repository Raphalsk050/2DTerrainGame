#include "core/registry.h"

// The two lists, as function-local statics.
//
// Not namespace-scope vectors, and the difference is the whole reason this file
// exists: a registrar in another translation unit runs during static
// initialisation and would find a namespace-scope vector that may or may not have
// been constructed yet, depending on the order the linker chose. A function-local
// static is constructed on the first call, whenever that is, which is exactly the
// guarantee needed here.
std::vector<void (*)()> &registry::Freezers() {
    static std::vector<void (*)()> all;

    return all;
}

std::vector<std::string (*)()> &registry::Checks() {
    static std::vector<std::string (*)()> all;

    return all;
}

std::vector<std::string (*)()> &registry::Counters() {
    static std::vector<std::string (*)()> all;

    return all;
}
