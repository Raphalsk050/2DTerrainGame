#pragma once

#include "raylib.h"

#include <cmath>
#include <vector>

// Light as a field of directional samples, solved at several scales at once.
//
// The technique is radiance cascades. It rests on one observation: light needs
// angular precision in inverse proportion to spatial precision. Close to a
// point, a shadow edge is sharp and exactly where it falls matters, but a
// handful of directions is enough to say which way the light came from. Far
// away the opposite holds. A penumbra widens in proportion to the distance from
// what casts it, so distant light is smooth in space and nothing is gained by
// sampling it finely, while telling two distant sources apart takes many
// directions.
//
// So the field is kept as a stack of cascades. Each one gathers light over an
// interval four times longer than the one below it, with four times as many
// directions and half the probe density on each axis. Every cascade therefore
// holds the same number of samples, and the whole stack costs a small multiple
// of one image rather than growing with the distance light is allowed to
// travel.
//
// They are solved from the top down and merged downwards. A ray that reaches
// the end of its own interval without being stopped continues into the interval
// above, which has already been solved, so what arrives at cascade zero is
// light gathered over the entire range, still labelled with the direction it
// came from. Nothing here is a blur or a falloff curve: light spreads because
// rays were traced, which is why it turns corners and softens with distance on
// its own.
namespace light {

// Linear light, three channels, unbounded above. Kept linear rather than as a
// screen colour because it is added and scaled all the way through the solve,
// and only becomes a colour at the very end.
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

// A height no world coordinate reaches, standing in for a column with no ground
// in it at all.
inline constexpr float kUnreachable = 1.0e9f;

// Perceived brightness of a radiance, which is what a game rule should be
// written against rather than any one channel.
inline float Luminance(Radiance value) {
    return 0.2126f * value.r + 0.7152f * value.g + 0.0722f * value.b;
}

// Maps unbounded radiance onto [0,1].
//
// Light does not stop at a maximum, but a screen does, and so does any rule
// phrased as "bright enough". Clipping instead would make everything near a
// torch equally, flatly white.
inline float Expose(float value, float exposure) {
    return 1.0f - std::exp(-value * exposure);
}

// What light travels through: one cell per lattice vertex of the region being
// lit, holding what stops light and what gives it off.
//
// The medium knows nothing about the world that filled it. It is a flat buffer
// with an origin, exactly like the liquid automaton's, which is what keeps the
// solver free of any chunk bookkeeping.
struct Medium {
    int cols      = 0;
    int rows      = 0;
    float spacing = 1.0f;
    Vector2 origin{};

    // Share of the light stopped by one cell of travel, in [0,1].
    std::vector<float> extinction;

    // Light given off within a cell, per cell of travel.
    std::vector<Radiance> emission;

    // World Y at which the ground begins, looking down from the open sky, one
    // per column.
    //
    // The cascades know only the world inside the region they are given. Above
    // it there is nothing to march through, so a ray leaving through the top
    // would be credited with sunlight whatever happens to stand between it and
    // the sky, and a sealed cavern a screen below the surface would be lit like
    // a field at noon. This is what stands between: the height of the ground in
    // each column, found once and read by every ray that asks for the sky.
    //
    // Left empty, nothing blocks the sky and every clear ray reaches it.
    std::vector<float> skyline;

    // Share of the daylight held back by whatever stands between the column and
    // the sky without stopping it outright — cloud — in [0,1], one per column.
    //
    // Sits beside the skyline and for the same reason: both are properties of what
    // is above a column, found once and read by every ray that asks for the sky.
    // The skyline says whether the sky can be seen at all; this says how much of it
    // is getting through.
    //
    // It belongs here rather than in the sky's own radiance because a single
    // radiance is one number for the whole world, and the whole point of a cloud is
    // that it shades the ground under itself and not the field next to it.
    //
    // Left empty, nothing shades the sky.
    std::vector<float> cover;

    // How deep the daylight carries into the ground at full strength, in world
    // units, one per column.
    //
    // A property of what the ground is made of and not of the light, which is why
    // it arrives from outside: the world lays a cover of soil, sand or snow over
    // the rock and the generator decides how thick it is column by column. The
    // daylight should reach the bottom of that cover and start to fail in the rock
    // under it — and a constant would be wrong in both directions at once, cutting
    // the band short wherever the cover ran deep and lighting bare stone wherever
    // it ran thin.
    //
    // Left empty, Settings::sunDepth answers for every column.
    std::vector<float> sunDepth;

    void Resize(int cols, int rows);
    void Clear();

