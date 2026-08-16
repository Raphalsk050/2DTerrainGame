#pragma once

// The compute shaders that solve the light. See radiance.h for what they are and
// radiance.cpp for the order they run in.
//
// They are string literals rather than files under assets/ on purpose: this is the
// algorithm, not content, and a light solver that silently does nothing because a
// path was wrong at startup is a worse failure than one that will not link.
//
// Everything here follows Freeman, Sannikov & Margel, "Holographic Radiance
// Cascades for 2D Global Illumination" (arXiv:2505.02041), with equation numbers
// quoted where the code implements one. Three things depart from the paper's text,
// each for a reason given where it happens: the direction count in kPrelude, the
// boundary cascade in kBoundary, and the sky in kMergeDown.

namespace radiance {

// ---------------------------------------------------------------------------
// Shared by every kernel: the grid, the rotation, and the way a probe cell turns
// into a place in the world.
//
// THE COORDINATE SYSTEMS, because there are four and confusing two is the only way
// to get this wrong:
//
//   world   - the game's own units. Only the caller sees these.
//   scene   - the medium's cells, uSceneW x uSceneH, one per lattice step. Also the
//             resolution the fluence comes out at.
//   probe   - half of scene, each way. The cascades live here. That halving is what
//             makes the memory affordable, and the resolution it costs is put back
//             by kFinish, which traces the missing half-step for real.
//   cell(n) - probe with x scaled by 2^n: cascade n's own grid. y is never scaled,
//             and that is the entire idea of the method -- spatial resolution is
//             kept across the direction light arrives from, and given up only along
//             it, so a penumbra never falls between two probes.
//
// A cascade at level n has uGX = GX >> n columns, uGY rows and uDirs = 2 << n
// directions, so every level holds the same number of samples.
// ---------------------------------------------------------------------------
const char *const kPrelude = R"GLSL(#version 430

// --- the cascade being written ---------------------------------------------
uniform int uGX;        // columns at this level: GX >> level
uniform int uGY;        // rows; the same at every level
uniform int uDirs;      // directions at this level: 2 << level
uniform int uStep;      // 1 << level: the x scale of a cell at this level

// Where each level's block starts inside the pyramid buffer, in elements. Levels
// are packed end to end because their sizes differ, and a buffer per level would be
// a binding per level.
uniform int uHere;
uniform int uBelow;
uniform int uAbove;

// Both interleaved probe grids share one buffer; this picks one. kFinish says why
// there are two. The two strides differ because the pyramid holds one more ray per
// probe than the cascade holds cones, and the difference is not constant across
// levels -- keeping one uniform for both was a bug that reads the wrong parity's
// data at every level but the first.
uniform int uParity;
uniform int uPyramidStride;
uniform int uRadianceStride;

// Whether last frame's fluence is fed back as a scattering source: 1 for global
// illumination, 0 for direct light alone.
uniform float uBounce;

// --- the medium -------------------------------------------------------------
uniform int uSceneW;
uniform int uSceneH;

// --- the rotation -----------------------------------------------------------
//
// A quadrant is solved by running everything with the world turned a quarter turn,
// four times over. Rather than rotate the medium, the grid is read through this
// basis: uXDir is where the cascade's +x points in scene cells, uYDir is its
// perpendicular, and uAnchor is the scene cell the grid's origin stands on.
uniform ivec2 uXDir;
uniform ivec2 uYDir;
uniform ivec2 uAnchor;

// The scene's extent along uXDir and uYDir. Swapped for the two quarter turns.
uniform int uSpanX;
uniform int uSpanY;

// --- the sky ----------------------------------------------------------------
uniform vec3 uSkyRadiance;
uniform float uSkyHorizon;   // how far up a ray must point before any sky reaches it
uniform float uSkyZenith;    // and where it has all of it
uniform float uSkyCover;     // what the weather is holding back, in [0,1]

const float kPi = 3.14159265358979323846;
const float kTau = 6.28318530717958647692;

// A radiance interval and what it let through: the pair the whole method is written
// in. Paper Eq 9.
struct Fluence {
    vec3 radiance;
    float transmittance;
};

// Paper Eq 7. Identical to premultiplied alpha with transmittance standing for
// 1 - a, which is the observation the entire hierarchy rests on: intervals compose,
// so a long ray is a short one merged with another short one, for ever.
Fluence Over(Fluence near, Fluence far) {
    return Fluence(near.radiance + near.transmittance * far.radiance,
                   near.transmittance * far.transmittance);
}

// Paper Eq 8, for when the far end has already been integrated over its own cone
// and has no transmittance left to give.
vec3 OverRadiance(Fluence near, vec3 far) {
    return near.radiance + near.transmittance * far;
}

Fluence Blend(Fluence a, Fluence b) {
    return Fluence((a.radiance + b.radiance) * 0.5,
                   (a.transmittance + b.transmittance) * 0.5);
}

// Turns a radiance into the angular fluence of a cone of the given arc, normalised
// by the full turn. Every cone of every quadrant then sums to a mean radiance
// rather than an integral, which is what the screen wants and what keeps the four
// quadrants addable: a uniformly lit world comes out at exactly its own brightness.
Fluence Restrict(Fluence value, float arc) {
    return Fluence(value.radiance * (arc / kTau), value.transmittance);
}

// The ray for direction k at a level with `dirs` directions, in that level's cells:
// one column across, and k - dirs/2 rows up or down.
//
// THE FIRST DEPARTURE. The paper has v_n(k) = (2^n, 2k - 2^n), whose y component is
// always even for n >= 1 -- and then observes in section 5.0.3 that this is exactly
// why probes of odd and even y never interact, which is the checkerboard it treats
// afterwards with a blur. The authors' own reference implementation instead carries
// twice the directions at every level, so k - dirs/2 takes every integer and the two
// parities are coupled at the source. That is what is done here. kBlur stays, but it
// is cleaning up a residue rather than covering the artefact.
ivec2 RayOffset(int dirs, int k) {
    return ivec2(1, k - dirs / 2);
}

// The angle a ray leaves at. The x extent is dirs/2 rather than the column width,
// which is what makes the fan span exactly the quadrant's ninety degrees at every
// level.
float RayAngle(int dirs, float k) {
    return atan(k - float(dirs) / 2.0 + 0.5, float(dirs) / 2.0);
}

// Paper Eq 13: the arc of cone i, between the two rays either side of it. Narrower
// at the edges of the quadrant than at its middle, which is why it is computed and
// not assumed.
float ConeArc(int dirs, int i) {
    return RayAngle(dirs, float(i) + 0.5) - RayAngle(dirs, float(i) - 0.5);
}

// Where a probe stands, in scene cells, for a cell at a level whose x scale is
// `step`.
//
// The fractional offsets are not decoration. 0.501 along x keeps a probe off the
// exact boundary between two scene cells, where the walk would have to break a tie;
// parity + 0.499 along y is what separates the two interleaved grids by exactly one
// scene cell, which is how the pair covers a resolution neither covers alone.
vec2 ProbeScene(vec2 cell, int step, int parity) {
    return vec2(uAnchor)
         + vec2(uXDir) * (2.0 * cell.x * float(step) + 0.501)
         + vec2(uYDir) * (2.0 * cell.y + float(parity) + 0.499);
}

// Light from beyond the region, as a function of the direction a ray left in.
//
// This is the paper's footnote 1 -- an environment map read in the direction of
// v_N(i) -- and it is the whole of the daylight. Nothing here knows where the ground
// is: a ray that starts underground is stopped by the rock it is actually in, by the
// same transport that stops it anywhere else. That is what replaces a skyline, a
// per-column cloud cover, and a sunlight depth, and it is why none of them are here.
vec3 SkyRadiance(vec2 dir) {
    float len = length(dir);
    if (len < 1e-6) return vec3(0.0);

    float up = -dir.y / len;    // world y grows downward
    float share = smoothstep(uSkyHorizon, uSkyZenith, up);

    return uSkyRadiance * (share * (1.0 - uSkyCover));
}
)GLSL";

