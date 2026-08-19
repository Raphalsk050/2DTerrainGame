#include "core/sheet.h"

#include <cmath>

sheet::Strip sheet::Load(const char *path, int wide) {
    Strip strip;

    if (path == nullptr || wide <= 0) return strip;

    const Texture2D texture = LoadTexture(path);

    if (texture.id == 0) return strip;

    // Point sampling, always, and it is the same argument CLAUDE.md §5.5 makes about
    // the terrain cache: at one texel per world pixel a screen pixel takes the texel
    // whose square it falls in, which is exactly the square the art drew. Bilinear
    // turns pixel art into a smear at any zoom but one.
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    if ((texture.width % wide) != 0) {
        // Not cut by the tool, or cut at a different frame size. Refused rather than
        // drawn part way: a strip whose frames do not divide its width animates as a
        // creature sliding sideways through itself, which reads as a bug in the
        // animation rather than as a bad file.
        UnloadTexture(texture);

        return strip;
    }

    strip.texture = texture;
    strip.wide    = wide;
    strip.tall    = texture.height;
    strip.frames  = texture.width / wide;

    return strip;
}

void sheet::Draw(const Strip &strip, int frame, Vector2 at, float pixel, int facing, Color tint) {
    if (!strip.Ready()) return;

    const int which = ((frame % strip.frames) + strip.frames) % strip.frames;

    // A negative source width is how raylib mirrors, which saves a second strip and a
    // second thing to keep in step.
    Rectangle source = {static_cast<float>(which * strip.wide), 0.0f, static_cast<float>(strip.wide),
                        static_cast<float>(strip.tall)};

    if (facing < 0) source.width = -source.width;

    const float wide = strip.wide * pixel;
    const float tall = strip.tall * pixel;

    // Snapped to the pixel grid rather than drawn at the body's true position. A body
    // moves in fractions of a pixel and the world is drawn in whole ones, so an
    // unsnapped sprite shimmers against the ground it is standing on — the same
    // argument §17.2e makes about a mountain face.
    const float left = std::floor((at.x - wide * 0.5f) / pixel) * pixel;
    const float top  = std::floor((at.y - tall) / pixel) * pixel;

    DrawTexturePro(strip.texture, source, {left, top, wide, tall}, {0.0f, 0.0f}, 0.0f, tint);
}

void sheet::Unload(Strip &strip) {
    if (strip.texture.id != 0) UnloadTexture(strip.texture);

    strip = {};
}
