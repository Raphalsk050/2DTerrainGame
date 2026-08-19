#include "core/picture.h"

void DrawPicture(const Picture &picture, Vector2 at, float pixel) {
    for (int row = 0; row < kPictureSide; row++) {
        for (int col = 0; col < kPictureSide; col++) {
            const Color *tone = ToneAt(picture, picture.art[row][col]);
            if (tone == nullptr) continue;

            DrawRectangleV({at.x + static_cast<float>(col) * pixel, at.y + static_cast<float>(row) * pixel},
                           {pixel, pixel}, *tone);
        }
    }
}