// ---------------------------------------------------------------------------
// The medium, and the source term that carries the bounce.
// ---------------------------------------------------------------------------
const char *const kSource = R"GLSL(
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

struct Material {
    vec4 albedoSigma;   // rgb diffuse albedo, a extinction per scene cell
    vec4 emission;      // rgb radiance emitted per cell of travel
};

layout(std430, binding = 0) readonly buffer Scene { Material bScene[]; };
layout(std430, binding = 1) writeonly buffer Source { vec4 bSource[]; };
layout(std430, binding = 2) readonly buffer Previous { vec4 bPrevious[]; };
layout(std430, binding = 6) writeonly buffer Accum { vec4 bAccum[]; };

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= uSceneW || y >= uSceneH) return;

    int cell = y * uSceneW + x;

    // The four quadrants add into the accumulator, so it has to start at nothing.
    // Done here rather than by a clear of its own because this pass already visits
    // every cell exactly once and the write is free beside the read below.
    bAccum[cell] = vec4(0.0);

    Material material = bScene[cell];
    float sigma = material.albedoSigma.a;

    // Q, the radiance a cell contributes per unit of travel through it:
    //
    //     Q = emitted + sigma * albedo * F / (2 pi)
    //
    // The scattering half is multiplied by sigma deliberately, and that factor is
    // what lets a surface be a dense volume. Integrated along a ray that cannot get
    // through, Q gives Q / sigma, so the sigma cancels and the face returns
    // albedo * F / (2 pi) however dense it was made: a rock face lit by the room in
    // front of it, dark a little further in, with no separate pass and no reach to
    // tune. The emitted half is not multiplied, so a torch still glows in open air
    // where sigma is zero.
    //
    // F is last frame's fluence. One bounce is added per frame and the room settles
    // within a few of them, which is the temporal accumulation the paper describes
    // for multiple bounces.
    //
    // But not this cell's own F, where the cell is dense. A probe inside a solid
    // cannot see out of its own cell -- at sigma 32 the ray is dead within half a
    // cell of where it started -- so a surface reads no incident light, reflects
    // nothing, and comes out black however bright the room in front of it is. That
    // is what left the whole lit hillside dark in the sky test.
    //
    // What a face is lit by is the space it faces, so a dense cell takes its incident
    // light from the open cells around it. This is one neighbourhood and no tuning,
    // which is the difference between it and the sweep it replaces: the old solver
    // pushed light into solids with a chamfer distance transform, over a reach and a
    // lip that both had to be set by hand and were wrong at different depths.
    vec3 incident = bPrevious[cell].rgb;

    if (sigma > 1.0) {
        vec3 outside = vec3(0.0);
        float facing = 0.0;

        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;

                int nx = x + dx;
                int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= uSceneW || ny >= uSceneH) continue;

                int at = ny * uSceneW + nx;
                if (bScene[at].albedoSigma.a > 1.0) continue;

                outside += bPrevious[at].rgb;
                facing += 1.0;
            }
        }

        // No open cell anywhere around it: this is the inside of the rock, and the
        // inside of rock is black. Falling back to its own value would be the leak
        // the whole scheme avoids.
        incident = (facing > 0.0) ? (outside / facing) : vec3(0.0);
    }

    // No second division by the full turn here, and that is worth stating because
    // the physics textbook form has one. Restrict() already normalises every cone by
    // the turn, so what comes out of the solver is a mean radiance and not an
    // integral -- a uniformly lit world reads back at exactly its own brightness,
    // which is what the harness checks first. Dividing again made every surface 2 pi
    // too dark, and what that looked like was a sunlit hillside coming out black
    // while the sky above it was correct.
    vec3 scattered = material.albedoSigma.rgb * sigma * incident * uBounce;

    bSource[cell] = vec4(material.emission.rgb + scattered, sigma);
}
)GLSL";

