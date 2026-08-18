#pragma once

#include "raylib.h"

// What the frame covers, in the world's own units.
//
// Two lines of arithmetic in a file of their own, and they earn it: nearly
// everything the loop does is priced by this rectangle — the light solves a region
// around it, the water steps one, the ground is painted over it, the wood is grown
// under it — so it is asked for in the loop, in the draw and in every probe. Living
// in main.cpp made it look like the loop's private business, and a probe that wants
// it had to be in main.cpp too.
namespace view {

// The world region the frame covers. Read from the window rather than from the
// configured size, so a resized window shows more of the world instead of the same
// amount of it stretched.
// Divided by the zoom, so that what it describes is the ground the frame covers
// rather than the pixels it is drawn with. Zoomed in, that is less world for the
// same window — which is exactly what everything reading this wants to be told,
// since a chunk off the edge of a zoomed-in view is a chunk nobody has to
// generate, light or grow grass on.
inline Rectangle Bounds(const Camera2D &camera) {
    const float zoom = (camera.zoom > 0.0f) ? camera.zoom : 1.0f;

    const float width  = static_cast<float>(GetScreenWidth()) / zoom;
    const float height = static_cast<float>(GetScreenHeight()) / zoom;

    return {camera.target.x - camera.offset.x / zoom, camera.target.y - camera.offset.y / zoom, width, height};
}

// A rectangle with room around it.
//
// The margin is always the same idea wherever it is used: the band that has to be
// simulated is wider than the band that is seen, because something walking in from
// off screen has to have been there a moment before it is looked at.
inline Rectangle Expand(Rectangle rect, float margin) {
    return {rect.x - margin, rect.y - margin, rect.width + 2.0f * margin, rect.height + 2.0f * margin};
}

} // namespace view
