#include "core/figure.h"

#include <cmath>

namespace {

// One tone under a tint, as a multiply. The alpha is the tint's, so a fading
// creature fades whatever it is drawn from.
Color Under(Color tone, Color tint) {
    const auto mix = [](unsigned char a, unsigned char b) {
        return static_cast<unsigned char>((static_cast<int>(a) * static_cast<int>(b)) / 255);
    };

    return {mix(tone.r, tint.r), mix(tone.g, tint.g), mix(tone.b, tint.b), mix(tone.a, tint.a)};
}

} // namespace

void figure::Draw(const Figure &fig, Vector2 at, float pixel, int facing, Color tint) {
    // Bottom-centre, so the anchor is the creature's feet — the same anchor
    // body::Body keeps its position at, which is what makes a drawn mob stand on
    // the ground its collider is resting on.
    //
    // Snapped to the texel grid rather than drawn at the body's true position. The
    // body moves in fractions of a pixel and the world is drawn in whole texels,
    // so an unsnapped figure shimmers against the ground it is standing on — the
    // same argument §17.2e makes about a mountain face, one creature further in.
    const float left = at.x - (fig.width * pixel) / 2.0f;
    const float top  = at.y - fig.height * pixel;

    const float x0 = std::floor(left / pixel) * pixel;
    const float y0 = std::floor(top / pixel) * pixel;

    for (int row = 0; row < fig.height; row++) {
        for (int col = 0; col < fig.width; col++) {
            // Mirrored by reading the row backwards rather than by drawing it
            // twice. A creature facing left is the same creature.
            const int read = (facing >= 0) ? col : (fig.width - 1 - col);

            const Color *tone = ToneAt(fig, fig.art[row][read]);
            if (tone == nullptr) continue;

            DrawRectangleV({x0 + static_cast<float>(col) * pixel, y0 + static_cast<float>(row) * pixel},
                           {pixel, pixel}, Under(*tone, tint));
        }
    }
}