// ---------------------------------------------------------------------------
// Walking a segment through the medium.
//
// Amanatides & Woo's grid traversal, with each cell's contribution integrated in
// closed form rather than sampled. That closed form is why nothing here needs a step
// size: a cell is homogeneous by construction, so the answer over whatever piece of
// it a ray crosses is exact, at any length and any angle.
// ---------------------------------------------------------------------------
const char *const kTracer = R"GLSL(
layout(std430, binding = 1) readonly buffer SourceRead { vec4 bSourceRead[]; };

Fluence TraceSegment(vec2 from, vec2 to) {
    vec3 gathered = vec3(0.0);
    float through = 1.0;

    vec2 delta = to - from;
    float span = length(delta);
    if (span < 1e-6) return Fluence(gathered, through);

    vec2 dir = delta / span;

    ivec2 cell = ivec2(floor(from));
    ivec2 stepping = ivec2(dir.x >= 0.0 ? 1 : -1, dir.y >= 0.0 ? 1 : -1);

    bool flatX = abs(dir.x) < 1e-12;
    bool flatY = abs(dir.y) < 1e-12;

    // How far along the ray one whole cell of each axis is, and how far to the first
    // crossing. An axis the ray does not move along is given a distance nothing can
    // reach, so it drops out of the min below without a branch.
    vec2 rate = vec2(flatX ? 1e30 : abs(1.0 / dir.x),
                     flatY ? 1e30 : abs(1.0 / dir.y));

    vec2 next;
    next.x = flatX ? 1e30
           : ((dir.x > 0.0 ? (float(cell.x) + 1.0 - from.x) : (from.x - float(cell.x))) * rate.x);
    next.y = flatY ? 1e30
           : ((dir.y > 0.0 ? (float(cell.y) + 1.0 - from.y) : (from.y - float(cell.y))) * rate.y);

    float travelled = 0.0;

    // Bounded rather than while(true). A ray crosses at most one cell boundary per
    // axis per unit of its own length and the region is finite, so this is a ceiling
    // that is never met rather than a cutoff -- but it is here because a compute
    // shader that will not terminate is not a debuggable failure, it is a reset
    // graphics driver.
    for (int guard = 0; guard < 8192; guard++) {
        if (travelled >= span) break;

        float reach = min(min(next.x, next.y), span);
        float dl = reach - travelled;

        // Outside the medium there is nothing to gather and nothing to stop the ray.
        // What lies beyond the region is the sky's, and it is credited once, at the
        // far end of the whole chain, rather than per cell here.
        if (dl > 0.0 && cell.x >= 0 && cell.y >= 0 && cell.x < uSceneW && cell.y < uSceneH) {
            vec4 source = bSourceRead[cell.y * uSceneW + cell.x];
            float sigma = source.a;

            float taken = 1.0 - exp(-sigma * dl);

            // L = Q (1 - e^-sigma dl) / sigma, and its limit Q dl as sigma goes to
            // zero. A select rather than a branch, so both sides cost the same in a
            // warp holding solid and open cells at once -- which every warp near a
            // surface does, and near a surface is where the frame is spent.
            float weight = (sigma > 1e-6) ? (taken / sigma) : dl;

            gathered += through * source.rgb * weight;
            through *= 1.0 - taken;

            if (through < 1e-4) return Fluence(gathered, 0.0);
        }

        travelled = reach;

        // Step across whichever boundary came first, and across both when the ray
        // goes exactly through a corner.
        if (next.x < next.y) {
            cell.x += stepping.x;
            next.x += rate.x;
        } else if (next.y < next.x) {
            cell.y += stepping.y;
            next.y += rate.y;
        } else {
            cell += stepping;
            next += rate;
        }
    }

    return Fluence(gathered, through);
}
)GLSL";

