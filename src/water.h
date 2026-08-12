#pragma once

#include <vector>

// Cellular automaton for liquid on a regular lattice.
//
// The simulation runs on a flat buffer and knows nothing about how the world is
// partitioned, which is what makes its behaviour checkable: every transfer is
// applied as a matching subtraction and addition on the next state, so the
// total mass in the buffer after a step equals the total before it, whatever
// the iteration order.
namespace water {

// Mass a cell holds when at rest with nothing stacked on it.
inline constexpr float kMaxMass = 1.0f;

// Extra mass a cell accepts per unit of liquid above it. Without compression a
// column would never push down once every cell reached kMaxMass, and connected
// vessels would not level out.
inline constexpr float kMaxCompress = 0.02f;

// Transfers at or below this are applied whole. Larger ones are halved, which
// is what stops a surface from overshooting and sloshing between neighbours
// without ever settling.
inline constexpr float kMinFlow = 0.01f;

// Mass under which a cell is dry for display purposes. Nothing is removed at
// this level: culling it would be the usual optimisation but would silently
// destroy volume.
inline constexpr float kDryMass = 0.0001f;

// Behaviour of the liquid.
struct Settings {
    // Fraction of the available downward transfer applied per step, on top of
    // the standard damping. Lower values make the liquid fall lazily, and one is
    // as fast as the lattice allows: it is read as a share and clamped to it, so
    // nothing above one means anything.
    float fallRate = 1.0f;

    // Resistance to spreading sideways, in [0,1]. At 0 a surface levels as fast
    // as the lattice allows; at 1 horizontal flow stops entirely and the liquid
    // stacks in columns. Values near 1 read as tar, near 0 as a thin fluid.
    float viscosity = 0.0f;

    // Upward transfer carries only the excess of an over-compressed cell, and
    // is damped separately: applied in full, a deep column visibly pulses as
    // the excess bounces between its top and bottom.
    // Must stay above zero. Rising is the only way liquid reaches a higher
    // place, so at zero connected vessels never level: in a U-tube filled from
    // one side, the two surfaces settle three rows apart and stay there. At
    // 0.10 they come out exactly level.
    float riseDamping = 0.1f;

    // Extra sideways push given to a cell that has liquid resting on it.
    //
    // Without it a poured column piles into a hill and levels out far too
    // slowly, because an unpressured cell only ever hands a quarter of its
    // surplus to each side. Liquid under load spreads harder, which is the
    // cheapest stand-in for a pressure model.
    float pressureSpread = 3.0f;

    // Upper bound on a single transfer, which keeps a tall column from emptying
    // into one cell in a single step.
    float maxSpeed = 2.0f;
};

struct Buffer {
    int cols = 0;
    int rows = 0;

    std::vector<float> mass;            // Liquid held by each cell.
    std::vector<unsigned char> blocked; // Non-zero where another element fills the cell.

    // Counts steps taken. Only its parity is used, to alternate which side is
    // served first.
    unsigned int step = 0;

    int Index(int i, int j) const { return i * rows + j; }
    bool InBounds(int i, int j) const { return i >= 0 && i < cols && j >= 0 && j < rows; }

    void Resize(int c, int r);
};

// Mass the lower of two stacked cells settles at, given their combined mass.
float StableState(float totalMass);

// Advances the buffer by one step. Cells beyond the border act as walls, so
// liquid piles up at the edge of the simulated region rather than leaking out
// of it and taking its volume along.
void Step(Buffer &buffer, const Settings &settings);

float TotalMass(const Buffer &buffer);

} // namespace water
