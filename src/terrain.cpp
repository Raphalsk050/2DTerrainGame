#include "terrain.h"

// stb_perlin ships inside raylib. Without STB_PERLIN_IMPLEMENTATION this header
// contributes declarations only; the implementation is already compiled into
// libraylib. The include path is set in CMakeLists.txt.
#include "stb_perlin.h"

#include <cstddef>

namespace terrain {
namespace {

// Fractal Brownian motion: sums several octaves of Perlin noise, each with its
// frequency multiplied by `lacunarity` and its amplitude by `gain`. Layering is
// what gives the field its natural appearance; a single octave is featureless.
//
// The result is renormalised to [-1,1] by the accumulated amplitude, which
// keeps contrast independent of the octave count so that `threshold` stays
// meaningful when octaves change.
float Fbm(float x, float y, const Settings &s) {
    float sum          = 0.0f;
    float amplitude    = 1.0f;
    float frequency    = 1.0f;
    float maxAmplitude = 0.0f;

    for (int o = 0; o < s.octaves; o++) {
        // Perturbing the seed per octave decorrelates the layers. Sharing one
        // seed would repeat the same pattern at every scale.
        sum += stb_perlin_noise3_seed(x * frequency, y * frequency, 0.0f, 0, 0, 0, s.seed + o) * amplitude;

        maxAmplitude += amplitude;
        frequency *= s.lacunarity;
        amplitude *= s.gain;
    }

    return (maxAmplitude > 0.0f) ? (sum / maxAmplitude) : 0.0f;
}

} // namespace

float Sample(Vector2 world, const Settings &s) {
    // Both axes use the same divisor so that noise cells stay square.
    const float nx = (world.x + s.offsetX) * s.frequency / kFeatureSpan;
    const float ny = (world.y + s.offsetY) * s.frequency / kFeatureSpan;

    return (Fbm(nx, ny, s) + 1.0f) * 0.5f; // [-1,1] -> [0,1]
}

bool IsSolid(Vector2 world, const Settings &s) {
    if (world.y < s.skyDepth) return false;
    return Sample(world, s) > s.threshold;
}

void Fill(Grid &grid, const Settings &s) {
    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            grid.SetSolid(i, j, IsSolid(grid.PointAt(i, j), s));
        }
    }
}

Field Generate(const Settings &s, Vector2 origin, int cols, int rows, int spacing) {
    Field field;
    field.cols = cols;
    field.rows = rows;
    field.value.resize(static_cast<std::size_t>(cols) * rows);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const Vector2 world = {origin.x + static_cast<float>(i * spacing),
                                   origin.y + static_cast<float>(j * spacing)};

            field.value[i * rows + j] = Sample(world, s);
        }
    }

    return field;
}

Image ToImage(const Field &field) {
    Image image = GenImageColor(field.cols, field.rows, BLACK);

    for (int i = 0; i < field.cols; i++) {
        for (int j = 0; j < field.rows; j++) {
            const auto v = static_cast<unsigned char>(field.At(i, j) * 255.0f);
            ImageDrawPixel(&image, i, j, {v, v, v, 255});
        }
    }

    return image;
}

} // namespace terrain
