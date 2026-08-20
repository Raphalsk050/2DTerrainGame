#include "core/sheet.h"

#include <algorithm>
#include <cmath>

namespace {

// The union of every frame's drawn-on part, in frame-local texels.
//
// Walked over the pixels once at load. It is the only reason the image is opened as an
// image at all rather than handed straight to the graphics device — a few thousand
// pixels once per file, against a picture that would otherwise have to be cut to its
// content by hand before it could be stood on the ground.
void Solid(const Image &picture, int wide, int frames, sheet::Strip &strip) {
    Color *pixels = LoadImageColors(picture);

    if (pixels == nullptr) return;

    int left  = wide;
    int top   = picture.height;
    int right = -1;
    int foot  = -1;

    for (int y = 0; y < picture.height; y++) {
        for (int x = 0; x < picture.width; x++) {
            if (pixels[y * picture.width + x].a == 0) continue;

            // Folded into one frame, so every frame shares the window.
            const int local = x % wide;

            left  = std::min(left, local);
            right = std::max(right, local);
            top   = std::min(top, y);
            foot  = std::max(foot, y);
        }
    }

    UnloadImageColors(pixels);

    // A picture with nothing on it keeps the whole canvas, which is the answer that
    // cannot be wrong: it draws nothing either way, and a zero-wide window would divide
    // by nothing the first time somebody scaled it.
    if (right < left || foot < top) {
        strip.solidWide = wide;
        strip.solidTall = picture.height;

        return;
    }

    strip.solidX    = left;
    strip.solidY    = top;
    strip.solidWide = right - left + 1;
    strip.solidTall = foot - top + 1;

    (void)frames;
}

} // namespace

sheet::Strip sheet::Load(const char *path, int wide) {
    Strip strip;

    if (path == nullptr || wide <= 0) return strip;

    // Opened as an image first, so the drawn-on part can be measured before it goes to
    // the graphics device — see `Solid`.
    Image picture = LoadImage(path);

    if (picture.data == nullptr) return strip;

    const Texture2D texture = LoadTextureFromImage(picture);

    if (texture.id == 0 || (texture.width % wide) != 0) {
        // Not cut by the tool, or cut at a different frame size. Refused rather than
        // drawn part way: a strip whose frames do not divide its width animates as a
        // creature sliding sideways through itself, which reads as a bug in the
        // animation rather than as a bad file.
        if (texture.id != 0) UnloadTexture(texture);

        UnloadImage(picture);

        return strip;
    }

    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    strip.texture = texture;
    strip.wide    = wide;
    strip.tall    = texture.height;
    strip.frames  = texture.width / wide;

    Solid(picture, wide, strip.frames, strip);

    UnloadImage(picture);

    return strip;
}

void sheet::DrawSolid(const Strip &strip, int frame, Vector2 at, float pixel, Color tint) {
    if (!strip.Ready() || strip.solidWide <= 0 || strip.solidTall <= 0) return;

    const int which = ((frame % strip.frames) + strip.frames) % strip.frames;

    const Rectangle source = {static_cast<float>(which * strip.wide + strip.solidX),
                              static_cast<float>(strip.solidY), static_cast<float>(strip.solidWide),
                              static_cast<float>(strip.solidTall)};

    const float wide = strip.solidWide * pixel;
    const float tall = strip.solidTall * pixel;

    // Snapped to the pixel grid, for `Draw`'s reason: the world is drawn in whole pixels
    // and an unsnapped sprite shimmers against the ground it stands on.
    const float left = std::floor((at.x - wide * 0.5f) / pixel) * pixel;
    const float top  = std::floor((at.y - tall) / pixel) * pixel;

    DrawTexturePro(strip.texture, source, {left, top, wide, tall}, {0.0f, 0.0f}, 0.0f, tint);
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
