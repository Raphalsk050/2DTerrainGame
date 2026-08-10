#pragma once

// Project-wide configuration constants.
namespace config {

inline constexpr int kScreenWidth  = 800;
inline constexpr int kScreenHeight = 600;
inline constexpr int kTargetFps    = 60;

// Spacing in pixels between neighbouring grid vertices.
inline constexpr int kResolution = 20;

// A grid spanning N cells in one direction is delimited by N+1 vertices.
inline constexpr int kCols = kScreenWidth / kResolution + 1;
inline constexpr int kRows = kScreenHeight / kResolution + 1;

// Side length of the debug square drawn on each vertex.
inline constexpr float kVertexSize = 8.0f;

// Radius around a vertex within which a click is attributed to it. Half the
// spacing gives each vertex exactly its own neighbourhood.
inline constexpr float kPickRadius = kResolution / 2.0f;

// Shader path, relative to the executable. The working directory is switched to
// the binary's own directory at startup, and the build copies assets there.
inline constexpr const char *kNoiseShaderPath = "assets/noise_filter_shader.fs";

} // namespace config
