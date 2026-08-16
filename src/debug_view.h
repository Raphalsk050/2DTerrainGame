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
    bool limits   = false; // Every edge the light solve has — see DrawLightLimits.

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

    // Everything in the world that collides, drawn as the collider rather than as
    // the picture.
    //
    // Nothing else here answers the one question that produces the most
    // convincing bugs: a body, a hitbox and a sprite are three different
    // rectangles worked out by three different pieces of code, and when they
    // disagree what a player sees is not "the box is wrong" but "the thing is in
    // the wrong place". A tree that cannot be chopped where its trunk is drawn
    // reads as a badly positioned tree; a character standing a pixel inside the
    // ground reads as a badly drawn character.
    //
    // Drawing them together is the whole of the fix, because the disagreement is
    // the finding and neither rectangle is wrong on its own.
    bool bodies = false;

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

// Every edge the light solve has, drawn over the world it is lighting.
//
// The overlay for "does this move when I move?", and it exists because that
// question cannot be answered from a picture of the light alone. A solve that is
// correct everywhere can still change what it produces at a fixed world point
// simply by standing somewhere else, and every way it can do so is a boundary:
//
//   - **The region.** The solve covers a rectangle around the view and nothing
//     outside it is lit at all. Its corner is snapped to a stride of cells, so it
//     does not slide with the player, it *jumps* — and the jump is drawn.
//   - **The probe lattices.** Each cascade level stands its probes twice its own
//     step apart, so a region that jumps two cells re-phases every level above the
//     first. The three coarsest are drawn because that is where a re-phasing is
//     large enough to see; the rest are reported as numbers.
//   - **The cloud deck.** The cloud is matter, stamped into the medium between the
//     deck's two edges — and the stamp is clipped to the region. So the deck is only
//     as thick as the part of it that fell inside, and a region whose top edge
//     crosses the deck's changes the optical depth of every cloud on screen at once.
//     Two rectangles: where the deck is, and how much of it was stamped.
//
// There was a per-column *cover* strip here too, for the share of sky a canopy held
// back. It went with the canopy shade itself — see World::AddCover's note, which is
// the whole account of what this overlay was built to find.
//
// What is deliberately *not* here: a cache boundary, because the solve has none. It
// keeps one frame of fluence to feed the bounce and nothing else, and that history
// is reprojected by however far the region walked. Chunk residency is a cache and it
// is on F3.
void DrawLightLimits(const World &world, Rectangle view);

// The surface the world's collision actually presents, column by column, against
// the surface the world is drawn at.
//
// Two lines, and they are meant to be read against each other. `World::IsSolidAt`
// snaps to the nearest lattice vertex, so what a body rests on is a staircase on a
// six pixel grid; the ground is *drawn* on the five pixel texel grid, from a
// contour interpolated between those same vertices. The two are near each other
// everywhere and equal almost nowhere, and every "the character is standing a
// little inside the hill" is one of the places they part.
//
// Nothing here is a judgement about which is right. They are different grids
// answering different questions, and the only thing worth knowing is how far apart
// they get.
void DrawGroundCollision(const World &world, Rectangle view);

} // namespace debug_view
