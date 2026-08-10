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

// Shape of a noise field, independent of what it is used for. The terrain and
// each ore vein describe themselves with one of these.
struct NoiseShape {
    float frequency = 4.0f;  // Number of features per kFeatureSpan pixels. Low
                             // values give few broad shapes, high values many
                             // small ones.
    int octaves = 4;         // Layers summed together. 1 is smooth; 4-6 gives a
                             // rugged outline with fine detail on top.
    float lacunarity = 2.0f; // Frequency multiplier per octave.
    float gain       = 0.5f; // Amplitude multiplier per octave.
    float offsetX    = 0.0f; // Sampling offset. Scrolls the field without
    float offsetY    = 0.0f; // changing its shape.
    int seed         = 0;    // Equal seeds yield identical fields.
};

// Continuous noise value in [0,1] at a world position.
float Sample(Vector2 world, const NoiseShape &shape);

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
    float skyDepth;   // World Y above which the field is fully faded out, so
                      // the world has an open sky rather than starting inside
                      // rock.
    float skyFade;    // Depth in pixels over which that fade happens. Zero
                      // would cut the field at a single height and draw a
                      // ruler-straight horizon across the whole world.
};

// Continuous noise value in [0,1] at a world position, before the sky is cut.
float Sample(Vector2 world, const Settings &s);

// Rock density in [0,1]: the sample, forced to zero above skyDepth. This is the
// value stored in the field, so the contour can interpolate through it.
float Density(Vector2 world, const Settings &s);

// Density thresholded into ground or air. The threshold is passed in rather
// than stored here: it belongs to the element the field represents, and keeping
// a second copy alongside the noise settings is what let drawing and collision
// disagree about where the ground was.
bool IsSolid(Vector2 world, const Settings &s, float threshold);

// Fills every sample of a block from its own world position.
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
