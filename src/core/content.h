#pragma once

// The one line that turns a pile of self-registered rows into the game's content.
//
// Everything in `src/*/items`, `src/*/mobs`, `src/world/elements` and so on has put
// itself into a table by the time main runs. Two things still have to happen before
// any of it may be read, and both happen here:
//
//   1. **Freeze.** Ids are handed out, by name, so that they are the same on every
//      build. See the head of `registry.h` for why this cannot be done as the rows
//      arrive.
//   2. **Verify.** Every check any content file registered is run, all of them, and
//      the program stops if any fails.
//
// Called from main before the window opens and before any probe runs, and it is
// the only thing in the startup path that has to be in a particular place.
namespace content {

// Freezes every table and runs every check.
//
// Reports **all** the faults it finds rather than the first, because content faults
// come in batches — one rename breaks every row that named the old thing — and a
// tool that makes you fix them one build at a time is a tool that gets switched
// off. Returns false when something is wrong; the caller is expected not to
// continue.
bool Open();

// A one-line summary of what was registered, for the console and the startup log.
//
// It exists because the failure mode of a self-registering table is silence: a file
// left out of the build does not error, it simply is not there, and the only sign
// is a thing missing from the world. A count printed at startup is what makes that
// visible on the run it happens rather than the week after.
const char *Summary();

} // namespace content
