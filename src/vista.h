#pragma once

#include "element.h"
#include "raylib.h"
#include "soil.h"
#include "terrain.h"
#include "weather.h"

#include <cstddef>
#include <vector>

// The country behind the country: ranges of mountains standing between the air
// and the world, moving at their own speed as the view goes past them.
//
// It is scenery and not terrain, and the whole design follows from saying so.
// Nothing here is generated, stored, collided with, lit, dug or built on; a range
// is a heightfield along x evaluated where it is drawn and thrown away, so the
// module has no state but its settings and the scratch it rasterises into. What
// it buys is the one thing the generator cannot give at any price — distance.
// The world is a single plane, so everything in it moves with the camera at
// exactly one pixel per pixel, and a landscape where nothing is further away than
// anything else reads as a cross-section rather than as a place.
//
// **Drawn in front of the air and behind the world.** That puts it on the unlit
// side of the frame, beside the sky and away from the light multiply, which is
// where a distant range belongs: what lights a mountain forty screens off is not
// the lantern in the player's hand, and the haze it dissolves into is the very air
// drawn behind it. It takes the day from the same `Daylight::light` the atmosphere
// does, so dusk falls on the ranges and the sky together.
//
// **The palette is the world's own tables and not a second one.** A range takes
// its low ground from `sod::LookAt` — the same meadow, steppe, taiga and desert
// mix that colours the grass at the player's feet — its rock from the rock row's
// paint, and its snow from the snow row's, under the snow row's own climate bell.
// So walking from a pine forest into a desert turns the horizon with the ground,
// nothing anywhere says the word "biome", and a cover added to `sod::kCovers`
// appears in the distance without this file being opened. See CLAUDE.md §8.
namespace vista {

// One range: a heightfield along x, and how far off it stands.
//
// The rows are written far to near, which is also the order they are *not* drawn
// in — see Range::Draw, which walks them near to far and keeps a ceiling per
// column so that every texel of the view is shaded exactly once.
struct LayerDef {
    // Where the foot of the range sits and how far its crests stand above it, in
    // pixels above the horizon.
    //
    // The far rows are the tall ones, which looks backwards and is not: a range on
    // the skyline is a range, and the ones in front of it are the foothills between
    // here and there. Drawn the other way round the nearest layer covers every one
    // behind it and the stack is a single silhouette.
    float foot = 0.0f;
    float amp  = 0.0f;

    // Pixels of world one crest is wide, before the octaves under it.
    float span = 700.0f;

    // Added to the world seed, so two rows of the same width are two different
    // ranges rather than one drawn twice.
    int seed = 0;

    // Share of the camera's own motion the layer takes, in [0,1].
    //
    // Zero pins it to the frame, which is a painted backdrop; one puts it in the
    // world's own plane, where it stops being distant at all. Everything between is
    // a distance, and the ordering of these numbers down the table is the whole of
    // what makes the stack read as depth.
    float distance = 0.5f;

    // Share of the layer dissolved into the air behind it.
    //
    // Aerial perspective, and it is doing two jobs. The obvious one is that far
    // things are paler; the one that matters more is that it is a *sort order the
    // eye can read* — a nearer range is darker than the one behind it whatever the
    // two silhouettes happen to be doing, so the stack never collapses into one
    // shape. It mixes towards the sky as drawn, so an overcast afternoon greys the
    // horizon and a sunset burns it without either being written down here.
    float haze = 0.0f;

    // Overall brightness of the layer before the haze takes it.
    //
    // Under one for the near rows. Ground close to the eye is in the shade of
    // everything standing on it, and a foreground hill at the brightness of a
    // distant one reads as a paper cut-out laid over the picture.
    float lum = 1.0f;

