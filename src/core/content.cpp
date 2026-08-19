#include "core/content.h"

#include "core/registry.h"

#include <cstdio>
#include <string>

namespace {

// Held rather than rebuilt, since `Summary` hands out a pointer.
std::string summary;

} // namespace

bool content::Open() {
    for (void (*freeze)() : registry::Freezers()) freeze();

    int wrong = 0;

    for (std::string (*check)() : registry::Checks()) {
        const std::string said = check();

        if (said.empty()) continue;

        // To stderr and not through the console: the console is a thing inside a
        // window that has not opened yet, and a content fault is exactly the case
        // where it never will.
        std::fprintf(stderr, "content: %s\n", said.c_str());

        wrong++;
    }

    // Built after freezing, so the counts are the ones the game will actually use.
    summary.clear();

    for (std::string (*count)() : registry::Counters()) {
        if (!summary.empty()) summary += ", ";

        summary += count();
    }

    if (wrong > 0) {
        std::fprintf(stderr, "content: %d fault%s — refusing to start\n", wrong, (wrong == 1) ? "" : "s");

        return false;
    }

    return true;
}

const char *content::Summary() {
    return summary.c_str();
}
