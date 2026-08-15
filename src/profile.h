#pragma once

#include <chrono>
#include <cstdio>

// Where a frame goes, measured rather than guessed at.
//
// Zones nest, and each one reports both the time inside it and the time inside
// it that was not spent in a zone below it. That distinction is the whole point:
// a phase that reads as expensive is usually one cheap step wrapped around an
// expensive one, and a total alone cannot say which.
//
// The registry is a fixed array indexed by an id handed out once per call site,
// so a zone costs two clock reads and an add. Nothing here is thread safe: the
// zones are placed around whole phases, which the main thread runs, and never
// inside the workers a phase splits itself across.
namespace profile {

inline constexpr int kMaxZones = 64;
inline constexpr int kMaxDepth = 16;

struct Zone {
    const char *name    = nullptr;
    long long inclusive = 0;  // Nanoseconds inside this zone.
    long long child     = 0;  // Of which, inside a zone nested in it.
    long long calls     = 0;
    int depth           = -1; // How deep it was first entered, for the report.
};

struct State {
    Zone zones[kMaxZones]{};
    int count = 0;

    // Time charged to zones opened at each depth, so a zone can subtract what
    // its children took when it closes.
    long long nested[kMaxDepth]{};
    int depth = 0;

    int frames   = 0;
    bool enabled = false;
};

inline State &Get() {
    static State state;
    return state;
}

inline int Register(const char *name) {
    State &state = Get();

    if (state.count >= kMaxZones) return kMaxZones - 1;

    const int id = state.count++;

    state.zones[id].name = name;

    return id;
}

// A zone chosen at run time from a fixed set of names, for a phase that repeats
// per cascade or per pass and is worth seeing broken out.
// Ids are held one higher than they are, so that a zero-initialised table reads
// as "not registered yet" without a sentinel pass over it.
inline int Slot(int *ids, const char *const *names, int index, int count) {
    if (index < 0 || index >= count) index = count - 1;

    if (ids[index] == 0) ids[index] = Register(names[index]) + 1;

    return ids[index] - 1;
}

class Scope {
public:
    explicit Scope(int id) : id_(id) {
        State &state = Get();

        if (!state.enabled || state.depth >= kMaxDepth) {
            id_ = -1;
            return;
        }

        if (state.zones[id].depth < 0) state.zones[id].depth = state.depth;

        state.nested[state.depth] = 0;
        state.depth++;

        start_ = std::chrono::steady_clock::now();
    }

    ~Scope() {
        if (id_ < 0) return;

        const long long elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();

        State &state = Get();

        state.depth--;

        Zone &zone = state.zones[id_];

        zone.inclusive += elapsed;
        zone.child += state.nested[state.depth];
        zone.calls++;

        if (state.depth > 0) state.nested[state.depth - 1] += elapsed;
    }

    Scope(const Scope &)            = delete;
    Scope &operator=(const Scope &) = delete;

private:
    int id_ = -1;
    std::chrono::steady_clock::time_point start_{};
};

inline void Begin() { Get().enabled = true; }

inline void Frame() { Get().frames++; }

// Throws away everything measured so far, keeping the registry. Called after
// the first frames, whose cost is generation and not steady state.
inline void Reset() {
    State &state = Get();

    for (int i = 0; i < state.count; i++) {
        state.zones[i].inclusive = 0;
        state.zones[i].child     = 0;
        state.zones[i].calls     = 0;
    }

    state.frames = 0;
}

inline void Report(const char *title) {
    const State &state = Get();

    const int frames = (state.frames > 0) ? state.frames : 1;
    const double runs = static_cast<double>(frames);

    // The frame's own total, which every share is measured against. Zones at
    // depth zero are the phases of a frame and do not overlap, so their
    // inclusive times add up to it.
    double total = 0.0;

    for (int i = 0; i < state.count; i++) {
        if (state.zones[i].depth == 0) total += static_cast<double>(state.zones[i].inclusive);
    }

    total /= runs * 1.0e6;

    std::printf("\n%s over %d frames\n\n", title, frames);
    std::printf("%-34s %8s %10s %10s %8s\n", "zone", "calls/f", "ms/f", "self ms", "share");

    for (int i = 0; i < state.count; i++) {
        const Zone &zone = state.zones[i];

        if (zone.calls == 0) continue;

        const double ms   = static_cast<double>(zone.inclusive) / (runs * 1.0e6);
        const double self = static_cast<double>(zone.inclusive - zone.child) / (runs * 1.0e6);

        // Indented by how deep the zone sits, so the tree is readable without
        // the names having to spell the hierarchy out.
        char label[64];
        const int indent = (zone.depth > 0) ? zone.depth * 2 : 0;

        std::snprintf(label, sizeof(label), "%*s%s", indent, "", zone.name);

        std::printf("%-34s %8.2f %10.3f %10.3f %7.1f%%\n", label, static_cast<double>(zone.calls) / runs, ms, self,
                    (total > 0.0) ? 100.0 * ms / total : 0.0);
    }

    std::printf("\n%-34s %8s %10.3f            (%.0f fps)\n", "frame", "", total, (total > 0.0) ? 1000.0 / total : 0.0);
}

} // namespace profile

#define PROFILE_JOIN2(a, b) a##b
#define PROFILE_JOIN(a, b) PROFILE_JOIN2(a, b)

// Opens a zone for the enclosing scope. The id is handed out once, on the first
// pass through this line, so a zone costs no lookup.
#define PROFILE_ZONE(label)                                                                                            \
    static const int PROFILE_JOIN(kProfileId, __LINE__) = ::profile::Register(label);                                  \
    ::profile::Scope PROFILE_JOIN(kProfileScope, __LINE__)(PROFILE_JOIN(kProfileId, __LINE__))

// The same, for a phase that runs once per level and is worth seeing per level.
// `names` is an array of at least `count` labels, indexed by `index`.
#define PROFILE_ZONE_AT(names, index, count)                                                                           \
    static int PROFILE_JOIN(kProfileIds, __LINE__)[count]{};                                                           \
    ::profile::Scope PROFILE_JOIN(kProfileScope, __LINE__)(                                                            \
        ::profile::Slot(PROFILE_JOIN(kProfileIds, __LINE__), names, index, count))