    int Index(int i, int j) const { return i * rows + j; }
    bool InBounds(int i, int j) const { return i >= 0 && i < cols && j >= 0 && j < rows; }

    // World region the cells cover, each cell being the square of one spacing
    // centred on its own lattice vertex.
    Rectangle Bounds() const;
};

// Light arriving from beyond everything the medium describes.
//
// Added at the end of a ray that was never stopped, rather than along it. A
// glowing band of sky laid into the medium would work until the lit region
// moved: how much of the band a ray crossed would depend on how far the region
// happened to reach, and the whole world would change brightness as the camera
// walked. As a background it depends only on whether the sky was in view from
// there, which is a property of the world.
struct Sky {
    Radiance radiance{};

    // World Y above which a ray sees the full sky. Y grows downward.
    float horizon = 0.0f;

    // Depth below the horizon over which it fades to nothing, so the sky does
    // not end along a ruled line drawn across the light.
    float fade = 1.0f;

    // Height below which Medium::cover applies, and the distance over which it
    // comes in. Y grows downward, so "below" is a larger number.
    //
    // A cloud shades what is under it. It does not shade what is beside it and it
    // does not shade itself, but the cover is one figure for a whole column, so
    // without a floor under it the shadow runs the entire height of that column:
    // the open sky above the cloud is drawn in shadow, and so is the cloud.
    //
    // Defaults to reaching everywhere, which is the right answer for a medium that
    // has no cover in it.
    float coverBelow = -kUnreachable;
    float coverFade  = 1.0f;
};

struct Settings {
    // How many cascades. Each one quadruples the distance light is gathered
    // over, so this is the reach of the whole solve: cascade zero's interval
    // times (4^cascades - 1) / 3. Beyond it, light simply is not collected.
    //
    // Four reaches about a screen height at the resolution below, which is as
    // far as anything on screen can be from anything else that lights it. A
    // fifth doubles the reach again for roughly a millisecond, and is worth it
    // only if the view grows.
    int cascades = 4;

    // Directions in cascade zero. The cascade above has four times as many, so
    // by the fourth there are hundreds and a distant source is placed to within
    // a degree.
    int baseDirections = 4;

    // Cascade zero's probe spacing, counted in medium cells. An integer,
    // because the probe grid has to land on the same world positions from one
    // solve to the next; a probe grid that slid by a fraction of a cell as the
    // region moved would make the light crawl.
    //
    // This is the resolution of the light, and the one setting worth spending
    // on: it decides how sharp a shadow edge can be and how finely a surface is
    // lit. One probe per cell is as fine as the world itself is described.
    int probeCells = 1;

    // Length of cascade zero's interval, as a multiple of its probe spacing.
    //
    // One is the natural choice and the one the whole scheme is balanced
    // around: it makes a cell at the far end of the interval subtend about as
    // much angle as one of cascade zero's four directions covers. Raising it
    // gathers more per cascade and misses small sources near the probe, which
    // shows up as light blinking as it is walked past.
    float intervalScale = 1.0f;

    // How deep light works into the materials it cannot pass through, in world
    // units. Brightness falls off by a factor of e over this distance, so a few
    // times it is black.
    //
    // Solving alone leaves every solid probe dark, which is right about the
    // inside of a rock and wrong about its face: what a surface shows is the
    // light arriving at it, and none arrives inside. Without this the world is
    // a black silhouette against a bright sky at noon.
    //
    // It is a depth rather than a lower opacity on the material itself, so that
    // a wall still stops light dead. Letting light through the rock instead
    // would light its face by also lighting whatever stood behind it, and a
    // seam of ore inside a cliff would glow through the cliff.
    //
    // It is also the number that decides how far down ore stays hidden, so it
    // is a balance setting as much as a look one.
    // Short on purpose. This is a sheen on the face of a material, not a glow
    // inside it: a lamp standing on a ledge should light the top of the ledge
    // and not the underside of it. Raised much past a wall's own thickness, the
    // nearest lit space to a cell on the far side of a floor becomes the lit
    // room on the near side, and the light comes through the floor.
    // Now a hard bound rather than a half-life: rock further than this from
    // open space is black, whatever is shining on the other side of it. Read it
    // as the thickest wall light can work its way through.
    float surfaceReach = 80.0f;

