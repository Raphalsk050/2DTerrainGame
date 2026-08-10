#include "water.h"

#include <algorithm>
#include <cstddef>

namespace water {
namespace {

// Moves `amount` from one cell to another on the next-state buffer.
//
// Every transfer in the automaton goes through here, as a subtraction paired
// with an addition of the same value. That pairing is the whole reason total
// mass survives a step: no rule is allowed to assign an absolute value or to
// drop a remainder.
void Transfer(std::vector<float> &next, int from, int to, float amount) {
    next[from] -= amount;
    next[to] += amount;
}

// Transfers larger than kMinFlow are halved. Applying a full difference makes
// the pair overshoot and trade the same excess back and forth instead of
// settling, so the surface never comes to rest.
float Damp(float flow) {
    return (flow > kMinFlow) ? flow / 2.0f : flow;
}

} // namespace

void Buffer::Resize(int c, int r) {
    cols = c;
    rows = r;

    const auto count = static_cast<std::size_t>(c) * r;
    mass.assign(count, 0.0f);
    blocked.assign(count, 0);
}

float StableState(float totalMass) {
    if (totalMass <= kMaxMass) return kMaxMass;
    if (totalMass < 2.0f * kMaxMass + kMaxCompress) {
        return (kMaxMass * kMaxMass + totalMass * kMaxCompress) / (kMaxMass + kMaxCompress);
    }
    return (totalMass + kMaxCompress) / 2.0f;
}

void Step(Buffer &buffer, const Settings &settings) {
    // Neighbours are read from the current state and written to the next, so
    // the outcome does not depend on the order cells are visited. Writing in
    // place would make liquid drift towards whichever side is swept last.
    std::vector<float> next = buffer.mass;

    for (int i = 0; i < buffer.cols; i++) {
        for (int j = 0; j < buffer.rows; j++) {
            const int cell = buffer.Index(i, j);

            if (buffer.blocked[cell] != 0) continue;

            float remaining = buffer.mass[cell];
            if (remaining <= 0.0f) continue;

            // Downwards, up to what the cell below can hold once compression is
            // taken into account.
            // True when nothing underneath can take the liquid, so it has to
            // find its way out sideways instead of falling.
            bool restingOnSolid = true;

            if (buffer.InBounds(i, j + 1)) {
                const int below = buffer.Index(i, j + 1);

                if (buffer.blocked[below] == 0) {
                    restingOnSolid = false;

                    float flow = StableState(remaining + buffer.mass[below]) - buffer.mass[below];
                    flow *= std::clamp(settings.fallRate, 0.0f, 1.0f);

                    // Damping only applies when the cell below already holds
                    // liquid. It exists to stop two cells trading the same
                    // excess back and forth, and a cell falling into empty
                    // space has nothing to trade with. Halving the transfer
                    // there smears a falling stream into a trail that loses
                    // half its mass every step until no cell along it is worth
                    // drawing, which is why running water breaks into
                    // disconnected drops.
                    if (buffer.mass[below] > kDryMass) flow = Damp(flow);

                    flow = std::clamp(flow, 0.0f, std::min(settings.maxSpeed, remaining));

                    Transfer(next, cell, below, flow);
                    remaining -= flow;
                }
            }

            // Sideways, levelling the difference with each neighbour.
            //
            // The side served first is alternated by step parity. Both sides
            // read the same previous state, but the first one served takes from
            // a larger remainder, and always serving the same one first walks a
            // pool slowly in that direction.
            const int firstSide = ((buffer.step & 1u) == 0u) ? -1 : 1;

            // A cell carrying liquid above it is under load and pushes harder.
            const bool loaded = buffer.InBounds(i, j - 1) && buffer.blocked[buffer.Index(i, j - 1)] == 0 &&
                                buffer.mass[buffer.Index(i, j - 1)] > kDryMass;

            for (const int dx : {firstSide, -firstSide}) {
                if (remaining <= 0.0f) break;
                if (!buffer.InBounds(i + dx, j)) continue;

                const int side = buffer.Index(i + dx, j);
                if (buffer.blocked[side] != 0) continue;

                // A quarter of the difference per side leaves the pair
                // converging instead of trading the same excess back and forth;
                // viscosity scales that down towards standing still.
                // A neighbour with room beneath it passes the liquid straight
                // on, so it is offered the remainder instead of a share of the
                // difference.
                //
                // Without this a film on a slope never leaves. Its cells have
                // rock underneath, so nothing falls, and they hold the same
                // mass as each other, so nothing levels: the coating is a
                // resting state and the hillside stays wet for ever. Handing it
                // to the draining side costs one step sideways and one step
                // down, which is half the speed of moving diagonally in a
                // single step and does not outrun a free fall.
                const int under = buffer.Index(i + dx, j + 1);
                const bool drainable =
                    buffer.InBounds(i + dx, j + 1) && buffer.blocked[under] == 0 && buffer.mass[under] < kMaxMass;

                float flow;

                if (drainable) {
                    // Capped at what the neighbour can still hold. Handing over
                    // the whole remainder regardless packs the receiving cell
                    // past the level the compression law allows, and a body of
                    // liquid ends up squeezed into fewer cells than it should
                    // occupy, sitting lower than its volume calls for.
                    flow = std::min(remaining, kMaxMass - buffer.mass[side]);
                } else {
                    flow = (buffer.mass[cell] - buffer.mass[side]) / 4.0f;
                    if (loaded) flow *= settings.pressureSpread;
                    flow *= 1.0f - std::clamp(settings.viscosity, 0.0f, 1.0f);
                    flow = Damp(flow);
                }

                flow = std::clamp(flow, 0.0f, std::min(settings.maxSpeed, remaining));

                Transfer(next, cell, side, flow);
                remaining -= flow;
            }

            // Upwards, only with the excess left over once the cell is holding
            // more than its stable share.
            if (remaining > 0.0f && buffer.InBounds(i, j - 1)) {
                const int above = buffer.Index(i, j - 1);

                if (buffer.blocked[above] == 0) {
                    float flow = remaining - StableState(remaining + buffer.mass[above]);
                    flow *= settings.riseDamping;
                    flow = std::clamp(Damp(flow), 0.0f, std::min(settings.maxSpeed, remaining));

                    Transfer(next, cell, above, flow);
                    remaining -= flow;
                }
            }
        }
    }

    buffer.mass = std::move(next);
    buffer.step++;
}

float TotalMass(const Buffer &buffer) {
    // Summed in double precision: a float accumulator over tens of thousands of
    // cells loses enough low bits to look like a leak that is not there.
    double total = 0.0;
    for (const float m : buffer.mass) total += m;

    return static_cast<float>(total);
}

} // namespace water