    // How much of the snow the climate offers this row actually takes, in [0,1].
    //
    // Not whether it is cold: that is the snow row's own bell, read at the column
    // (see Range::Draw). This is a property of the *distance* — the near rows are
    // foothills and stand below any snow line, so they take none of it however far
    // north the world has got.
    float snow = 0.0f;
};

// The stack, far to near.
//
// Five rows, and the count is not arbitrary. Three reads as three walls of card;
// past six the far rows are behind the near ones nearly everywhere and cost their
// whole width to contribute a few texels of sky. Five fills a screen from the
// treeline to the top of the frame with something at every distance.
inline constexpr LayerDef kRanges[] = {
    // The skyline. Broad, pale, barely moving, and the only row tall enough to
    // stand against the cloud deck — which hangs from y = -640, so a crest at 640
    // above a horizon of 144 is level with its underside and no higher. A range
    // drawn *into* the deck would be a mountain in front of a cloud that is
    // supposed to be miles behind it.
    {.foot = 250.0f, .amp = 250.0f, .span = 900.0f, .seed = 0, .distance = 0.10f, .haze = 0.55f, .lum = 1.00f, .snow = 1.0f},

    // The second range, and the first with any shape the eye can hold. Narrower
    // crests, a little more of the day on it.
    {.foot = 190.0f, .amp = 215.0f, .span = 620.0f, .seed = 811, .distance = 0.20f, .haze = 0.46f, .lum = 0.98f, .snow = 0.85f},

    // Middle distance: the last row that gets any snow at all, and it gets it only
    // on the crests the noise happens to push over the line.
    {.foot = 130.0f, .amp = 185.0f, .span = 430.0f, .seed = 1627, .distance = 0.34f, .haze = 0.31f, .lum = 0.93f, .snow = 0.45f},

    // Foothills. Below the snow line by construction, and dark enough to read as
    // the near side of the valley.
    {.foot = 70.0f, .amp = 140.0f, .span = 290.0f, .seed = 2333, .distance = 0.52f, .haze = 0.17f, .lum = 0.84f, .snow = 0.0f},

    // The near rise, which is mostly hidden behind the world's own hills and is
    // there for the moments it is not: standing on a summit, or looking across a
    // valley. Nearly the world's own speed, nearly no haze, and darkest of all.
    {.foot = 10.0f, .amp = 105.0f, .span = 190.0f, .seed = 3701, .distance = 0.74f, .haze = 0.07f, .lum = 0.70f, .snow = 0.0f},
};

inline constexpr std::size_t kRangeCount = std::size(kRanges);

// Everything the rows share.
struct Settings {
    // Off draws nothing and costs nothing. Kept as a switch rather than an empty
    // table because "what does the world look like without it" is the question this
    // whole module has to answer for itself.
    bool on = true;

    // ------------------------------------------------------------ the heightfield

    // Octaves of the ridged fold, and how each one is masked by the one above it.
    //
    // The mask is what separates a ridged multifractal from a folded field with
    // detail on it: an octave is multiplied by the octave above, so the fine work
    // only appears where the coarse field is already near a crest. Without it the
    // valleys are as busy as the ridges and the range reads as noise.
    int octaves    = 4;
    float sharp    = 1.15f;
    float lacunarity = 1.93f;
    float gain       = 0.5f;

    // An exponent on the finished fold, and it is under one for the reason
    // `SurfaceSettings::ridgeSharp` is (§9): `1 - |signed|` is a triangle by
    // construction, so the crest comes to a point and the near rows — whose crests
    // are two hundred pixels apart — read as a comb of needles rather than as
    // hills. Under one the mid-range is lifted instead, so the shoulders broaden
    // and every summit gets a top.
    float crestRound = 0.80f;

    // The wide swell over the top of it, which is what makes a range a run of
    // massifs with low ground between rather than a hedge of even teeth.
    //
    // Its frequency is a share of the row's own, so a narrow range and a broad one
    // are modulated at their own scale rather than both at a fixed one.
    float swellOf   = 0.15f;
    float swellFloor = 0.40f;
    float swellSwing = 0.90f;

    // ------------------------------------------------------------------ the dunes
    //
    // A desert's horizon is not a range of mountains in sand colours, and painting
    // it as one is the same fault §8 calls a wood in the desert: a palette says what
    // a place is made of and a *shape* says what it is. So the fold above is blended
    // out against the sand row's own climate bell — the very bell that put the sand
    // on the ground — and what it is blended into is a smooth field, broader and far
    // lower.
    //
    // Three numbers, and each says one thing a dune is:

    // **It has no crest.** The ridged fold creases where its field crosses zero,
    // which is exactly what a dune does not do; a plain smooth field rounds over
    // instead, and the exponent under one lifts its mid-range so the tops are long
    // curves rather than bumps between hollows.
    float duneRound = 0.65f;

    // **It is broader.** One swell to a row's crest and a half.
    float duneSpan = 1.6f;