// ---------------------------------------------------------------------------
// The acceleration structure, T.
//
// T_n(p, k) is Trace(p, p + v_n(k)) -- one ray, one column of cascade n across.
// Levels 0..2 are traced for real; every level above is two rays of the level below
// joined end to end. That is what makes the cost independent of how far light is
// allowed to travel, and it is also why this handles a scene that is opaque
// everywhere at the same price as an empty one: there is no empty space to skip and
// nothing is trying to.
// ---------------------------------------------------------------------------
const char *const kPyramid = R"GLSL(
layout(std430, binding = 3) buffer Pyramid { vec4 bPyramid[]; };

// Row innermost, so neighbouring threads touch neighbouring elements: the dispatch
// puts the row on x for exactly this reason.
int PyramidIndex(int base, int gy, int dirs, ivec2 cell, int k) {
    return base + (cell.x * (dirs + 1) + k) * gy + cell.y;
}

void PyramidStore(int base, int gy, int dirs, ivec2 cell, int k, Fluence value) {
    bPyramid[PyramidIndex(base, gy, dirs, cell, k)] = vec4(value.radiance, value.transmittance);
}

Fluence PyramidLoad(int base, int gy, int dirs, ivec2 cell, int k) {
    vec4 raw = bPyramid[PyramidIndex(base, gy, dirs, cell, k)];
    return Fluence(raw.rgb, raw.a);
}

// A read that may fall off the grid. Empty rather than opaque: there is no medium
// out there to stop anything, and the light that does arrive from beyond is the
// sky's, credited where the cone ends.
Fluence PyramidLoadSafe(int base, int gx, int gy, int dirs, ivec2 cell, int k) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= gx || cell.y >= gy) {
        return Fluence(vec3(0.0), 1.0);
    }
    return PyramidLoad(base, gy, dirs, cell, k);
}
)GLSL";

// Levels 0..2 of T: real rays.
//
// The paper traces these instead of merging them because the error in joining two
// rays at an angle between them is worst when the rays are shortest, and these are
// the shortest there are. It is about nineteen rays per output cell, none longer
// than six cells.
const char *const kTrace = R"GLSL(
layout(local_size_x = 32, local_size_y = 2, local_size_z = 2) in;

void main() {
    int cy = int(gl_GlobalInvocationID.x);
    int k  = int(gl_GlobalInvocationID.y);
    int cx = int(gl_GlobalInvocationID.z);

    if (cy >= uGY || k > uDirs || cx >= uGX) return;

    vec2 from = ProbeScene(vec2(cx, cy), uStep, uParity);

    ivec2 endCell = ivec2(cx, cy) + RayOffset(uDirs, k);
    vec2 to = ProbeScene(vec2(endCell), uStep, uParity);

    // A ray whose far end leaves the grid keeps whatever it had left. The reference
    // implementation stops such a ray dead, and that is right *there* for a reason
    // that does not hold here: it packs all eight sub-grids end to end down one
    // buffer, so a row index past the end is another quadrant's data rather than
    // nothing, and it has to be fenced off. The parities here are separated by an
    // offset and the row bound is the region's own, so there is nothing to fence --
    // and stopping the ray would throw away every ray that leaves through the top of
    // the region, which is most of them at the coarse levels and is exactly where
    // the daylight comes in. Measured: it cost a fifth of the sky.
    Fluence value = TraceSegment(from, to);

    PyramidStore(uHere + uParity * uPyramidStride, uGY, uDirs, ivec2(cx, cy), k, value);
}
)GLSL";