    // How far into a material the light carries with nothing taken off it, in
    // world units, before `surfaceReach` starts to dim it.
    //
    // Without it the very face of a material is already in shadow. The sweep
    // above measures from open space, so the first solid probe is a probe's
    // width in and is dimmed by that much before anything is drawn — and since
    // the falloff is squared, a face that ought to be showing the full daylight
    // in front of it came out at six sevenths of it, with the ground under a
    // sunlit meadow reading as dusk within a few pixels.
    //
    // A face is not in shadow. What a surface shows is the light arriving at it,
    // and the light arriving at it is the light of the space it faces — which the
    // sweep already carried there and then discounted for a distance the face has
    // not actually travelled.
    //
    // One probe's width, and it cannot usefully be less: the field is solved one
    // probe per lattice cell and drawn as flat blocks, so anything under that is
    // a fraction of a block nothing can express.
    float surfaceLip = 6.0f;

    // How deep the open sky carries into the ground straight below it, in world
    // units, at full strength.
    //
    // Measured downward and not as a distance to the nearest open space, and that
    // distinction is the whole of this. The sweep above takes the *nearest* open
    // probe, which is right for a wall and wrong for the ground: a ledge with a
    // cave under it has its lower half nearer the cave than the sky, so the lit
    // band stopped partway down and then stopped at a different height in the next
    // stretch of country. What that drew was daylight that did not follow the
    // surface — bright over the deep ground, black over the thin, changing
    // abruptly wherever the cave below happened to come close.
    //
    // Straight down, it is the same depth everywhere and the band follows the
    // ground the way daylight does.
    //
    // Only used where the medium has nothing to say about a column — bare rock
    // with no cover on it at all. Where there is a cover, Medium::sunDepth is
    // what answers, because the depth the day should reach is a property of the
    // ground rather than a number chosen here.
    float sunDepth = 12.0f;

    // How far the daylight is averaged sideways before it is carried down, in
    // probes either side.
    //
    // The measurement that asked for it: along one stretch of ordinary ground the
    // light standing over the surface read 0.36, 0.34, 0.26, **0.11**, 0.13, 0.14,
    // 0.19 — a hole three probes wide at the inside corner of a terrace riser,
    // where the step above shades the ground at its foot. That much is true, and a
    // corner should be darker. What is not true is the shape it drew: carried the
    // whole depth of the band at full strength, one dark probe became a dark slot
    // running forty pixels down into the ground, and the band stopped following the
    // surface.
    //
    // Averaged rather than taken at the maximum, so a real shadow — a cloud, a
    // canopy, the lee of a cliff — still darkens the ground under it. Those are
    // tens of probes across and survive a blur of three; the corner is one probe
    // and does not.
    int sunBlend = 3;

    // How conservatively a long ray reads the world, in [0,1].
    //
    // A far cascade steps in strides of many cells and reads a coarsened copy
    // of the world to keep from stepping over walls. How a cell that is part
    // wall should be coarsened is the whole question. Averaging is the gentle
    // answer and gives soft distant shadows, but a cell straddling a surface
    // then reads as letting half the light past, and near a surface every cell
    // is such a cell: what it draws is a cone of daylight spreading down
    // through the ground under anything standing on it. Taking the strongest
    // instead makes a wall a wall at every scale, and costs distant shadows
    // that come out a shade too dark.
    //
    // One is fully conservative, zero fully soft. Turn it down if the world is
    // too dark, up if light comes through the floor.
    float coarseOcclusion = 0.8f;

    Sky sky{};

    // Scales radiance on its way to the screen, and to any rule phrased in
    // terms of brightness. The one number to reach for when the world is
    // uniformly too dark or too bright.
    float exposure = 2.2f;
};

// The solved field.
//
// Holds the cascade stack between solves so that a moving region does not
// reallocate a megabyte sixty times a second.
class Field {
public:
    void Solve(const Medium &medium, const Settings &settings);

    // Light arriving at a world position from every direction at once,
    // interpolated between the four nearest probes.
    //
    // Positions outside the region that was solved read as dark. Light is only
    // computed where the game is looking, so a rule that asks about somewhere
    // else is asking about somewhere that was never lit.
    Radiance At(Vector2 world) const;

    // The same, as a single number in [0,1]. This is what a game rule should
    // be written against: crops that need light to grow, creatures that will
    // not stand in it, a torch that has to be bright enough to hold them off.
    float LevelAt(Vector2 world) const;

    // Cascade zero's probe grid, which is what the screen is lit from. Probe
    // (i,j) sits at the centre of its own cell, so the grid can be handed
    // straight to a texture and interpolated by the hardware.
    int Cols() const { return cols_; }
    int Rows() const { return rows_; }
    Vector2 Origin() const { return origin_; }
    float Spacing() const { return spacing_; }
    float Exposure() const { return settings_.exposure; }

    Radiance ProbeAt(int i, int j) const;
    Vector2 ProbePosition(int i, int j) const;

