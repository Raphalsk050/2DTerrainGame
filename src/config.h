#pragma once

#include "raylib.h"

// Project-wide configuration constants.
namespace config {

inline constexpr int kScreenWidth  = 1000;
inline constexpr int kScreenHeight = 600;
inline constexpr int kTargetFps    = 60;

// Spacing in pixels between neighbouring grid vertices.
inline constexpr int kResolution = 6;

// A grid spanning N cells in one direction is delimited by N+1 vertices.
inline constexpr int kCols = kScreenWidth / kResolution + 1;
inline constexpr int kRows = kScreenHeight / kResolution + 1;

// Side length of the debug square drawn on each vertex.
inline constexpr float kVertexSize = 2.0f;

// Spacing of the world height grid, in pixels. It is the scale the spawn bands
// in the element table are written against: a band reads as a range of heights,
// and this is the ruler those heights are counted on.
inline constexpr int kHeightGridStep = 64;

// Interval at which a height line is drawn brighter and labelled. Labelling
// every line at the fine step would leave the screen unreadable.
inline constexpr int kHeightGridMajor = 256;

// Radius around a vertex within which a click is attributed to it. Half the
// spacing gives each vertex exactly its own neighbourhood.
inline constexpr float kPickRadius = kResolution / 2.0f;

// Transparency of the liquid layer, applied once to the whole layer instead of
// to each piece drawn into it.
inline constexpr unsigned char kLiquidAlpha = 170;

// Draws the light as one flat block per probe rather than blending between
// them.
//
// It is a look, and it is also the more honest of the two: a probe knows the
// light over its own patch and nothing about the patch next door, and blending
// between them carries the brightness of a lit wall a full probe into the cave
// beside it, and the darkness of the cave back into the wall. Blocks keep each
// answer inside the square it was solved for.
inline constexpr bool kBlockyLight = true;

// Draws the world as square pixels rather than as smooth polygons.
//
// It changes only how the field is rasterised, not what the field says, so the
// shape is the same one the contour would have drawn. Nothing else on screen is
// touched: the character keeps whatever resolution its own art has, which is
// the reason for doing it here instead of rendering the whole frame small and
// blowing it back up.
inline constexpr bool kPixelArt = true;

// Side of one square, in world units. A divisor of kResolution keeps the
// squares lined up with the lattice the field is sampled on.
inline constexpr float kPixelSize = 5.0f;

// Outlines every material along its own boundary.
//
// Off, a material is only its fill, and what separates it from what is beside
// it is the change of colour alone. Under pixel rasterisation this also saves
// the four extra samples a square spends finding out whether it is exposed, so
// turning it off is cheaper as well as plainer.
inline constexpr bool kDrawContours = false;

// The lantern the player carries.
//
// Strength is in the same unit the sky and a torch are measured in, so it can
// be read as a fraction of either. Deliberately low: enough to place a foot,
// not enough to survey a cavern, which is what keeps an ore seam something to
// be found rather than something visible from the mouth of the tunnel.
//
// Adjustable while the game runs, with the keys below, so the balance can be
// settled by walking around at each setting rather than by argument.
inline constexpr float kLanternStrength = 2.0f;
inline constexpr float kLanternRadius   = 10.0f;
inline constexpr Color kLanternGlow     = {255, 206, 150, 255};

// How much one press changes it, and how far it can be pushed either way.
inline constexpr float kLanternStep = 0.01f;
inline constexpr float kLanternMax  = 8.0f;

// Shader path, relative to the executable. The working directory is switched to
// the binary's own directory at startup, and the build copies assets there.
inline constexpr const char *kNoiseShaderPath = "assets/noise_filter_shader.fs";

} // namespace config
