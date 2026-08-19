#pragma once

#include <cstdint>

// A creature's own stream of small decisions.
//
// The only randomness in this project that is not a function of position, and it
// is worth saying why it is allowed to exist here when it is allowed nowhere else.
//
// Everything the world is made of is a pure function of `(position, seed)` —
// terrain, caves, ore, covers, woods — because a world has to be the same country
// twice and a chunk that came back different from the one that left is the fault
// that ends the byte-identical bar. None of that applies to a boar. A creature is
// not regenerated from anywhere: it is born, it walks about, and when the view
// leaves it is gone. There is nothing for its choices to have to agree with.
//
// What its choices *do* have to be is different from its neighbour's. Two boars
// standing in a field that turn on the same frame are not two boars, and a field
// sampled at the creature's position would give exactly that for two creatures
// standing in the same place. So each carries its own stream, seeded when it is
// born.
//
// xorshift32 rather than anything from `<random>`: it is four lines, it has no
// state but the number itself, it is the same on every machine, and a creature's
// gait does not need a Mersenne Twister.
namespace mob {

// Advances the stream and returns the next value. The seed is the state.
inline std::uint32_t Roll(std::uint32_t &seed) {
    // Zero is a fixed point of xorshift and would leave a creature perfectly
    // still for ever, which is a bug that looks exactly like a calm animal.
    if (seed == 0) seed = 0x9E3779B9u;

    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;

    return seed;
}

// The next value as a fraction in [0,1).
inline float Chance(std::uint32_t &seed) {
    // Twenty-four bits, which is the mantissa a float actually carries. Dividing
    // the whole thirty-two would round to values it cannot represent, and the
    // top of the range would occasionally come out as exactly 1.
    return static_cast<float>(Roll(seed) >> 8) / 16777216.0f;
}

// A fraction between two bounds.
inline float Between(std::uint32_t &seed, float low, float high) {
    return low + (high - low) * Chance(seed);
}

// An integer in an inclusive range.
inline int Count(std::uint32_t &seed, int least, int most) {
    if (most <= least) return least;

    return least + static_cast<int>(Roll(seed) % static_cast<std::uint32_t>(most - least + 1));
}

// A seed derived from another, so that a creature born from a spawner that itself
// has a stream does not share it.
//
// Mixed rather than incremented: consecutive seeds in xorshift produce visibly
// related first values, which for a sounder of boars born on one frame means four
// animals that all step off in the same direction.
inline std::uint32_t Spread(std::uint32_t of, std::uint32_t by) {
    std::uint32_t mixed = of ^ (by * 0x9E3779B9u);

    Roll(mixed);

    return mixed;
}

} // namespace mob