    // Rays traced by the last solve, reported so the cost of a change to the
    // settings can be seen rather than guessed at.
    long Rays() const { return rays_; }

private:
    // One level of the stack. Probe count falls by four per level and direction
    // count rises by four, so every cascade is the same size.
    struct Cascade {
        int cols       = 0;
        int rows       = 0;
        int directions = 0;
        float spacing  = 0.0f;

        std::vector<Radiance> radiance;

        // What the ray had left when it reached the end of its interval. It is
        // the weight the cascade above is merged in with: light from further
        // away arrives only through whatever the near interval did not stop.
        std::vector<float> transmittance;

        int Index(int i, int j, int d) const { return (i * rows + j) * directions + d; }
    };

    // Successively halved copies of the medium, each cell the average of the
    // four beneath it.
    //
    // A long ray has to take long steps to stay affordable, and long steps over
    // the finest copy would stride straight through walls thinner than the
    // step. Reading a copy whose cells are the size of the step instead means a
    // wall is never missed; it is averaged, so a thin one dims the light rather
    // than stopping it. That is also what makes distant light soften instead of
    // breaking into bands.
    struct Pyramid {
        int levels    = 0;
        int cols      = 0;
        int rows      = 0;
        float spacing = 1.0f;
        Vector2 origin{};

        std::vector<std::vector<float>> extinction;
        std::vector<std::vector<Radiance>> emission;
    };

    void BuildPyramid(const Medium &medium);
    void March(int level);
    void Merge(int level);
    void Gather();

    // Carries light from open probes into the solid ones beside them, so that a
    // surface is lit by what reaches it. Runs only into solids, so it can never
    // add light to open space that the solve did not put there.
    void Spread();

public:
    // The daylight reckoning, for inspection.
    //
    // Exposed because the band it draws under the ground is the thing being
    // tuned, and a picture of the light multiplied over the world cannot say
    // which of the two terms produced any given patch of it.
    float SunDepthAt(int i, int j) const {
        return (i < 0 || j < 0 || i >= cols_ || j >= rows_) ? kUnreachable : sunDepth_[i * rows_ + j];
    }

    Radiance SunlitAt(int i, int j) const {
        return (i < 0 || j < 0 || i >= cols_ || j >= rows_) ? Radiance{} : sunlit_[i * rows_ + j];
    }

    float SolidAt(int i, int j) const {
        return (i < 0 || j < 0 || i >= cols_ || j >= rows_) ? 0.0f : solid_[i * rows_ + j];
    }

private:

    Radiance SkyAt(Vector2 world) const;

    // Height of the ground in the column a world position stands in.
    float SkylineAt(float worldX) const;

    // Share of the daylight held back over that column.
    float CoverAt(float worldX) const;

    // Whether a ray that was never stopped inside the region reaches the sky.
    //
    // Sampled against the skyline along its whole length, which is what makes
    // this answer about the world rather than about the region: a ray climbing
    // a natural shaft passes columns whose ground lies below it and reaches the
    // sky, while one crossing under a hillside does not, whether or not the
    // hillside was inside the region being solved.
    bool ReachesSky(Vector2 from, Vector2 to) const;

    // Interval cascade `level` gathers over, measured from the probe.
    float IntervalStart(int level) const;

    bool Inside(Vector2 world) const;
    int MipIndex(float world, float origin, float spacing, int level) const;
    float ExtinctionAt(int level, Vector2 world) const;
    Radiance EmissionAt(int level, Vector2 world) const;

    Settings settings_{};
    Pyramid pyramid_{};

    std::vector<Cascade> cascades_;

    // Cascade zero collapsed to one value per probe, which is all the screen
    // and the game rules ever ask for.
    std::vector<Radiance> probes_;

    // How much of each probe's own patch is filled by something light cannot
    // pass through. Only these take light from their neighbours.
    std::vector<float> solid_;

    // The medium's skyline and sky cover, kept for the length of the solve.
    std::vector<float> skyline_;
    std::vector<float> cover_;
    std::vector<float> sunReach_;

    // The light each solid probe is drawing from, and how far away it is. Also
    // stands in for the state before the smoothing pass, so that it averages
    // one state rather than its own partial results.
    std::vector<Radiance> previous_;
    std::vector<float> distance_;

    // The daylight reaching a solid probe from straight above it, and how far
    // down it had to come. Kept as members rather than as locals so that the
    // buffers are reused rather than allocated every frame.
    std::vector<Radiance> sunlit_;
    std::vector<float> sunDepth_;

    int cols_       = 0;
    int rows_       = 0;
    float spacing_  = 1.0f;
    Vector2 origin_ = {};

    long rays_ = 0;
};

} // namespace light
