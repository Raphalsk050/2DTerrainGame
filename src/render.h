#pragma once

#include "debug_view.h"
#include "editor.h"
#include "fixture.h"
#include "grove.h"
#include "inventory.h"
#include "light.h"
#include "liquid_layer.h"
#include "lit_layer.h"
#include "player.h"
#include "raylib.h"
#include "scuff.h"
#include "world.h"

// The order the world is drawn in, and the one place that knows it.
//
// It is an order and not a list: the sky is behind the ground, the ground is behind
// what grows on it, the light multiplies everything except the sky it comes from,
// and the marks a player is meant to *read* rather than see go on the far side of
// that multiply. Every one of those is a decision with a reason, and they were
// spread through the middle of the loop where the reasons could not be read
// together.
//
// The loop's own business is when to draw, not what order to draw in.
namespace render {

// The solved light over what has been drawn, as one multiply.
void ComposeLight(const light::Field &field);

// Everything the light multiplies, into the layer that holds it. Runs between
// LitLayer::Capture and Finish, and so before the frame is opened.
void LitWorld(const World &world, const Grove &grove, const fixture::Fixtures &fixtures, const Player &player,
              const scuff::Trail &trail, const LiquidLayer &liquids, const light::Field &lights,
              const Camera2D &camera, const debug_view::Toggles &debug);

// The world in the frame: the sky, the lit layer over it, and the overlays and the
// cursor over that. Does not open the frame — the caller decides whether it is
// drawing to the screen or to a target.
void Scene(const World &world, const Grove &grove, const Inventory &inventory, const Player &player,
           const Editor &editor, const LitLayer &lit, const Camera2D &camera, const debug_view::Toggles &debug,
           bool aiming);

} // namespace render
