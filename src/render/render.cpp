#include "render/render.h"

#include "core/config.h"
#include "core/profile.h"
#include "flora/flora.h"
#include "rlgl.h"
#include "world/sod.h"
#include "core/view.h"

#include <cmath>

// The solved light, multiplied over the frame.
//
// There is no texture to keep and no pixel loop to run: the solver exposes the field
// on the GPU as the last thing it does, so this is one blit. What the old layer did
// here -- walk a grid of probes, expose each channel, dither, and upload -- was a
// millisecond and a half of the frame and is simply gone.
void render::ComposeLight(const light::Field &field) {
    const Texture2D texture = field.Screen();
    if (texture.id == 0) return;

    const Rectangle source = {0.0f, 0.0f, static_cast<float>(field.Cols()),
                              static_cast<float>(field.Rows())};

    // A cell sits at the centre of its own texel, so the picture covers the grid's
    // whole extent rather than the span between the first and last cell.
    const Rectangle target = {field.Origin().x, field.Origin().y,
                              field.Cols() * field.Spacing(), field.Rows() * field.Spacing()};

    // Spelled out rather than taken from BLEND_MULTIPLIED, because what this has
    // to do to the alpha is not what it does to the colour and the two are only
    // accidentally the same here. Drawn into LitLayer, the alpha channel is how
    // much of the sky each pixel covers, and dimming it would open the ground up
    // and let the blue through wherever the light was low. Alpha therefore keeps
    // what is already in the target, whatever the multiply does to the colour.
    //
    // BLEND_MULTIPLIED happens to leave it alone too -- it is
    // (RL_DST_COLOR, RL_ONE_MINUS_SRC_ALPHA), and the field's own alpha is 1
    // everywhere, so the destination survives -- but that rests on a constant in
    // light_shaders.h that nothing here is entitled to assume.
    rlSetBlendFactorsSeparate(RL_DST_COLOR, RL_ZERO,   // colour: multiply
                              RL_ZERO,      RL_ONE,    // alpha:  keep
                              RL_FUNC_ADD,  RL_FUNC_ADD);

    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    DrawTexturePro(texture, source, target, {0.0f, 0.0f}, 0.0f, WHITE);
    EndBlendMode();
}

