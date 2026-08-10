#pragma once

#include "grid.h"
#include "raylib.h"

#include <vector>

// Procedural terrain generation. This module operates purely on CPU-side data;
// it creates no textures and issues no draw calls.
//
// Sampling is a pure function of world position, so any region can be generated
// on its own and neighbouring regions agree along their shared border without
// exchanging any state.
namespace terrain {

// Span in world pixels over which `frequency` counts features. Fixing it here
// keeps the noise scale independent of how the world is partitioned.
inline constexpr float kFeatureSpan = 1000.0f;

// Tunable parameters of the noise generator.
struct Settings {
    float frequency;  // Number of large features per kFeatureSpan pixels. Low
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
    float threshold;  // Value in [0,1] above which a point counts as ground.
                      // Higher values leave less ground.
    float skyDepth;   // Everything above this world Y is forced empty, so the
                      // world has an open sky rather than starting inside rock.
};

// Continuous noise value in [0,1] at a world position.
float Sample(Vector2 world, const Settings &s);

// Sample thresholded into ground or air.
bool IsSolid(Vector2 world, const Settings &s);

// Fills every vertex of a block from its own world position.
void Fill(Grid &grid, const Settings &s);

// Continuous field over a world region, for inspection and debug rendering.
struct Field {
    int cols = 0;
    int rows = 0;
    std::vector<float> value;

    float At(int i, int j) const { return value[i * rows + j]; }
};

Field Generate(const Settings &s, Vector2 origin, int cols, int rows, int spacing);

// Renders the field as a greyscale image. The caller owns the result and must
// release it with UnloadImage.
Image ToImage(const Field &field);

} // namespace terrain