// Levels 3..N of T: two rays of the level below, joined.
//
// Paper Eq 18 for an even direction, where the halves line up exactly and the join
// loses nothing. Eq 19 and 20 for an odd one, where they do not: the answer is the
// mean of going out along the upper edge and back along the lower, and the other way
// round. Averaging the two orders is what stops the error leaning to one side of the
// cone and compounding all the way up the stack.
const char *const kMergeUp = R"GLSL(
layout(local_size_x = 32, local_size_y = 2, local_size_z = 2) in;

void main() {
    int cy = int(gl_GlobalInvocationID.x);
    int k  = int(gl_GlobalInvocationID.y);
    int cx = int(gl_GlobalInvocationID.z);

    if (cy >= uGY || k > uDirs || cx >= uGX) return;

    int here  = uHere + uParity * uPyramidStride;
    int below = uBelow + uParity * uPyramidStride;

    // The level below has twice the columns and half the directions.
    int lowGX   = uGX * 2;
    int lowDirs = uDirs / 2;
    ivec2 lowCell = ivec2(cx * 2, cy);

    int offset = k - uDirs / 2;   // this ray's rise, in rows

    Fluence value;

    if ((k & 1) == 0) {
        // Eq 18. Direction k/2 below, twice: out to the midpoint, and on from there.
        int lowK = k / 2;
        ivec2 mid = lowCell + ivec2(1, offset / 2);

        value = Over(PyramidLoad(below, uGY, lowDirs, lowCell, lowK),
                     PyramidLoadSafe(below, lowGX, uGY, lowDirs, mid, lowK));
    } else {
        // Eq 19 and 20. k/2 is not a whole direction below, so the ray is
        // approximated by the two straddling it, taken in both orders and averaged.
        int lowerK = k / 2;
        int upperK = k / 2 + 1;

        float rise = float(offset) * 0.5;
        ivec2 midLower = lowCell + ivec2(1, int(floor(rise)));
        ivec2 midUpper = lowCell + ivec2(1, int(ceil(rise)));

        Fluence a = Over(PyramidLoad(below, uGY, lowDirs, lowCell, lowerK),
                         PyramidLoadSafe(below, lowGX, uGY, lowDirs, midLower, upperK));
        Fluence b = Over(PyramidLoad(below, uGY, lowDirs, lowCell, upperK),
                         PyramidLoadSafe(below, lowGX, uGY, lowDirs, midUpper, lowerK));

        value = Blend(a, b);
    }

    PyramidStore(here, uGY, uDirs, ivec2(cx, cy), k, value);
}
)GLSL";

// ---------------------------------------------------------------------------
// The cascades of angular fluence, R.
// ---------------------------------------------------------------------------
const char *const kRadiance = R"GLSL(
layout(std430, binding = 4) buffer RadianceOut { vec4 bRadianceOut[]; };
layout(std430, binding = 5) readonly buffer RadianceIn { vec4 bRadianceIn[]; };

int RadianceIndex(int base, int gy, int dirs, ivec2 cell, int d) {
    return base + (cell.x * dirs + d) * gy + cell.y;
}

void RadianceStore(int base, int gy, int dirs, ivec2 cell, int d, vec3 value) {
    bRadianceOut[RadianceIndex(base, gy, dirs, cell, d)] = vec4(value, 0.0);
}
)GLSL";

