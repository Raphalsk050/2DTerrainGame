#pragma once

#include "grid.h"
#include "raylib.h"

#include <vector>

// Procedural terrain generation. This module operates purely on CPU-side data;
// it creates no textures and issues no draw calls.
namespace terrain {

// Tunable parameters of the noise generator.
struct Settings {
    float frequency;  // Number of large features across the map width. Low
                      // values give few broad shapes, high values many small
                      // ones.
    int octaves;      // Noise layers summed together. 1 is smooth; 4-6 produces
                      // a rugged contour with fine detail on top.
    float lacunarity; // Frequency multiplier per octave. Typically around 2.0.
    float gain;       // Amplitude multiplier per octave. Below 0.5 the fine
                      // detail nearly vanishes; above it, detail dominates.
    float offsetX;    // Sampling offset. Scrolls the map without changing the
    float offsetY;    // shape of the terrain.
    int seed;         // Equal seeds yield identical terrain.
    float threshold;  // Value in [0,1] above which a vertex counts as ground.
                      // Higher values leave less ground.
    int skyRows;      // Number of rows at the top forced to stay empty.
};

// Continuous scalar field in [0,1], sampled once per grid vertex, before the
// threshold is applied.
//
// Exposing it separately allows the same noise to be reused with different
// thresholds without regenerating, inspected or rendered for debugging, and
// interpolated along cell edges rather than always cut at the midpoint.
struct Field {
    int cols = 0;
    int rows = 0;
    std::vector<float> value;

    float At(int i, int j) const { return value[i * rows + j]; }
};

// Evaluates fractal Brownian motion noise at one point per grid vertex.
Field Generate(const Settings &s, int cols, int rows);

// Thresholds the field into the grid's inside/outside state.
void Apply(const Field &field, const Settings &s, Grid &grid);

// Renders the field as a greyscale image. The caller owns the result and must
// release it with UnloadImage.
Image ToImage(const Field &field);

} // namespace terrain
