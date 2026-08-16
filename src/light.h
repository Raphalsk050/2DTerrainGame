#pragma once

#include "raylib.h"

#include <cmath>
#include <vector>

// Light as a solved radiance field, by Holographic Radiance Cascades.
//
// The method is Freeman, Sannikov & Margel 2025 (arXiv:2505.02041). It rests on the
// same observation as ordinary radiance cascades -- that light needs angular
// precision in inverse proportion to spatial precision, so a stack of grids trading
// one for the other holds the whole field for the price of one image -- and then
// fixes the thing that observation gets wrong.
//
// What it gets wrong is that halving the probe density on *both* axes throws away
// the resolution across the direction light arrives from, which is exactly the
// direction a shadow edge varies in. A small distant light behind an occluder makes
// a penumbra a few pixels wide, and the cascade that resolves the occluder has
// probes further apart than that; the edge falls between them and is reconstructed
// by interpolation, which is what rings and blurs. Holographic cascades keep full
// resolution perpendicular to the gathering direction and give it up only along it,
// where nothing varies quickly. That is the whole of the idea, and it is why this
// resolves a hard shadow at all.
//
// Two consequences worth knowing before reading the code:
//
//   - There is no ray marching with a step size, no coarsened copy of the world to
//     step over, and so no dial trading light leaking through walls against shadows
//     that come out too dark. Long rays are short rays joined end to end, exactly,
//     and a wall is a wall at every scale because no scale ever averaged it.
//
//   - Cost is constant for a region size. A cave packed with detail and an empty sky
//     take the same time, because nothing here skips empty space and nothing here
//     branches on the scene. The frame does not get slower where the world gets
//     interesting.
//
// It runs entirely on the GPU. The CPU fills a medium, hands it over, and reads back
// a fluence grid one frame later for whatever the game rules want to ask.
namespace light {

// Linear light, three channels, unbounded above. Kept linear all the way through
// because it is added and scaled at every step, and only becomes a colour at the
// very end.
struct Radiance {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

inline Radiance operator+(Radiance a, Radiance b) {
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}

inline Radiance operator*(Radiance a, float scale) {
    return {a.r * scale, a.g * scale, a.b * scale};
}

// Perceived brightness, which is what a game rule should be written against rather
// than any one channel.
inline float Luminance(Radiance value) {
    return 0.2126f * value.r + 0.7152f * value.g + 0.0722f * value.b;
}

// Maps unbounded radiance onto [0,1]. Light does not stop at a maximum but a screen
// does, and so does any rule phrased as "bright enough". Clipping instead would make
// everything near a torch equally, flatly white.
inline float Expose(float value, float exposure) {
    return 1.0f - std::exp(-value * exposure);
}

// What light travels through: one cell per lattice step of the region being lit.
//
// Three fields and no more, and the third is what makes this global illumination
// rather than direct light. `albedo` is what a cell reflects, and the solver feeds
// the last frame's fluence back through it, so a lit floor lights the ceiling above
// it and a torch fills a room instead of only the wall it faces.
//
// There is deliberately nothing here about the sky, the ground's height, or how deep
// daylight should reach. All three were properties the old solver needed because it
// could not transport daylight itself. This one can: the sky is radiance arriving
// from outside the region, a ray that starts underground is stopped by the rock it is
// in, and that is the whole mechanism.
struct Medium {
    int cols      = 0;
    int rows      = 0;
    float spacing = 1.0f;
    Vector2 origin{};

    // Extinction per cell of travel. Zero is empty air; around 32 is opaque, since
    // e^-32 is nothing. It is a coefficient rather than a share stopped, because the
    // tracer integrates it in closed form over whatever fraction of a cell a ray
    // actually crosses.
    std::vector<float> sigma;

    // What the cell reflects, per channel, in [0,1].
    std::vector<Radiance> albedo;

    // Radiance emitted per cell of travel. Not multiplied by sigma, so a flame glows
    // in open air where there is nothing to scatter off.
    std::vector<Radiance> emission;

    void Resize(int cols, int rows);
    void Clear();

    int Index(int i, int j) const { return j * cols + i; }
    bool InBounds(int i, int j) const { return i >= 0 && i < cols && j >= 0 && j < rows; }

    // World region the cells cover.
    Rectangle Bounds() const;
};

// Light arriving from beyond the region, as a function of direction alone.
//
// This is the paper's environment map, and it is the entire daylight model. It is a
// function of direction and not of position because that is what it means to be far
// away; everything positional about daylight -- that a cave is dark, that a cliff
// shades its own foot, that a cloud shades the ground under it -- is transport, and
// the solver does transport.
struct Sky {
    Radiance radiance{};

    // How far up a ray has to point before any sky reaches it, and where it has all
    // of it. Both are the cosine to straight up, so 0 is the horizon and 1 the
    // zenith.
    float horizon = -0.1f;
    float zenith  = 0.35f;

    // What the weather is holding back, in [0,1].
    float cover = 0.0f;
};

struct Settings {
    Sky sky{};

    // Scales radiance on its way to the screen and to any rule phrased in terms of
    // brightness. The one number to reach for when the world is uniformly too dark
    // or too bright.
    float exposure = 2.2f;

    // How different two cells' extinction may be before the cross blur refuses to
    // average them together. The blur exists to take out the last of the method's
    // checkerboard; this is what stops it carrying light across a wall while it does
    // so, which would give back the leaking the whole method avoids.
    float opacityGuard = 4.0f;

    // Whether last frame's fluence is fed back as a scattering source. Off, this is
    // direct light with volumetric occlusion -- useful for telling the two apart
    // when something looks wrong, and nothing else.
    bool bounce = true;