// The top of the stack, and the only place light from outside the region gets in
// through the near edge.
//
// THE SECOND DEPARTURE. The paper says to treat R_N as uniformly zero, because there
// are no lights beyond the grid. That is right about lights and wrong about the
// boundary: at level N-1 the even-column case reads R_N at its own position, which
// is *inside* the grid, and zeroing it halves that estimate along the near edge of
// every quadrant -- a quarter of the screen, in a band. So R_N is computed here
// instead, out of the ray that T_N already holds, with whatever that ray had left
// when it reached the far side crediting the sky. Exact where the paper is
// approximate, for one small kernel.
const char *const kBoundary = R"GLSL(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main() {
    int cy = int(gl_GlobalInvocationID.x);
    int d  = int(gl_GlobalInvocationID.y);

    if (cy >= uGY || d >= uDirs) return;

    int here = uHere + uParity * uPyramidStride;

    // Cone d lies between rays d and d + 1, and T_N holds both already.
    Fluence lower = PyramidLoad(here, uGY, uDirs, ivec2(0, cy), d);
    Fluence upper = PyramidLoad(here, uGY, uDirs, ivec2(0, cy), d + 1);

    vec2 at      = ProbeScene(vec2(0.0, float(cy)), uStep, uParity);
    vec2 lowerTo = ProbeScene(vec2(ivec2(0, cy) + RayOffset(uDirs, d)), uStep, uParity);
    vec2 upperTo = ProbeScene(vec2(ivec2(0, cy) + RayOffset(uDirs, d + 1)), uStep, uParity);

    vec3 value = OverRadiance(lower, SkyRadiance(lowerTo - at))
               + OverRadiance(upper, SkyRadiance(upperTo - at));

    // The mean of the two edges, taken over the cone they bound.
    value *= 0.5 * (ConeArc(uDirs, d) / kTau);

    RadianceStore(uParity * uRadianceStride, uGY, uDirs, ivec2(0, cy), d, value);
}
)GLSL";

// Levels N-1 down to 0.
//
// Paper Eq 14 for an odd column, where the level above has nothing at this position
// and the two half cones are traced out to where it does. Eq 15 to 17 for an even
// one, where it does -- and where taking that value alone would be the obvious thing
// and is wrong. The paper's reason is worth keeping in view: the odd-column estimate
// leans towards the edges of its cone, so joining two of them leans towards the
// middle, and nothing downstream can take that back out. Averaging the value in
// place against a second estimate traced two columns further is what cancels it.
//
// THE THIRD DEPARTURE, and it is the sky again. A cone that runs off the far side of
// the grid reads nothing there, and the paper leaves it at nothing. Here it reads
// the sky in its own direction, so daylight enters through whichever edge a ray
// actually leaves by. Every ray is credited the sky exactly once, wherever it gets
// out; a ray still inside the region is not credited at all, because the rock it is
// in stopped it.
const char *const kMergeDown = R"GLSL(
layout(local_size_x = 32, local_size_y = 2, local_size_z = 2) in;

vec3 AboveOrSky(int base, int gx, int gy, int dirs, ivec2 cell, int d, ivec2 from, int step) {
    if (cell.x >= 0 && cell.y >= 0 && cell.x < gx && cell.y < gy) {
        return bRadianceIn[RadianceIndex(base, gy, dirs, cell, d)].rgb;
    }

    // Off the grid. The cone's own direction decides what it sees out there.
    vec2 at = ProbeScene(vec2(from), step, uParity);
    vec2 to = ProbeScene(vec2(from + RayOffset(dirs, d)), step, uParity);

    return SkyRadiance(to - at) * (ConeArc(dirs, d) / kTau);
}

void main() {
    int cy = int(gl_GlobalInvocationID.x);
    int d  = int(gl_GlobalInvocationID.y);
    int cx = int(gl_GlobalInvocationID.z);

    if (cy >= uGY || d >= uDirs || cx >= uGX) return;

    int here   = uHere + uParity * uPyramidStride;
    int above  = uAbove + uParity * uPyramidStride;
    int upBase = uParity * uRadianceStride;

    int upDirs = uDirs * 2;
    int upGX   = uGX / 2;
    int upStep = uStep * 2;

    // The two halves of this cone are cones d*2 and d*2+1 of the level above.
    int lowerD = d * 2;
    int upperD = d * 2 + 1;

    bool even = (cx & 1) == 0;
    int reach = even ? 2 : 1;

    ivec2 lowerAt = ivec2(cx, cy) + ivec2(1, d - uDirs / 2) * reach;
    ivec2 upperAt = ivec2(cx, cy) + ivec2(1, d - uDirs / 2 + 1) * reach;

    // The near segment. An odd column reads its own level's rays; an even one has to
    // reach twice as far, and a ray of twice the length is a ray of the level above.
    Fluence lower;
    Fluence upper;

    if (even) {
        lower = PyramidLoad(above, uGY, upDirs, ivec2(cx / 2, cy), lowerD);
        upper = PyramidLoad(above, uGY, upDirs, ivec2(cx / 2, cy), (d + 1) * 2);
    } else {
        lower = PyramidLoad(here, uGY, uDirs, ivec2(cx, cy), d);
        upper = PyramidLoad(here, uGY, uDirs, ivec2(cx, cy), d + 1);
    }

    // These are the level above's cones, so they take the level above's arcs.
    lower = Restrict(lower, ConeArc(upDirs, lowerD));
    upper = Restrict(upper, ConeArc(upDirs, upperD));

    ivec2 lowerUp = ivec2(lowerAt.x / 2, lowerAt.y);
    ivec2 upperUp = ivec2(upperAt.x / 2, upperAt.y);

    vec3 nextLower = OverRadiance(lower,
        AboveOrSky(upBase, upGX, uGY, upDirs, lowerUp, lowerD, lowerUp, upStep));
    vec3 nextUpper = OverRadiance(upper,
        AboveOrSky(upBase, upGX, uGY, upDirs, upperUp, upperD, upperUp, upStep));

    vec3 value;

    if (even) {
        ivec2 selfUp = ivec2(cx / 2, cy);

        vec3 hereLower = AboveOrSky(upBase, upGX, uGY, upDirs, selfUp, lowerD, selfUp, upStep);
        vec3 hereUpper = AboveOrSky(upBase, upGX, uGY, upDirs, selfUp, upperD, selfUp, upStep);

        value = (hereLower + nextLower) * 0.5 + (hereUpper + nextUpper) * 0.5;
    } else {
        value = nextLower + nextUpper;
    }

    RadianceStore(uParity * uRadianceStride, uGY, uDirs, ivec2(cx, cy), d, value);
}
)GLSL";