// Everything in the world that the light multiplies: the cloud standing in the
// air, the ground, what grows on it, what walks on it and the weather in front
// of it -- and then the multiply itself, which is the last thing this draws.
//
// The one thing left out is the atmosphere behind it all. See lit_layer.h for
// why, and for why the clouds are not left out with it.
//
// Called between LitLayer::Capture and LitLayer::Finish, so it runs before
// BeginDrawing like every other capture in the loop.
void render::LitWorld(const World &world, const Grove &grove, const fixture::Fixtures &fixtures,
                      const mob::Herd &herd, const Player &player, const Stack &held, const scuff::Trail &trail,
                      const LiquidLayer &liquids, const light::Field &lights, const Camera2D &camera,
                      const debug_view::Toggles &debug) {
    const Rectangle view = view::Bounds(camera);

    BeginMode2D(camera);

    // What is behind the ground, which is the only part of the sky that belongs on
    // this side of the multiply. See World::DrawUnderground.
    {
        PROFILE_ZONE("DrawUnderground");

        world.DrawUnderground(view);
    }

    // It erases with a blend of its own and leaves BLEND_ALPHA behind it, which is
    // not the one the layer is built with.
    LitLayer::Blend();

    // The cloud standing in the air first, and then the ground over it. Underground
    // the band is out of view and this returns having done nothing.
    {
        PROFILE_ZONE("DrawClouds");

        world.Sky().DrawClouds(view, world.Spacing());
    }

    {
        PROFILE_ZONE("DrawTerrain");

        world.DrawTerrain(view);
    }

    // The tufts on top of the band the terrain drew, and under everything that
    // stands in them: a trunk belongs in front of the grass around its own foot,
    // which is the reason the ferns are drawn before the trees as well.
    //
    // On the weather clock, so the sway runs with the wind that drives it and
    // both speed up together under F7.
    {
        PROFILE_ZONE("DrawTufts");

        sod::DrawTufts(world.Grass(), view, world.Sky().Time(), world.Settings().seed);
    }

    // The plants over the ground and behind the character, and on this side of
    // the light multiply: a tree is lit by the same daylight as the ground it
    // stands on, and has to know nothing about it to be.
    // Whatever time of year it is. There is no calendar yet, so this is spring
    // unless F9 is holding one — see weather::Sky::Turn.
    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    {
        PROFILE_ZONE("grove.Draw");

        grove.Draw(world.Sky(), season, world.Sky().Time());

        grove.DrawFruit(world.Sky(), season, world.Sky().Time());
        grove.DrawLeaves(world.Sky(), season, view, world.Sky().Time());

        // What the wood left on the ground, over the plants and under the character.
        grove.Fallen().Draw();
    }

    // What the player has put up. Over the ground it is fixed to and under the
    // character, so walking past a torch passes in front of it — which is where a
    // thing on a wall belongs, and is the same place the pickups above sit.
    {
        PROFILE_ZONE("fixtures.Draw");

        fixtures.Draw(view);
    }

    // Under the character and over the ground, which is where dust off a foot
    // belongs: it is in front of the hillside it came out of and behind the boot
    // that kicked it.
    {
        PROFILE_ZONE("trail.Draw");

        trail.Draw(world.Sky().Time());
    }

    // What walks, behind the character and in front of everything it walks on.
    //
    // Behind on purpose: the character is the one thing the player must never lose,
    // and a boar standing on the same spot has to be the one that gives way. It is
    // the same rule the pickups and the fixtures above are drawn by.
    //
    // Inside the light, like everything else here — a creature in an unlit cave is
    // as dark as the cave, and has to know nothing about that to be.
    {
        PROFILE_ZONE("herd.Draw");

        herd.Draw(view);
    }

    player.Draw(held);

    // Rain in front of the world rather than behind it, so it falls past a cliff
    // face instead of behind one. Still inside the light, because rain in an unlit
    // place should not be the one bright thing on screen.
    //
    // Asked of the world and not of the sky: a drop stops at the first thing under
    // it, and what is under it is the world's to know.
    {
        PROFILE_ZONE("DrawRain");

        world.DrawRain(view);
    }

    // And the fog over all of it, which is the last thing drawn inside the light.
    //
    // Over the rain as well as over the ground, because that is where it is: a
    // shower falling into a fog bank is seen through the fog, and drawing the two
    // the other way round puts every drop in front of the air it is falling
    // through. Inside the light for the same reason the rain is — fog in an unlit
    // place must not be the one bright thing on screen.
    {
        PROFILE_ZONE("DrawMist");

        world.DrawMist(view);
    }

    EndMode2D();

    // Composited over the character, so a submerged body is tinted by the
    // liquid it is standing in.
    {
        PROFILE_ZONE("liquids.Compose");

        liquids.Compose(config::kLiquidAlpha);
    }

    BeginMode2D(camera);

    // Then the whole layer is multiplied by the light at once, and this is the
    // last thing drawn into it. Everything above this line is lit; everything
    // the frame draws after the layer is composited is not, which is exactly the
    // right side of the line for anything meant to be read rather than seen.
    //
    // Skipping the multiply is all it takes to see the world unlit, since the
    // world underneath was already drawn at full brightness.
    //
    // It leaves the blend where EndBlendMode leaves it, which is not the blend
    // the layer is built with — so anything added below this line has to set
    // LitLayer's blend again, or draw with an alpha the layer cannot record.
    {
        PROFILE_ZONE("lights.Compose");

        if (!debug.unlit) ComposeLight(lights);
    }

    EndMode2D();
}