    // Whether the cross blur runs at all. Off is not a look, it is an instrument:
    // the blur exists to take out what is left of the method's row-to-row
    // checkerboard, and the only way to know whether it is still needed -- or
    // whether it is now just softening a field that no longer needs it -- is to
    // measure the roughness with and without.
    bool crossBlur = true;
};

// The solved field.
class Field {
public:
    ~Field();

    Field()                         = default;
    Field(const Field &)            = delete;
    Field &operator=(const Field &) = delete;

    // Must be called with a GL context current, and before the context goes.
    void Unload();

    // Solves the medium. Requires a current GL context and must not run inside
    // BeginDrawing -- it binds its own programs and buffers and does not put the
    // drawing state back.
    void Solve(const Medium &medium, const Settings &settings);

    // Fluence at a world position, from the readback of the last completed solve.
    // Positions outside the region read as dark: light is only solved where the game
    // is looking, so a rule asking about somewhere else is asking about somewhere
    // that was never lit.
    Radiance At(Vector2 world) const;

    // The same as a single number in [0,1]. This is what a game rule should be
    // written against.
    float LevelAt(Vector2 world) const;

    // One cell of the field, unfiltered. For the debug view, which wants to see the
    // grid the solver actually produced rather than a reading between two of them.
    Radiance ProbeAt(int i, int j) const {
        if (i < 0 || j < 0 || i >= cols_ || j >= rows_ || !primed_) return {};

        const std::size_t at = (static_cast<std::size_t>(j) * cols_ + i) * 4;

        return {field_[at], field_[at + 1], field_[at + 2]};
    }

    // Where that cell sits in the world.
    Vector2 ProbePosition(int i, int j) const {
        return {origin_.x + (i + 0.5f) * spacing_, origin_.y + (j + 0.5f) * spacing_};
    }

    // The exposed field as a texture, for the screen. One texel per medium cell.
    Texture2D Screen() const { return screen_; }

    int Cols() const { return cols_; }
    int Rows() const { return rows_; }
    Vector2 Origin() const { return origin_; }
    float Spacing() const { return spacing_; }
    float Exposure() const { return settings_.exposure; }

    // Whether the GPU side came up. False means the shaders did not compile or the
    // context is not 4.3, and the reason has already been logged.
    bool Ready() const { return ready_; }

    // Rays traced by the last solve, so the cost of a change can be seen rather than
    // guessed at. Counts real traced segments only, not the merges that stand in for
    // the long ones.
    long Rays() const { return rays_; }

private:
    // One cascade level's shape and where it starts in the pyramid buffer.
    struct Level {
        int gx      = 0;
        int gy      = 0;
        int dirs    = 0;
        int step    = 0;
        int offset  = 0;   // elements from the start of one parity's block
        int count   = 0;
    };

    // A compiled program and the uniform locations it uses. Locations are fetched
    // once: glGetUniformLocation is a string lookup and this dispatches a hundred
    // times a frame.
    struct Program {
        unsigned int id = 0;

        int gx = -1, gy = -1, dirs = -1, step = -1;
        int here = -1, below = -1, above = -1;
        int parity = -1, parityStride = -1;
        int sceneW = -1, sceneH = -1;
        int xDir = -1, yDir = -1, anchor = -1;
        int spanX = -1, spanY = -1;
        int skyRadiance = -1, skyHorizon = -1, skyZenith = -1, skyCover = -1;
        int opacityGuard = -1;

        void Locate();
    };

    bool Build();
    bool Allocate(int cols, int rows);
    void Upload(const Medium &medium);
    void Readback();

    void SetGrid(const Program &program, const Level &level) const;
    void SetScene(const Program &program) const;
    void SetRotation(const Program &program, int rotation) const;
    void SetSky(const Program &program) const;

    void Dispatch(int x, int y, int z, int localX, int localY, int localZ) const;

    void SolveRotation(int rotation);

    Settings settings_{};

    Program source_{};
    Program trace_{};
    Program mergeUp_{};
    Program boundary_{};
    Program mergeDown_{};
    Program finish_{};
    Program blur_{};

    unsigned int scene_      = 0;   // binding 0
    unsigned int sourceBuf_  = 0;   // binding 1
    unsigned int previous_   = 0;   // binding 2
    unsigned int pyramid_    = 0;   // binding 3
    unsigned int radianceA_  = 0;   // binding 4 / 5, ping-ponged
    unsigned int radianceB_  = 0;
    unsigned int accum_      = 0;   // binding 6
    unsigned int final_      = 0;   // binding 7

    Texture2D screen_{};

    // The pyramid's shape for each rotation. Rotations 0 and 2 walk the region the
    // long way and 1 and 3 across it, so the two have different level counts.
    std::vector<Level> levels_[4];
    int parityStride_ = 0;

    // Cell (i,j) of the medium, packed the way the shaders index it.
    struct Cell {
        float albedoR = 0.0f, albedoG = 0.0f, albedoB = 0.0f, sigma = 0.0f;
        float emitR = 0.0f, emitG = 0.0f, emitB = 0.0f, pad = 0.0f;
    };

    std::vector<Cell> staging_;
    std::vector<float> field_;   // the readback, four floats per cell

    int cols_       = 0;
    int rows_       = 0;
    int probeCols_  = 0;
    int probeRows_  = 0;
    float spacing_  = 1.0f;
    Vector2 origin_ = {};

    bool ready_   = false;
    bool built_   = false;
    bool primed_  = false;   // whether a solve has completed and the readback is real

    long rays_ = 0;
};

} // namespace light
