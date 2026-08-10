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

Field Generate(const Settings &s, int cols, int rows) {
    Field field;
    field.cols = cols;
    field.rows = rows;
    field.value.resize(static_cast<std::size_t>(cols) * rows);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            // Both axes are divided by `cols` so that noise cells stay square.
            // Dividing y by `rows` would stretch the terrain vertically
            // whenever cols differs from rows.
            const float nx = (i + s.offsetX) * s.frequency / cols;
            const float ny = (j + s.offsetY) * s.frequency / cols;

            field.value[i * rows + j] = (Fbm(nx, ny, s) + 1.0f) * 0.5f; // [-1,1] -> [0,1]
        }
    }

    return field;
}

void Apply(const Field &field, const Settings &s, Grid &grid) {
    for (int i = 0; i < grid.Cols(); i++) {
        for (int j = 0; j < grid.Rows(); j++) {
            grid.SetSolid(i, j, field.At(i, j) > s.threshold && j > s.skyRows);
        }
    }
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
