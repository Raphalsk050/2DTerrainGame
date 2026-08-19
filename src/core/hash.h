#pragma once

#include <cstdint>

// A stable number for a name.
//
// It exists because a row's *index* is a poor seed. Indices are handed out by
// sorting the table's names (see `core/registry.h`), so they are stable across
// builds — and they still shift when a row is added, because everything after the
// new name moves up one. Anything that seeded a roll from an index would therefore
// have every existing tree in the world drop something different the day a new item
// was added, which is a world quietly rewriting itself for a reason nobody could
// see.
//
// A hash of the name does not shift. Adding a row changes nothing about any other
// row, which is the same promise the registry makes one level up.
//
// FNV-1a, 32 bits: it is six lines, it is `constexpr`, and what it is being asked
// for is a decorrelated number rather than a cryptographic one.
namespace hash {

inline constexpr std::uint32_t Of(const char *text) {
    std::uint32_t value = 2166136261u;

    if (text == nullptr) return value;

    for (const char *at = text; *at != '\0'; at++) {
        value ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*at));
        value *= 16777619u;
    }

    return value;
}

} // namespace hash
