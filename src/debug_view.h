#pragma once

#include "raylib.h"
#include "world.h"

// Overlays that draw what the world is made of rather than what it looks like.
//
// Both are drawn inside the camera transform, over the world and in its own
// coordinates, because both are about world positions: a chunk border and an
// ore band are at fixed places in the world and have to be read against it.
namespace debug_view {

// Which overlays are on. Held by the caller rather than here, so the state of
// the screen stays in one place instead of hiding in a module.
struct Toggles {
    bool vertices = false; // One mark per lattice sample.
    bool chunks   = false; // Chunk borders, coordinates and what pins them.
    bool layers   = false; // The world height grid and the spawn bands on it.
    bool light    = false; // The probes the light was solved on.

    // Skips the light entirely and draws the world at full brightness.
    //
    // The light is a multiply over the finished frame, so unlit ground is
    // genuinely black and there is nothing to be made out in it. That is the
    // right answer for playing and the wrong one for looking at what was
    // generated, which is what this is for.
    bool unlit = false;

    // The baked plant sprites, at one screen pixel per texel.
    //
    // The one overlay here that is not about world positions and so is drawn
    // outside the camera transform. What it is for is the half of a plant that
    // cannot be judged in place: a tree in the world is thirty texels tall behind
    // a hillside at whatever the light is doing, and whether its notches came out
    // as notches is a question about the image.
    bool atlas = false;

    // Runs the weather far faster than real time.
    //
    // A front takes minutes to cross, which is right to play under and hopeless to
    // work on: finding out what the rain looks like meant waiting for it, or walking
    // several screens to meet it. This is the only toggle here that changes the world
    // rather than how it is drawn, which is why it says so on screen.
    bool fastWeather = false;
};

// How much faster the weather runs while that toggle is on.
inline constexpr float kFastWeather = 40.0f;

// Reads the function keys that switch the overlays.
void ReadToggles(Toggles &toggles);

// Chunk grid: where the borders fall, which chunks are resident, and which of
// them are being held in memory because the noise can no longer reproduce them.
void DrawChunks(const World &world, Rectangle view);

// Height grid: the world's own vertical scale, with the band each generated
// material is confined to marked against it.
//
// The bands are drawn along their true edges, wobble included, rather than at
// the height written in the table. What the table names is a nominal level; a
// vein actually stops where the edge runs, and an overlay that disagreed with
// the world by exactly the amount of the wobble would be worse than none.
void DrawLayers(const World &world, Rectangle view);

// The light, as the solver actually holds it: one mark per probe of cascade
// zero, at full brightness.
//
// The lit world shows what the light does; this shows where it is and how
// coarsely it is known, which is what a rule reading a light level is really
// asking about.
void DrawLight(const World &world, Rectangle view);

} // namespace debug_view