    // **It is low.** The whole lift is scaled, foot and all, so a desert horizon
    // flattens down towards the land instead of standing over it — which is most of
    // what makes a desert read as open country from inside one.
    float duneRise = 0.42f;

    // And how far either side of the sand row's own pair of edges the change of
    // shape is spread, in units of its bell.
    //
    // Wider than the ground's, on purpose, and it is the one place the horizon is
    // allowed to disagree with the table it reads. §8 wants the *ground's* desert to
    // have an edge — a bell is a tendency and a desert with a fringe of half-sand
    // around it is not a desert. A silhouette is the opposite case: the ranges come
    // down by three fifths across that edge, so at the ground's own width the
    // skyline falls off a cliff inside one screen, and what the eye reads is a wall
    // rather than a country changing. Widened, the crests round off over a walk.
    float duneEdge = 0.22f;

    // ---------------------------------------------------------------- the shading

    // Light on a face with no slope and nothing above it.
    float ambient = 0.42f;

    // How much a slope turned towards the sun brightens, and away from it darkens.
    // The sun's own direction is read from the day; only the strength is here.
    float slopeLight = 0.18f;

    // How much brighter the ground near a crest is than the ground in a valley,
    // and over how much of the row's own amplitude that runs.
    float volume = 0.40f;
    float volumeOf = 0.90f;

    // The mottling, which is the only term that varies across a face rather than
    // down it. Without it a range is a set of vertical ramps and the eye finds the
    // ramp before it finds the mountain.
    float mottle = 0.17f;

    // The lit rim along the very top of the silhouette, in texels.
    //
    // Small and load-bearing. A pixel-art mountain is read from its outline, and a
    // line of light along the crest is what tells the outline apart from the sky
    // behind it at any distance — it is the same trick the grass and the leaves use
    // and it is why the ranges do not need an outline drawn round them.
    float rim     = 0.45f;
    float rimFrom = 0.5f;
    float rimTo   = 2.5f;

    // Steps of light the shading is quantised to, and the dither across them.
    //
    // Four, like the cloud's bands and the material ramps: the picture is made of
    // flat tones and the dither is what carries the values between them. A smooth
    // ramp here would be the one part of the frame that is not pixel art.
    int tones        = 4;
    float ditherTone = 0.30f;

    // ----------------------------------------------------------------- the colour

    // Pixels above the horizon the ground cover gives out over and the bare rock
    // takes over, and the steps between them.
    //
    // A treeline, in effect, and it is measured in the layer's *own* apparent frame
    // rather than in the world: a distant range and a near one both have their
    // rock above their green, which is what altitude looks like from here.
    float ground = 55.0f;
    float bare   = 280.0f;
    int bands    = 3;

    // How far the line between them wanders, as a share of the run, and how far the
    // snow line does, in pixels.
    //
    // Both exist for the same reason and it is the reason every edge in this
    // project is dithered rather than cut: a boundary that follows a contour of
    // height alone is a horizontal line drawn across a mountain, and there is no
    // such thing.
    float lineJitter = 0.22f;
    float snowJitter = 110.0f;

    float ditherGround = 0.55f;
    float ditherSnow   = 0.55f;

    // Pixels above the horizon the snow begins, and over how many it comes in.
    //
    // Higher than `bare`, so there is a belt of naked rock between the green and
    // the white. A snow line sitting on the treeline reads as a hill someone has
    // painted the top of.
    float snowLine = 330.0f;
    float snowFade = 55.0f;

    // Slope past which snow does not lie, as a rise per pixel run.
    //
    // A cliff sheds it. This is what keeps the caps on the shoulders and the tops
    // instead of coating the whole crest, and it is most of what makes them read as
    // snow rather than as a second colour of rock.
    float snowSheds = 0.90f;
    float snowBare  = 2.20f;

    // ---------------------------------------------------------------- the extents

    // Pixels below the horizon the stack is drawn down to at the most.
    //
    // A bound and not a shape: every column is cut at the generated surface long
    // before it gets here (see Range::Draw), and this is what stops a view with no
    // ground in it at all — flying, or underground — from filling the frame with
    // hillside nobody can see. Comfortably past the lowest the land ever gets, so
    // it is never the thing that decides where a range stops.
    float reach = 1100.0f;