// ---------------------------------------------------------------------------
// From the probe grid back to the scene.
//
// The cascades run at half the scene's resolution; this is the step that puts the
// other half back, and it does it by tracing the missing half-cell for real rather
// than interpolating it. The paper's results section names this as how the method is
// meant to be used in a renderer -- "interpolated from these probes onto a screen 2x
// the size, merging with rays traced to the probes to avoid light leaks" -- and it
// is what keeps a shadow edge on the scene's own grid instead of the probes'.
//
// Two interleaved probe grids exist because one cannot serve both parities of a
// scene row: a probe covers two cells and the half-step lands differently depending
// on whether the cell it is going to is odd or even. Between the two grids and the
// two column cases, all four combinations of (column, row) parity are covered, each
// by a probe that actually stands where it needs to.
// ---------------------------------------------------------------------------
const char *const kFinish = R"GLSL(
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(std430, binding = 6) buffer Accum { vec4 bAccum[]; };

// Cone d of R_0, or the sky when the cone starts outside the region -- the same
// rule the merge down follows, for the same reason. Returning nothing here instead
// would put a dark rim round every edge of the lit region, which moves with the
// camera and so cannot be mistaken for anything in the world.
vec3 LoadR0(int parity, ivec2 cell, int d) {
    if (cell.x >= 0 && cell.y >= 0 && cell.x < uGX && cell.y < uGY) {
        return bRadianceIn[RadianceIndex(parity * uRadianceStride, uGY, uDirs, cell, d)].rgb;
    }

    vec2 at = ProbeScene(vec2(cell), 1, parity);
    vec2 to = ProbeScene(vec2(cell + RayOffset(uDirs, d)), 1, parity);

    return SkyRadiance(to - at) * (ConeArc(uDirs, d) / kTau);
}

