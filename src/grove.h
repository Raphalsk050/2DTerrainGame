#pragma once

#include "canopy.h"
#include "flora.h"
#include "raylib.h"
#include "terrain.h"
#include "world.h"

#include <vector>

// The plants standing in the world right now.
//
// Everything about a plant that the noise can answer is answered by flora, and
// none of it is kept: this holds the plants under the view for the length of a
// frame and throws them away, the same way the world holds chunks. What it will
// come to own, and the reason it exists as an object at all, is the part the
// noise cannot answer — which tree has been cut, how far a planted one has grown
// — and that is a sparse record of the few the player has touched rather than a
// list of all of them.
//
// It reads the world but never writes to it. A tree stands on the surface and
// stops nothing, so nothing in the lattice has to know it is there.
class Grove {
public:
    // Bakes the sprites as well as settling the placement, so it needs a window
    // open. Costs a few milliseconds, once.
    void Configure(const flora::Settings &settings, const terrain::Settings &terrain);
    void Unload();

    // Grows the plants covering `view` plus the margin a canopy can hang over.
    void Update(const World &world, Rectangle view);

    // Drawn between the terrain and the character, and so before the light is
    // multiplied over the frame: a tree is lit by the same daylight as the ground
    // it stands on, and needs to know nothing about it to be.
    void Draw(flora::Season season) const;

    // The whole sheet, at one screen pixel per texel, for checking that what was
    // drawn is what was meant. Drawn in screen space.
    void DrawSheet() const;

    int VisiblePlants() const { return static_cast<int>(plants_.size()); }
    int DrawnPlants() const { return sheet_.Held(); }

    const std::vector<flora::Plant> &Plants() const { return plants_; }
    const flora::Settings &Settings() const { return settings_; }

private:
    // Fills the surface buffer and points `ground_` at it. The scatter asks about
    // columns either side of what it is placing — the lie of the land, and the
    // footing under a trunk — so the run prepared is wider than the view by more
    // than a canopy.
    void ReadGround(const World &world, Rectangle view);

    // A cache, so drawing from it is a const operation on this object even
    // though it fills itself in as it goes: what a plant looks like is settled
    // by the plant, and the sheet only remembers the answer.
    mutable canopy::Sheet sheet_;

    flora::Settings settings_{};
    terrain::Settings terrain_{};

    std::vector<flora::Plant> plants_;

    // The skyline under one frame's plants. Kept between frames only so that
    // updating does not allocate; nothing in it survives the call that fills it.
    std::vector<float> surface_;
    flora::Ground ground_{};
};