    // Pixels past the generated surface each column is drawn, so that the terrain
    // drawn over it has something to cover.
    //
    // The clip is against `terrain::Height`, which is where the ground *is* and not
    // where its contour was drawn — the contour stands up to half a lattice step
    // above it, and the chunk texture's squares another texel past that. Under-run
    // it and a hairline of sky opens along the whole skyline.
    float sink = 24.0f;

    // The mottling and the two wandering lines, as fields.
    // One octave, and the count is a measurement rather than taste. The mottle is
    // the only term read per *texel* rather than per column, so it is the whole of
    // what the shading costs — a second octave of it is a quarter of a million more
    // samples of Perlin a frame to add detail at a scale finer than the mottle is
    // meant to have.
    terrain::NoiseShape patch = {.frequency = 16.0f, .octaves = 1, .aspect = 1.4f, .seed = 6101};
    terrain::NoiseShape line  = {.frequency = 5.0f, .octaves = 1, .seed = 6117};

    // Slow and three octaves deep, and both halves of that are the fix for the one
    // fault this module shipped with. A snow line jittered by a fast field is a
    // straight line with a fringe on it; jittered by a slow one it wanders across a
    // range the way a real one does, and the octaves under it keep the wandering
    // from being a sine wave. Its swing is `snowJitter`, which is most of the fade
    // it is added to — anything less and the eye finds the ruled horizontal line
    // before it finds the mountain.
    terrain::NoiseShape drift = {.frequency = 2.2f, .octaves = 3, .seed = 6133};
};

// The ranges, configured against the world they stand behind.
class Range {
public:
    // Takes the terrain because the palette is the terrain's: the climate at a
    // column decides the cover, the cover decides the green, and the snow row's own
    // bell decides whether there is any white on the tops. Call again after
    // World::Rebuild — a range in the old country is the wrong colour in the new
    // one.
    void Configure(const Settings &settings, const terrain::Settings &terrain);

    const Settings &Config() const { return settings_; }

    // Whether any of the stack falls inside the view at all.
    //
    // Underground the answer is no and the whole cost of the horizon goes away
    // without anything having to ask where the player is — the same early out
    // Sky::DrawClouds makes against the cloud band, for the same reason.
    bool Visible(Rectangle view) const;

    // The stack, drawn in world space on the world's own texel grid.
    //
    // Reads the sky rather than being handed a palette: the haze is the air as it
    // is actually drawn at that height, the light on a face is the sun where it
    // actually stands, and the day is the same one the atmosphere is scaled by. So
    // there is nothing to keep in step and no second copy of the weather.
    void Draw(Rectangle view, const weather::Sky &sky) const;

    // Releases the texture the stack is blitted from. Called once, where the window
    // is being closed.
    void Unload();

    // World Y of the highest crest standing over a column, or nothing where the
    // stack does not reach the view.
    //
    // For the stars, which are drawn after the world and so are no longer hidden by
    // anything simply because it was drawn later — see Sky::DrawStars, which is
    // handed the ground for exactly this reason and now gets the ranges folded into
    // it.
    float CrestAt(float worldX, Rectangle view) const;

private:
    // Height of one range over a column, in pixels above its own foot, in [0,1].
    // The creased profile and the rounded one; `Lift` blends them.
    float Fold(float sample, const LayerDef &def) const;
    float Dune(float sample, const LayerDef &def) const;

    // Where a piece of a layer stands, as a position the climate can be read at.
    //
    // **This is the fix for a range that changed shape as the player walked**, and
    // it is worth the whole of this comment because the fault was subtle and the
    // picture of it was unmistakable: a mountain would stay put on the skyline and
    // its snow, its colour and — worst — its very outline would slide about under
    // it, so the country in the distance rearranged itself while nobody was going
    // anywhere.
    //
    // The cause was reading the climate at the *drawn* position. A layer's content
    // moves at its own fraction of the camera, so a fixed piece of a far range is
    // drawn at a world position that runs away from it at nine tenths of walking
    // pace; asking the climate there asks about somewhere else, and the answer
    // changes every frame. Everything else in the module was already a pure function
    // of `sample` and therefore already still — the fold, the swell, the dune field
    // — which is exactly why the drift read as one feature misbehaving rather than
    // as the scenery moving.
    //
    // So the climate is asked at the content coordinate, which is nailed to the
    // layer: `sample` divided by the distance. The division is the second half of
    // the fix and not a scale factor picked to look right. Without it the far rows
    // would traverse their own country at a tenth of walking pace and a desert
    // twenty screens across would show a fifth of itself on the horizon; with it,
    // one screen of *this* layer covers as much country as its distance says it
    // does, so the biome the player is standing in and the biome on the skyline
    // change over the same walk. It also falls out that a far row shows a wider
    // sweep of the world across one screen than a near one, which is what distance
    // means.
    float Country(float sample, const LayerDef &def) const;