void main() {
    int gxs = int(gl_GlobalInvocationID.x);   // along uXDir, in scene cells
    int gys = int(gl_GlobalInvocationID.y);   // along uYDir, in scene cells

    if (gxs >= uSpanX || gys >= uSpanY) return;

    ivec2 sceneCell = uAnchor + uXDir * gxs + uYDir * gys;

    // The paper's R_0([x + 1, y], 0). Without the step, the diagonal rays of
    // neighbouring quadrants overlap and every 45 degree line comes out brighter.
    int ax = gxs + 1;
    if (ax >= uSpanX) return;

    int parity = ((ax & 1) != (gys & 1)) ? 1 : 0;

    int cx = ax >> 1;
    int cy = (gys >> 1) - ((parity == 1 && (gys & 1) == 0) ? 1 : 0);

    bool even = (ax & 1) == 0;

    // A probe grid cell is two scene cells; a half-step is therefore half a probe
    // cell, and where it starts depends on which column case this is.
    vec2 cellf = vec2(cx, cy) + (even ? vec2(0.0) : vec2(0.5));

    // Backed off by just under half a cell. The probe's own centre is half a cell
    // behind where the index says, and starting exactly on the boundary opens a
    // two-pixel gap along every shadow edge.
    vec2 from = ProbeScene(cellf - vec2(0.49, 0.0), 1, parity);

    float factor = even ? 2.0 : 1.0;

    ivec2 lowerEnd = ivec2(floor(cellf + vec2(0.5, -0.5) * factor));
    ivec2 upperEnd = ivec2(floor(cellf + vec2(0.5, 0.5) * factor));

    // A segment that runs out of the region keeps what it had left; LoadR0 gives it
    // the sky. See kTrace for why this is not fenced off.
    Fluence lower = TraceSegment(from, ProbeScene(vec2(lowerEnd), 1, parity));
    Fluence upper = TraceSegment(from, ProbeScene(vec2(upperEnd), 1, parity));

    // Half a quadrant each, and four quadrants: the eight of them sum to the whole
    // turn, so a uniformly lit world reads back at exactly its own brightness.
    lower = Restrict(lower, kPi / 4.0);
    upper = Restrict(upper, kPi / 4.0);

    ivec2 lowerOff = ivec2(1, even ? -1 : 0);
    ivec2 upperOff = ivec2(1, 1);

    vec3 nextLower = OverRadiance(lower, LoadR0(parity, ivec2(cx, cy) + lowerOff, 0));
    vec3 nextUpper = OverRadiance(upper, LoadR0(parity, ivec2(cx, cy) + upperOff, 1));

    vec3 value;

    if (even) {
        value = (LoadR0(parity, ivec2(cx, cy), 0) + nextLower) * 0.5
              + (LoadR0(parity, ivec2(cx, cy), 1) + nextUpper) * 0.5;
    } else {
        value = nextLower + nextUpper;
    }

    // One quadrant of four. The rotations run one after another into the same
    // accumulator, so this adds rather than assigns.
    bAccum[sceneCell.y * uSceneW + sceneCell.x] += vec4(value, 0.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Paper Eq 21: the one-pixel cross blur.
//
// The paper adds this to treat the checkerboard, and here it is doing something
// else, which is worth writing down because it changes what may be traded away.
//
// Measured on the hard-shadow scene, over a lit region that ought to be smooth, as
// the mean second difference along each axis -- second, because that reads through a
// gradient and leaves only the wiggle:
//
//     cross blur      across rows    across cols     ratio
//     off                0.024829       0.024420     1.017
//     on                 0.003149       0.003471     0.907
//
// The two axes agree to within two per cent with the blur switched off. There is no
// checkerboard: the doubled direction count in kPrelude removed it at the source,
// which is what it was taken from the reference implementation to do. What the blur
// is actually taking out is the *other* artefact the paper names -- the Moire from
// ray segments being at fixed positions, which is isotropic -- and it takes out
// about eight ninths of it. So it stays, but as an anti-aliasing filter rather than
// as a patch over a directional fault, and anything that claims to replace it has to
// be judged on that.
//
// The guard is the paper's: a neighbour whose opacity is far from this cell's is not
// averaged in, so the blur never carries light across a wall it could not otherwise
// cross -- which would give back the leaking the whole method avoids.
// ---------------------------------------------------------------------------
const char *const kBlur = R"GLSL(
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

struct Material {
    vec4 albedoSigma;
    vec4 emission;
};

layout(std430, binding = 0) readonly buffer Scene { Material bScene[]; };
layout(std430, binding = 6) readonly buffer Accum { vec4 bAccum[]; };
layout(std430, binding = 7) writeonly buffer Final { vec4 bFinal[]; };

// The exposed field, for the screen. Written here rather than by a later pass so
// the tone curve is applied once, on the GPU, to the value that was just computed --
// the old solver spent a whole pass walking the grid on the CPU to do this.
layout(rgba8, binding = 0) uniform writeonly image2D uScreen;

uniform float uOpacityGuard;
uniform float uExposure;
uniform float uBlur;   // 1 to blur, 0 to pass the field through untouched

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= uSceneW || y >= uSceneH) return;

    int cell = y * uSceneW + x;

    float mine = bScene[cell].albedoSigma.a;

    vec3 total = bAccum[cell].rgb * 4.0;
    float weight = 4.0;

    ivec2 around[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));

    for (int n = 0; n < 4; n++) {
        int nx = x + around[n].x;
        int ny = y + around[n].y;
        if (nx < 0 || ny < 0 || nx >= uSceneW || ny >= uSceneH) continue;

        int at = ny * uSceneW + nx;
        if (abs(bScene[at].albedoSigma.a - mine) > uOpacityGuard) continue;

        total += bAccum[at].rgb;
        weight += 1.0;
    }

    vec3 fluence = mix(bAccum[cell].rgb, total / weight, uBlur);

    // Kept linear for the bounce and for the game rules, which both want the real
    // quantity and not a picture of it.
    bFinal[cell] = vec4(fluence, 1.0);

    // Each channel exposed on its own rather than the brightness as a whole, so a
    // torch stays warm where it is strong and washes towards white only where it is
    // overwhelming, the way a real light does.
    vec3 shown = vec3(1.0) - exp(-fluence * uExposure);

    imageStore(uScreen, ivec2(x, y), vec4(shown, 1.0));
}
)GLSL";

} // namespace radiance
