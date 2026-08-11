#pragma once

#include "light.h"
#include "raylib.h"

#include <vector>

// The solved light field, on its way to the screen.
//
// The world is drawn at full brightness and then multiplied by this, rather
// than each piece of it being drawn with a colour worked out from the light.
// One pass over the frame darkens terrain, liquid and characters alike, and
// nothing that is added to the world later has to be taught how to be lit.
//
// Multiplying is also what makes unlit ground genuinely black rather than
// merely dim, which is the point: an ore seam is a change in colour, and no
// change in colour survives being multiplied by nothing.
class LightLayer {
public:
    void Unload();

    // Uploads the field as a texture, one texel per probe.
    //
    // The probe grid is far coarser than the screen, and is meant to be: light
    // is smooth except at edges, and letting the hardware interpolate between
    // probes costs nothing and gives a gradient no amount of extra probes would
    // improve on.
    void Update(const light::Field &field);

    // Multiplies the frame by the light. Drawn in world space, so it must be
    // called inside the camera transform.
    void Compose() const;

private:
    Texture2D texture_ = {};

    int cols_ = 0;
    int rows_ = 0;

    Vector2 origin_ = {};
    float spacing_  = 0.2f;

    std::vector<Color> pixels_;
};