    // The climate of a stretch of country, without the altitude term.
    //
    // `terrain::ClimateAt` cools its reading by the elevation of the ground under
    // it, which is right for the ground and is a **cliff generator** here. A far
    // row's country runs ten pixels for every one of the view, so `terrain::Height`
    // read along it swings a hillside's worth between neighbouring columns; through
    // the lapse rate that is a swing of a third of the temperature range, which
    // flips the desert on and off column by column — and since being a desert scales
    // the whole row (see DuneScale), what it draws is a wall of vertical slabs and
    // one-column needles where a range should be.
    //
    // It is also the wrong question. The lapse says how the air cools over the
    // ground *at that column*, and a range on the horizon is not standing on that
    // ground — its own altitude is its own business, and the snow line already asks
    // it as a height in the row's own frame. So this reads the two fields flat, at
    // sea level, which is the regional climate and is the only half of the answer
    // scenery has any use for.
    terrain::Climate Regional(float country) const;

    // How much of a desert a piece of a layer is, in [0,1], off the sand row's own
    // bell.
    float Dunes(const terrain::Climate &climate) const;

    // And the whole of it: pixels above the horizon, in the layer's apparent frame.
    float Lift(float sample, const LayerDef &def, float dunes) const;

    // Where a layer's content is read for a column of the view, and how far its
    // horizon has slid from the world's. The two halves of the parallax, and the
    // only place either is written.
    float Sample(float worldX, Rectangle view, const LayerDef &def) const;
    float Slide(Rectangle view, const LayerDef &def) const;

    Settings settings_{};
    terrain::Settings terrain_{};

    float horizon_ = 0.0f;

    // The rock and the snow, built once. Neither is a function of anything that
    // moves, and rebuilding a ramp per column is seven blends of nothing.
    soil::Ramp rock_{};
    soil::Ramp snow_{};

    // Scratch, and not state: filled and read inside one call to Draw and saying
    // nothing about the world between them. Members so that drawing a horizon costs
    // no allocation, which is the same reason Sky keeps its own band buffer.
    mutable std::vector<float> lifts_;   // kRangeCount rows of (wide + 2)

    // The climate under every column of every row, one column either side, read once
    // in the lift pass and read again by the shading. Both need it — the shape asks
    // it for the dunes and the colour asks it what the ground is made of and whether
    // anything up there is cold.
    //
    // Per row and not per column, because each row stands in its own country: see
    // `Country`. That is five climates a column rather than one, and it is what
    // being still costs.
    mutable std::vector<terrain::Climate> climates_;

    mutable std::vector<int> floors_;    // where each column is cut off, in texels
    mutable std::vector<int> ceilings_;  // first texel row of each column not yet painted
    mutable std::vector<Color> air_;     // the sky behind, one per texel row

    // The finished picture, one texel of it per element, **row major** — which is
    // the layout the upload wants and not the one the shading writes, and the
    // shading takes the stride rather than the other way round.
    mutable std::vector<Color> paint_;

    // And the texture it is blitted from, which is what a horizon costs instead of
    // what it used to.
    //
    // Submitted as runs of one colour first, on the model of every other rasteriser
    // here, and that measured **ten milliseconds a frame** — a third of it — for one
    // layer of scenery. The reason is in the haze: it mixes towards the air, the air
    // is drawn in ten-pixel bands, and a texel is three, so the colour changes every
    // third row however flat the mountain is and no run can ever be longer than
    // that. Eighty thousand rectangles a frame is not a rasterisation strategy.
    //
    // A texture has none of that. It is the same argument World::PaintChunks makes
    // about the ground — one texel per unit of the grid it was worked out on, point
    // sampled, blitted at exactly that scale, so the picture is identical to the
    // rectangles it replaces — with the one difference that this cannot be cached
    // between frames, because the parallax moves it and the day recolours it. What
    // it saves is submission and not work, and submission was all of the cost.
    mutable Texture2D picture_ = {};
    mutable int width_         = 0;
    mutable int height_        = 0;
};

} // namespace vista
