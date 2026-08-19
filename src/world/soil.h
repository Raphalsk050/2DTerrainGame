#pragma once

#include "world/element.h"
#include "world/marching_squares.h"
#include "raylib.h"

// How a material of the ground is coloured, texel by texel.
//
// The counterpart of what canopy.cpp does for a leaf, and built the same way for
// the same reasons. Four authored tones become seven drawn ones; the form is
// worked out at the scale of the shape and the texture at the scale of the texel;
// and neither of them knows what time of day it is, because the light multiply
// over the whole frame is what says that.
//
// Nothing here opens a window or reads the world. A painter is a handful of
// numbers and a function of a texel, so the same one serves the game, an offline
// probe, and a test.
namespace soil {

// The seven tones a material is drawn from, interpolated from the four it is
// written with.
struct Ramp {
    Color tone[kElementRamp];
};

Ramp Build(const Color authored[kElementTones]);

// Two colours mixed, `t` of the way from the first to the second.
Color Blend(Color from, Color to, float t);

// The painter for one material.
//
// Held by value and passed to marching_squares::DrawPainted, which calls it once
// per square. It carries the ramp rather than the authored tones so the
// interpolation is done once per material per frame instead of once per texel.
struct Paint {
    Ramp ramp;

    float grain  = 0.55f;
    float patch  = 0.9f;
    float strata = 0.0f;

    // Draws no shaded underside.
    //
    // For a layer that lies on something rather than standing free. A sod is
    // eleven pixels of grass on top of thirty-six of earth: its lower edge is not
    // a face at all, it is a join, and shading it as though the sky were being
    // turned away from draws a dark line along the bottom of every lawn — which
    // reads as an outline round the grass rather than as grass on soil.
    //
    // The lit crest stays. What is dropped is only the half of the form that
    // describes a surface facing away from the light, because there is no such
    // surface here.
    bool bedded = false;

    // Keeps one material's texture from being another's read at the same place,
    // which would put the same speckle on the soil and on the rock beneath it.
    int seed = 0;

    Color operator()(const marching_squares::Texel &texel) const;
};

// The painter a material's own row asks for.
Paint For(const ElementDef &def, int seed);

// Where in the ramp a texel lands, in [0,1], term by term.
//
// Split rather than summed on the way out because the split is the thing worth
// measuring. The rule the plants were tuned against is that the form must carry
// most of the variance: if the texture carries it instead, the result reads as
// disorganised however good the texture is, and the only way to know which has
// happened is to take the variance of each and compare them.
struct Shade {
    float base   = 0.0f;
    float form   = 0.0f; // How near a face, times which way that face points.
    float patch  = 0.0f; // The slow drift.
    float strata = 0.0f; // The bedding.
    float grain  = 0.0f; // The per-texel stipple.

    float Lit() const { return base + form + patch + strata + grain; }

    // Everything that is not form. What has to stay the smaller half.
    float Texture() const { return patch + strata + grain; }
};

Shade Shading(const Paint &paint, const marching_squares::Texel &texel);

// One step of the ramp, in the units Shade is measured in. Every weight in the
// element table is written in steps and converted through this, so a weight can
// be read against the one rule that decides whether a texture is texture: under a
// step it stipples a boundary, over a step it decides which tone a region is.
inline constexpr float kStep = 1.0f / static_cast<float>(kElementRamp);

} // namespace soil