// The world, from the sky down to the brush cursor over it.
//
// Split out from the head-up display because the two are wanted apart: with the
// inventory open the world goes through a blur and the display does not, and a
// blur needs the world rendered into a target of its own. Neither half opens the
// frame, so the caller decides whether that target is the screen.
void render::Scene(const World &world, const Grove &grove, const mob::Herd &herd, const Inventory &inventory,
                   const Player &player, const Editor &editor, const LitLayer &lit, const Camera2D &camera,
                   const debug_view::Toggles &debug, bool aiming) {
    const Rectangle view = view::Bounds(camera);

    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    BeginMode2D(camera);

    // The air first, filling the frame. It replaces clearing it rather than being
    // drawn over a cleared one: there is no height at which the sky is not some
    // colour, so there is nothing for a background to be.
    //
    // And on this side of the light, alone among the things that are drawn in
    // world space. The sky is the source; a cloud's shadow falls through it and
    // not onto it. What is behind the *ground* is drawn again on the other side —
    // see World::DrawUnderground, which is where the two are told apart.
    world.Sky().DrawAtmosphere(view);

    // And the far country in front of it, which is the only other thing in the
    // frame drawn in world space and left out of the light.
    //
    // It belongs on this side of the line for the reason the sky does rather than
    // in spite of it: what lights a range forty screens off is not the lantern in
    // the player's hand, and the haze it dissolves into is the very air drawn
    // behind it. It takes the day from the same number the atmosphere is scaled
    // by, so dusk falls on the horizon and the sky together — and being drawn
    // before the lit layer is composited is what puts it behind every cloud, every
    // hillside and every tree without any of them being asked.
    {
        PROFILE_ZONE("DrawVista");

        world.Vista().Draw(view, world.Sky());
    }

    EndMode2D();

    // Then the world over it, already lit, in one blend.
    lit.Compose();

    BeginMode2D(camera);

    // The stars go on this side of that line, and they are the only part of the
    // world that does.
    //
    // A star is a light rather than something lit, and the multiply cannot express
    // one: under it nothing may come out brighter than the sky's own radiance, which
    // at midnight is a tenth — so every star was a grey smudge and its colour went
    // with its brightness. Out here it keeps both. What it costs is that the ground
    // and the cloud no longer hide a star by being drawn over it, so both are asked
    // instead.
    world.DrawStars(view);

    // Drawn over the world rather than under it: the point of an overlay is to
    // check the world against what produced it, which is impossible while the
    // world covers it.
    if (debug.vertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);
    if (debug.layers) debug_view::DrawLayers(world, view);
    if (debug.chunks) debug_view::DrawChunks(world, view);
    if (debug.light) debug_view::DrawLight(world, view);

    // After the light's own picture, so the edges are drawn over whichever of the
    // two views is underneath them. They are about both.
    if (debug.limits) debug_view::DrawLightLimits(world, view);

    // Last of the overlays, so the colliders sit over the grids rather than under
    // them: what this is for is comparing a box against the picture it belongs to,
    // and a chunk border drawn on top of that comparison is noise in it.
    if (debug.bodies) {
        debug_view::DrawGroundCollision(world, view);

        grove.DrawCollision(season, world.Sky().Time());
        grove.Fallen().DrawCollision(player.Centre());

        // A creature's box, what it notices and what it can reach. The last two are
        // the point: both are invisible in play, and the only symptom of either being
        // wrong is an animal that reacts too early, too late or not at all.
        herd.DrawCollision();

        // The body last and brightest. It is the one collider the player is
        // steering, so it is the one they are checking the others against.
        DrawRectangleLinesEx(player.Bounds(), 1.0f, GREEN);

        const Rectangle swing = player.AttackHitbox();
        if (swing.width > 0.0f) DrawRectangleLinesEx(swing, 1.0f, ORANGE);
    }

    // Not while the panel is up: the pointer is over a slot, not over the world,
    // and a brush ring left under an inventory says the next click will dig
    // where it is sitting when it will not.
    if (aiming) editor.DrawCursor(inventory, grove, season, camera);

    EndMode2D();
}

