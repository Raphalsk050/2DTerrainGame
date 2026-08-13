#pragma once

#include "grid.h"
#include "raylib.h"
#include "terrain.h"

// The sky over the world: the air itself, the cloud standing in it, the rain it
// brings and the shade it casts.
//
// There is one direction of dependency here and everything follows it downwards.
// Nothing asks downwards.
//
//   THE WEATHER   a state of the whole world, on a timer
//   clear / fair / overcast / storm
//        |
//        +--> how much of the sky is filled
//        +--> whether it is raining, and how hard
//        +--> the palette the cloud is lit and shaded with
//                |
//   THE CLOUD FIELD   procedural, a pure function of position
//        |
//        +--> what is drawn in the sky
//        +--> how much daylight reaches the ground   (light::Medium::cover)
//        +--> where the drops fall from
//
// Rain belongs to the top of that list and not the bottom, and this is the one
// structural decision in the module. It was the other way round and it was wrong:
// rain read off each column's own cloud thickness, so a small cluster rained while
// the cluster beside it, in the same weather, did not. Every game that has solved
// this solved it the same way — Minecraft's sky is one sky for the whole dimension
// and every biome turns overcast together, Terraria's rain covers all surface
// biomes for its duration, Stardew picks one weather per day for the map. Weather
// is a state of the world; a cloud is what that state looks like.
//
// The front survives, demoted. It ripples the cover from place to place so the sky
// is not a flat sheet. It no longer decides whether it rains.
//
// Nothing is simulated. The weather is a pure function of elapsed time and the
// cloud a pure function of position, exactly as the terrain is, so there is no
// state to keep and two views of the same world at the same moment agree.
namespace weather {

// How far the air carries a loose thing sideways over its own lifetime, in world
// pixels, at the strongest gust the weather can produce.
//
// The one figure every particle in the world is blown by. Leaves off a crown,
// leaves the year sheds, chips out of a trunk — each has its own weight, below,
// and every one of them multiplies this. Turning the wind up or down in the world
// is a change to this line and to nothing else, which is the whole reason it is
// here rather than three constants in three files that happened to agree.
//
// Well over the width of a mature crown, and it has to be: a leaf that lands where
// it was let go of has not been blown anywhere.
inline constexpr float kCarry = 150.0f;

// How readily one kind of loose thing takes the wind up, as a share of what a leaf
// does. A leaf is the unit, because a leaf is the thing the figure above was set
// against; anything denser than one is carried less.
inline constexpr float kLeafDrag = 1.00f;
inline constexpr float kChipDrag = 0.22f;

// The sideways offset the air has given a particle, in world pixels.
//
// `push` is the wind where the particle is, as a share of the strongest gust —
// Sky::PushAt, and nothing should be computing it any other way. `flown` is how
// far through its own flight the particle is, in [0,1]. `drag` is one of the
// weights above.
//
// Squared in `flown`, and that is the part worth keeping in one place: what is
// being drawn is a thing picking the wind up, not one already travelling at its
// speed. A leaf leaves the branch at whatever speed it was let go at and is moving
// with the air by the time it lands, and a term linear in time reads instead as
// the whole world sliding sideways.
inline float Carry(float push, float flown, float drag) {
    return push * kCarry * drag * flown * flown;
}

// Margin value the outside edge of a cloud sits at.
//
// The cloud grid holds a *margin* — how far the cloud field stands above the
// cutoff for the sky it is in — rather than a density clamped into [0,1]. That is
// what lets the same grid be drawn several times at rising thresholds and give
// properly nested shapes; a clamped density saturates at one through the whole
// interior and every threshold above zero would draw the same outline.
inline constexpr float kCloudEdge = 0.0f;

// A kind of weather, and everything that follows from having it.
//
// One row per mood, and the row is the whole definition: the sky's fill, the rain,
// the palette and the shade all move together because they are the same fact about
// the same afternoon. Crossing between two moods interpolates every field of the
// row at once, so a storm gathers rather than switching on.
enum class Mood { Clear, Fair, Overcast, Storm, Count };

inline constexpr int kMoodCount = static_cast<int>(Mood::Count);

struct MoodDef {
    const char *name;

    // Share of the sky this weather fills. The storm's figure is what makes cloud
    // cover most of the sky when it rains, which is not a separate rule.
    float cover;

    // How hard it rains under it, in [0,1]. Uniform across the world, which is the
    // entire point: it is the weather that is raining, not the cloud.
    float rain;

    // How likely this mood is to be the next one drawn. Relative, not a share.
    float likelihood;

    // The two lights a cloud is under in this weather: the sun on it, and the sky
    // all around it. A shadowed part of a cloud is not black, it is sky-coloured,
    // which is the most important thing about painting one — and under a storm what
    // changes is not how much ambient reaches the cloud but what the ambient is.
    Color sunlight;
    Color ambient;

    // Share of the daylight the thickest cloud holds back. Read against the
    // exposure curve, not as a brightness: see Settings::shade below.
    float shade;
};

// The weather at one moment, which is generally a blend of two moods.
struct Weather {
    const char *name = "clear";

    float cover = 0.0f;
    float rain  = 0.0f;
    float shade = 0.0f;

    Color sunlight = {255, 255, 255, 255};
    Color ambient  = {255, 255, 255, 255};
};

// The time of day at one moment.
//
// Kept apart from `Weather` rather than folded into it, for two reasons. A mood row
// is the whole definition of a kind of weather and every field of it is
// interpolated together; a daylight field would be the one exception to the rule
// that design exists to enforce. And they are genuinely orthogonal — a storm at
// noon and the same storm at midnight are the same weather, seen by a different
// light.
//
// Everything below follows from `elevation` alone. Nothing here is a keyframe and
// no colour in it is authored: a sunset is what the air already in `Atmosphere`
// does to light that has crossed a long path of it.
struct Daylight {
    const char *name = "night";

    // Where the day has got to, in [0,1). Zero is midnight, a half is noon.
    float phase = 0.0f;

    // How high the sun stands, in [-1,1]. Below zero it has set.
    float elevation = -1.0f;

    // How much of the day's light there is, in [0,1].
    //
    // The one scalar everything else is scaled by, and the number a game rule
    // should be written against — mobs that will not walk in it, crops that need
    // it, ground that dries under it.
    float light = 0.0f;

    // Which way the sun lies, for the cloud's own shading. Y grows downward, so a
    // negative Y is overhead. It points *below* the horizon once the sun has set,
    // which is what lights the underside of a cloud at dusk, and is free.
    Vector2 sun = {0.8f, -0.6f};

    // How far the sunlight has travelled through air before arriving, as a share of
    // the longest such path. Zero with the sun overhead, one along the horizon.
    float travel = 0.0f;

    // The colour it has left after that journey, normalised so its strongest
    // channel is one. White overhead, orange along the horizon — the reddening is
    // the blue scattered out on the way in, not a colour anybody picked.
    Vector3 beam = {1.0f, 1.0f, 1.0f};
};

// The air, as a scattering medium.
//
// The gradient up the sky is not a pair of colours interpolated between. It is
// the two facts that produce one: air thins out with altitude, and short
// wavelengths scatter far more than long ones. Everything the sky does follows —
// the pale band at the horizon where the air is thick enough to scatter every
// colour, the deep blue overhead where only the short end still scatters, and the
// fade towards black above that, where there is not enough air left to scatter
// anything.
//
// Written this way because it adapts for free. Change the daylight and the whole
// gradient changes with it, correctly, without a single colour being re-picked.
struct Atmosphere {
    // Altitude over which the air thins by a factor of e, in pixels.
    //
    // The one number that sets how quickly the sky deepens as it rises. Around
    // half a screen, so the gradient is a visible thing within one view rather
    // than something only noticed after climbing for a while.
    float scaleHeight = 320.0f;

    // How much air stands in the line of sight at the horizon. Raising it pushes
    // the whole gradient upward: a thicker atmosphere stays pale further up.
    float thickness = 3.5f;

    // Scattering strength per channel.
    //
    // Rayleigh scattering goes as the inverse fourth power of the wavelength, so
    // blue scatters some six times more strongly than red. These are those
    // coefficients, normalised so the blue channel is one. They are why the sky is
    // blue at all, and why it whitens rather than merely brightening towards the
    // horizon: the blue channel saturates first and the others catch up.
    Vector3 rayleigh = {0.52f, 1.15f, 2.60f};

    // World Y the gradient is measured from. Set from the terrain's surface level
    // when the sky is configured, so raising the ground raises the horizon with it.
    float horizon = 0.0f;

    // Colour the sky washes towards under complete cloud. An overcast day is not
    // a dimmer blue day; it is a flat grey one, because what is being looked at is
    // the underside of the cloud and not the air.
    Color overcast = {150, 156, 168, 255};

    // How far towards it a fully overcast sky actually goes.
    //
    // Short of all the way, and the wash is driven by how overcast the region is
    // rather than by whether a cloud happens to be overhead. Washing the background
    // out by the cloud in front of it takes the contrast out of the very sky the
    // cloud has to be seen against, which leaves a storm reading as fog.
    float overcastWash = 0.55f;

    // Height of one band of the gradient, in pixels. The sky is drawn as flat
    // bands rather than as a smooth ramp, so it belongs to the same picture as the
    // world beneath it. A multiple of config::kPixelSize keeps its steps in line
    // with everything else's.
    float bandHeight = 10.0f;
};

// The stars, which are what the sky is when there is no sun in it.
//
// Scattered rather than placed: one to a cell of a coarse lattice, each one's
// presence, position and brightness hashed out of the cell it is in. A pure
// function of where you are looking, like everything else in the sky, so there is
// no field to hold and no two views of it can disagree.
//
// Drawn between the air and the cloud, which is the whole of how a cloud hides
// them. What a cloud cannot hide is the sky above its own ceiling, and that is what
// `hidden` is for.
struct Stars {
    // Pixels of screen between one star and the next, roughly. Smaller is a denser
    // sky; below about forty they stop reading as points and start reading as noise.
    float spacing = 90.0f;

    // How big one is, in world pixels.
    //
    // Smaller than `config::kPixelSize`, and the only thing in the world drawn off
    // that lattice. Everything else is a surface standing in the world and belongs
    // to its grid; a star is a point at an unreachable distance, and at the world's
    // own pixel size it reads as a tile of something rather than as a light.
    float size = 3.0f;

    // Share of the world's own motion the field takes as the view moves.
    //
    // Zero pins them to the screen, which reads as dust on the glass; one puts them
    // at arm's length and they slide past like scenery. Small, because a star is a
    // long way off, and the whole point of the number is that it barely moves.
    //
    // Applied about the horizon, so the horizon stays where it is and the sky draws
    // in towards it — which is what distance does.
    float parallax = 0.14f;

    // Height above the horizon over which they come in, in pixels: nothing at the
    // ground, full above it.
    //
    // The air low in the sky is thick enough to put out anything shining through it
    // — the same fact that makes the horizon pale, seen from the other side — and it
    // has to bite hard. Stars that run all the way down to the treeline read as
    // holes punched in the picture rather than as a sky.
    //
    // Written as a height rather than as an airmass on purpose. Airmass only varies
    // fivefold across the whole visible sky, so a coefficient strong enough to clear
    // the horizon takes the top of the screen down with it; a height saturates, so
    // the ground can be swept clean while everything above stays at full strength.
    float rise = 320.0f;

    // The cover at which a sky begins putting its own stars out, and the cover at
    // which it has finished.
    //
    // Not a straight share of the cover, and the difference is the whole of how a
    // clear night looks. The clouds that are actually there already hide the stars
    // behind them, one at a time and exactly; this is only for the sky a closed deck
    // seals over, where there is no cloud at that point to ask. Dimming everything in
    // proportion to a cover of a third would count it twice and leave a fair night
    // reading as an overcast one.
    float hideFrom = 0.55f;
    float hideAt   = 0.95f;

    // How far either side of a cloud's own outline a star behind it is faded out,
    // in field units.
    //
    // Not a hard test against the outline, for two reasons. The cloud on screen is
    // rasterised from a lattice a dozen pixels across and interpolated between, so
    // its drawn edge and the field's exact edge disagree by up to a cell — and a
    // star left shining on the rim of a cloud is the one place the eye goes. And an
    // edge is where a cloud is thinnest, so something dimming as it passes behind
    // one is what it should do anyway.
    float cloudEdge = 0.12f;

    // How much brighter the brightest star is than the faintest.
    //
    // Small. A field with the full range in it reads as noise rather than as a sky —
    // the eye finds the scatter before it finds the pattern. What carries the
    // variety is the colour.
    float spread = 0.22f;

    // How far apart in colour the two ends actually run, of the range `hot` and
    // `cool` describe. One takes the pair as written; a half keeps them nearer white.
    float tint = 0.70f;

    // How much a star's brightness wavers, and how quickly. Air moving in front of
    // it; the reason a star twinkles and a planet does not.
    float twinkle     = 0.15f;
    float twinkleRate = 1.7f;

    // The two ends of the colour a star can be, drawn between per star.
    //
    // Not one colour. A field of identical warm-white dots reads as dead, and the
    // reason is that a real sky is not one colour: a star's colour is its
    // temperature, and they run from blue-white through white to amber. Two ends and
    // a hash is the whole of it, and it is the difference between a sky and a
    // scattering of pixels.
    //
    // Both are kept well clear of grey. What the eye is given here is a handful of
    // very small marks against a nearly black ground, and a colour that is only
    // slightly tinted does not survive being blended down to a quarter alpha.
    Color hot  = {170, 202, 255, 255};
    Color cool = {255, 186, 128, 255};
};

// How a cloud takes the light.
//
// One rule, applied per cell, from the cell's own depth into the cloud. Nothing
// about it can depend on where the camera is, because the camera is not one of its
// inputs — which is the fault this replaced, where a single column at the centre of
// the screen decided the shading of every cloud in view.
//
// The rule is the one the film and game industry settled on for volumetric cloud,
// from Andrew Schneider's Nubis work on the Decima engine:
//
//     BeerPowder(d) = 2 · exp(-absorption·d) · (1 - exp(-2·absorption·d))
//
// The first factor is Beer-Lambert: light falls away exponentially with how much
// cloud it has come through. On its own it is monotone, and a cloud shaded by it
// alone reads as a smear with a gradient on it. The second is the powder term. It
// models the light that scatters back out near a surface, and it turns the curve
// around at the very edge, so a thin fringe is *darker* than the body just inside
// it. That reversal is the whole difference between a shape with a gradient and
// something the eye reads as cloud.
//
// `d` is how much cloud lies between the cell and the sun, read by sampling the
// same grid at an offset — a lookup, not a new sample of the noise.
//
// The result is quantised into `layers` levels, so what reaches the screen is flat
// bands of colour. That is the pixel-art step, and it happens at the very end
// rather than being the model.
struct Shading {
    // How many bands the light is quantised into. Three is a flat sticker, ten is a
    // smooth ramp with no pixel-art character left; five or six reads as drawn.
    int layers = 6;

    // Where the sun lies is no longer written here. It is `Daylight::sun`, swung
    // through the day by `Day::sunTilt`, and every cloud in the world relights
    // itself as it moves. Nothing else about the shading changed.

    // How far towards the sun the depth is read, in pixels.
    float sunReach = 120.0f;

    // How much light is lost per unit of cloud between a point and the sun.
    float absorption = 2.2f;

    // How strongly the powder term bites, and over what depth of local cloud it
    // comes in. Powder at zero leaves plain Beer-Lambert.
    //
    // These two are measured on *different* things and that is the whole subtlety:
    // Beer is the path to the sun, powder is the density here. Applying powder to
    // the path is wrong in a way that is easy to miss and obvious once drawn —
    // powder is zero at zero depth, so a point with a clear line to the sun, which
    // is the brightest thing in the sky, comes out in the darkest band.
    float powder      = 1.0f;
    float powderScale = 9.0f;

    // Lightest and darkest the quantised bands may run, as a share of the range
    // between the mood's ambient and its sunlight. Keeping the top short of one
    // leaves the brightest band below pure sunlight, which stops a cloud edge from
    // blowing out to white.
    float darkest  = 0.06f;
    float lightest = 0.94f;
};

// The turning of the day.
//
// One angle drives all of it. The sun's elevation is a sine of the phase, and the
// light, its colour, its direction and how fast the ground dries are every one of
// them a function of that single number. There is no keyframe table here on
// purpose: a dawn is not four authored colours faded between, it is what happens
// when the light has to cross more air to arrive.
struct Day {
    // Days in a year. Zero means there is no year yet, which is where this world
    // stands: the seasons exist as a path through the code and as four palettes,
    // and nothing turns them until this is given a value.
    float yearDays = 0.0f;

    // Length of one whole turn, in minutes of the weather's own clock — so the
    // debug key that runs the weather fast runs the day fast with it.
    //
    // Deliberately not a whole multiple of `spellMinutes`. At four spells to a day
    // every dawn would fall at the same point of a spell for ever, and the weather
    // would never once break differently over a sunrise.
    float dayMinutes = 24.0f;

    // Where in the turn a fresh world starts, in [0,1): zero is midnight, a half is
    // noon.
    //
    // Not zero, which is where the clock would otherwise begin. Opening the game in
    // the pitch dark reads as something being broken rather than as night, and it is
    // the first thing anybody sees.
    float startAt = 0.30f;

    // Sun elevations the daylight eases between: dark at or below the first, full
    // at or above the second.
    //
    // Both sit below the horizon's own zero, which is what makes the day longer
    // than the night — light arrives before the sun does and outlasts it. Widening
    // the gap buys a longer twilight; lowering the pair buys a longer day.
    float darkAt = -0.45f;
    float litAt  = 0.05f;

    // How far the sun leans off vertical at noon, as a share.
    //
    // A sun directly overhead lights only the tops of the clouds and takes the side
    // off them, which is the fault `Shading` warns about. Held off the vertical so
    // there is always a lit flank, and the tilt is what swings the shadows across
    // the sky through the day.
    float sunTilt = 0.50f;

    // How much air the light crosses at the horizon, against overhead.
    //
    // This is the whole of the sunset. It is *not* the air in the line of sight —
    // more of that only whitens, because in-scattering saturates every channel
    // towards one. It is the air the beam already crossed before arriving, which
    // takes the blue out of it and leaves the red.
    float travel = 0.65f;

    // Share of a cloud's shade that survives the night.
    //
    // Without it a storm at midnight is black: the shade multiplies whatever light
    // there is, and holding back three quarters of a moonlit sky leaves nothing.
    // The cloud is still there and still darker; it simply cannot take away light
    // that was not arriving.
    float nightShade = 0.15f;

    // How long the ground remembers the weather, and how quickly it forgets.
    //
    // Both in minutes. The window is how far back the reckoning looks; the half
    // life is how fast rain an hour ago counts for less than rain a minute ago.
    float wetMinutes  = 15.0f;
    float wetHalfLife = 4.0f;

    // How far a soaking can raise the ground's humidity, and how far a long dry
    // spell of daylight can lower it, on top of the climate the place already has.
    float wetGain = 0.55f;
    float dryGain = 0.30f;
};

struct Settings {
    Atmosphere air{};
    Stars stars{};
    Shading shading{};
    Day day{};

    // The weather this world can have, and how long a spell of it lasts.
    //
    // The clock is not a state machine with a current mood in it. Time is cut into
    // spells of `spellMinutes`, each spell's mood drawn from a hash of its index and
    // the world seed, and the last `crossMinutes` of a spell blended into the next.
    // So the weather is a pure function of the clock: nothing to store, nothing to
    // save, and two views of the same world at the same moment agree.
    MoodDef moods[kMoodCount]{};

    float spellMinutes = 6.0f;
    float crossMinutes = 1.5f;

    // Reseeds the sequence of spells without touching anything else.
    int seed = 7717;

    // The band of sky cloud fills. Y grows downward, so `ceiling` is the top of it
    // and `base` the underside. Both are absolute heights, well above
    // `terrain::SurfaceSettings::level`, since cloud that could be walked into
    // would be fog.
    float ceiling = -620.0f;
    float base    = -300.0f;

    // How much lower the underside hangs in the heaviest weather. A rain cloud sits
    // closer to the ground than a fair-weather one, and this is the visible half of
    // that.
    float rainDrop = 90.0f;

    // The cloud shape itself. Stretched sideways, because a cloud is wider than it
    // is tall.
    terrain::NoiseShape shape{};

    // Pixels per second the cloud field drifts. Weather that does not move reads
    // as wallpaper.
    float wind = 16.0f;

    // Pixels per second the base shape travels through the third axis of its own
    // noise, which is what makes a cloud build and come apart rather than only
    // arrive.
    //
    // Drift alone was not enough, and it is worth saying why: a field that only
    // slides is a stencil, and the eye tracks the shape through it and sees a
    // cutout on a conveyor. Nothing about the silhouette ever changed. Depth is the
    // one axis that changes it, since the world has no third dimension for the
    // field to be scrolled along — see terrain::NoiseShape::offsetZ.
    //
    // One feature of the base shape is a couple of hundred pixels, so this wants to
    // be a few pixels a second: enough that a cloud is a different cloud by the time
    // it has crossed the screen, and no more. Above ten the sky churns.
    float evolve = 6.0f;

    // A broad slow field that ripples the cover from place to place, so the sky is
    // not one flat sheet at whatever level the weather set.
    //
    // Demoted on purpose. This used to decide whether it rained, which is what made
    // rain a property of the map instead of an event.
    terrain::NoiseShape front{};
    float frontWind = 6.0f;

    // Gusts, as a wave that travels rather than a value that varies in place.
    //
    // The cloud drift above is a constant, which is all a cloud needs: a sheet
    // moving at one speed reads correctly because nothing on it is fixed to the
    // ground. Anything that *is* fixed — a tree — needs the other half, because
    // what a gust looks like from the ground is not every tree bending at once
    // and not each one twitching on its own schedule. It is a wave crossing the
    // wood: the near trees go over, then the ones behind them, then the far side.
    //
    // So the field is read at `x/wavelength - t*speed`, which is a shape that
    // holds together and moves. The speed is its own, and faster than the air,
    // the way a wave on water outruns the water.
    struct Gust {
        // Share of the mean the gust adds and takes away. At zero the wind is a
        // constant and nothing sways out of step with anything else.
        float strength = 0.75f;

        // Pixels one gust spans, and pixels per second the pattern travels.
        // A wavelength near a screen is what makes a gust something that arrives.
        float wavelength = 900.0f;
        float speed      = 150.0f;

        terrain::NoiseShape shape{.frequency = 1.0f, .octaves = 2, .seed = 5507};
    };

    Gust gust{};

    // How far the front and the climate may push the weather's own cover, either
    // way. Both small: they texture the sky, they do not overrule the weather. A
    // storm has to stay overcast everywhere in it.
    float frontInfluence    = 0.14f;
    float humidityInfluence = 0.10f;

    // The lobes: a Worley at several times the base frequency, inverted so its cells
    // read as bumps rather than as walls, and added to the Perlin.
    //
    // It has to be a *finer* field than the base or there are no lobes to speak of —
    // cells the size of the cloud only move the outline about. This is what turns a
    // swell into a cauliflower.
    terrain::NoiseShape lobes{};
    float worleyMix = 0.35f;

    // How the lobes travel relative to the deck carrying them, in pixels per
    // second. Added to the wind rather than replacing it, so the churn stays put
    // when the wind is retuned.
    //
    // Worley is two-dimensional and has no depth to be moved through, so this is
    // its share of what `evolve` does for the base shape: bumps that crawl over the
    // swell instead of being painted on it. Mostly vertical, and deliberately.
    // Sideways is the direction the eye has already been told means wind, so a
    // large horizontal difference reads as texture sliding under a stencil —
    // whereas nothing else in the sky moves up, so the eye reads that as the cloud
    // convecting, which is what a cumulus does. Y grows downward, so a negative
    // second term climbs.
    Vector2 lobeCrawl = {-3.0f, -5.0f};

    // The finer Worley that eats into the silhouette, and how deep it bites.
    //
    // This is the erosion pass, and it is what turns a smooth outline into a rim of
    // small lobes. Sampled only within `erosionBand` of the cutoff, since deep inside
    // a cloud it cannot change the outcome and outside it there is no outline to
    // erode — which is most of the cost of the field, skipped.
    terrain::NoiseShape detail{};
    float erosion     = 0.0f;
    float erosionBand = 0.16f;

    // The same for the erosion, and about twice as fast. Small features change
    // faster than large ones, so the rim should boil while the lobes only roll.
    Vector2 detailCrawl = {4.0f, -11.0f};

    // How much coarser than the world's lattice the cloud field is sampled, as a
    // multiple of it.
    //
    // A cloud is some four hundred pixels across, so at two this is still thirty-odd
    // samples over one of them, and the rasteriser interpolates between them anyway.
    // The shape does not coarsen; only the sampling of it does, and the field is by
    // far the most expensive thing in the sky.
    float fieldStep = 2.0f;

    // Margin over which the outermost edge of a cloud softens, in field units.
    float softness = 0.13f;

    // How sharply the band thins towards its top and bottom, as an exponent on the
    // profile across it. Zero is a slab; one is a clean sine; above one the deck
    // pulls in to a thinner ribbon through the middle.
    //
    // Applied to the *cover* rather than as a penalty on the cutoff, and that is not
    // a detail. A fixed penalty is a fixed number of field units, so a heavily
    // covered sky — where the cutoff is already low — simply overwhelms it: the deck
    // fills the band to its boundary and ends on a ruled horizontal line at the top
    // and bottom. Thinning the cover instead means the edge always runs out to
    // nothing, however full the sky is, because a cover of zero has no cloud in it
    // by definition.
    float bandTaper = 0.85f;

    // Share of the daylight the thickest cloud holds back, in [0,1]. Scaled by the
    // mood's own `shade`.
    //
    // It has to be read against the exposure curve rather than as a brightness.
    // Light reaches the screen through 1 - exp(-value * exposure), and daylight sits
    // so far up that curve that most of a reduction in radiance is compressed away:
    // holding back half of it leaves the ground 4% darker on screen, three quarters
    // of it 22% darker. A figure that looks drastic written here is a moderate
    // shadow to look at, which is why the useful range is so close to the top.
    float shade = 0.78f;

    // Rain, as it falls.
    //
    // The colour is the pale end the drop is mixed towards; the drop is always
    // *lighter* than the air behind it, which is how rain reads against a sky of any
    // brightness and is what keeps it working once there is a night to fall through.
    Color rainLine  = {216, 234, 255, 255};
    float rainSpeed = 620.0f;

    // How much harder the wind blows on a drop than on a cloud.
    //
    // A cloud drifts at `wind`; a falling drop is in faster air and is much lighter,
    // so it is carried far more. Without this the slant is a degree and a half and
    // invisible. The slant follows the wind's own sign and size, so when there is a
    // gale the rain leans into it without anything being told to.
    float rainDrift = 11.0f;

    // Length of the middle-sized drop, and how far either side of it the gauges
    // spread. Varied per drop, because rain of one gauge is a comb.
    //
    // Length, speed and opacity only. Every drop is one square of the world's
    // lattice wide, and that is not a gauge worth varying: at this size one step
    // wider is twice as wide, and a drop as broad as a third of its own length
    // stops reading as rain.
    float rainLength = 24.0f;
    float rainSpread = 0.55f;

    // Streaks per thousand pixels of width at full rain.
    float rainDensity = 240.0f;

    // Distance a drop falls before it starts again at the cloud, in pixels.
    //
    // A fixed distance, and deliberately not the gap between the cloud and the
    // ground beneath it. That gap depends on how low the cloud is hanging, which
    // depends on the weather, so a drop's cycle would stretch as a shower passed and
    // its speed would fall away with the rain. A drop should fall at the speed a
    // drop falls at whatever the weather is doing.
    float rainSpan = 1400.0f;
};

// Everything about a column of sky that does not depend on the height in it.
//
// Split out because it is much the expensive half — it reads the front, the
// climate and the shape of the ground beneath, seven noise fields in all — and it
// is the same for every sample in the column. A cloud grid over one screen is a
// couple of hundred columns and tens of thousands of samples, so paying for this
// once per column instead of once per sample is the difference between the sky
// being free and it being the most expensive thing in the frame.
struct Column {
    // Share of this stretch of sky that is filled, in [0,1]: the weather's own
    // cover, rippled by the front and the climate beneath.
    float cover = 0.0f;

    // The cloud field has to stand above this to be cloud at all here.
    float cutoff = 1.0f;

    // How hard it is raining, in [0,1].
    //
    // The weather's, not the column's. It used to be measured from this column's own
    // cloud thickness, which is what made one cluster rain while its neighbour in the
    // same weather stayed dry.
    float rain = 0.0f;
};

// The ground the rain lands on: the world Y of the first solid surface in each of
// a run of columns, one entry per column of the world's own lattice.
//
// A view over what the caller already holds, not a copy of it. Passed in rather
// than asked for, because the world knows about the weather and the weather must
// not have to know about the world — and because "the first solid surface" is the
// world as it actually is, edits and all, which is a question only the world can
// answer. The shape of the land alone is not enough: a roof is not a function of
// the column it stands over.
struct Ground {
    const float *top = nullptr;
    int count        = 0;

    float originX = 0.0f; // World X of `top[0]`.
    float spacing = 1.0f;
};

// The sky, configured against the world it stands over.
class Sky {
public:
    // Measures the cloud field's own distribution as well as storing the settings,
    // so that `Settings::cover` means the share of sky it says it does.
    void Configure(const Settings &settings, const terrain::Settings &terrain);

    // Drifts the fields and the clock. The only mutable state in the whole module
    // is the one number; the weather below is derived from it and cached because
    // every column would otherwise ask for the same answer.
    void Advance(float dt);
    float Time() const { return time_; }

    const Settings &Config() const { return settings_; }

    // The weather right now, blended across a change of mood. This is the top of the
    // dependency chain: everything else in the sky reads it.
    const Weather &Now() const { return now_; }

    // The weather at any moment, without moving the clock. Pure, so a forecast and a
    // replay agree with what is drawn.
    Weather WeatherAt(float seconds) const;

    // The time of day right now, and at any moment. The same pair, and the same
    // promise: everything in `Daylight` follows from the argument alone.
    const Daylight &Today() const { return today_; }
    Daylight DaylightAt(float seconds) const;

    float SecondsPerDay() const;

    // How damp the ground is over a column, in [0,1].
    //
    // The climate the place has, raised by rain that has fallen and lowered by
    // daylight that has stood on it since. A game rule's number: crops that will
    // not set in a drought, a fire that will not take, ground that stays slick.
    //
    // Has memory and keeps no state, which is only possible because the weather is
    // a pure function of the clock — so how much it has rained lately can simply be
    // asked of the past rather than accumulated through it.
    float HumidityAt(float worldX) const;

    // Runs the day forward to its next quarter — midnight, dawn, noon or dusk.
    //
    // For looking at the thing rather than waiting for it. Eased in over a few
    // seconds rather than jumped, because what is usually being checked *is* the
    // transition, and a jump lands on the far side of it. Asking again while one is
    // running queues another.
    //
    // Moves the day alone. The clock the clouds and the weather run on is untouched,
    // so the sky does not tear sideways and the afternoon's storm still arrives when
    // it was going to.
    void SkipToQuarter();

    Column ColumnAt(float worldX) const;

    // How far the cloud field stands above the cutoff at a point of sky. Positive
    // inside a cloud, negative outside, and the value the layers are thresholded
    // against. The column is passed in so a caller walking a grid can hoist it out
    // of the inner loop.
    float MarginAt(Vector2 world, const Column &column) const;

    // The same, softened into [0,1]. What "how much cloud is here" means when the
    // answer has to be a share.
    float DensityAt(Vector2 world, const Column &column) const;

    // Share of the sky a column's cloud fills, which is what casts the shadow.
    float CoverAt(float worldX) const;

    // World Y the lowest cloud over a column stops at: where a drop of rain leaves
    // the sky. The bottom of the band when the column is clear, so the answer is
    // always a height and no caller has to carry a second case.
    //
    // Neither this nor CoverAt can be had from the other, though both march the
    // same band. CoverAt wants the thickest cloud anywhere in the column and so has
    // to read all of it; this wants the first edge going up and stops there, which
    // over an overcast sky is a third of the march.
    float UndersideAt(float worldX) const;

    // Share of the daylight a column's cloud holds back. This is what the light
    // solver is given.
    float ShadeAt(float worldX) const;

    // How hard it is raining in a column, in [0,1].
    float RainAt(float worldX) const;

    // Wind at a world position, in pixels per second, signed.
    //
    // The mean the clouds drift at, plus a gust travelling across the world — see
    // Settings::Gust for why a gust has to travel. Scaled by how unsettled the
    // weather is, so a storm blows and a clear afternoon does not.
    //
    // Read by anything rooted to the ground. The clouds and the rain deliberately
    // do not read it: they take the mean straight from the settings, because a
    // cloud is not fixed to anywhere and a slant that varied per column would
    // need the drop's whole path solved rather than its angle. Wiring the gust
    // into rain later means sizing RainReach from the *largest* gust rather than
    // from the mean, or the ground buffer runs out from under the slant.
    float WindAt(float worldX) const;

    // The strongest a gust can pull, in pixels per second. What a caller sizes a
    // sway or a margin against, so nothing has to guess at the envelope.
    float WindReach() const;

    // The same gust as a share of the strongest one there can be, in [-1,1].
    //
    // What everything blown about by the wind should actually read, and the reason
    // it exists as one function: the two lines that turn a speed into a share were
    // written out at every call site — the grass, the crowns, each kind of leaf —
    // and a rule spelled out in five places is a rule that will one day mean five
    // different things. Nothing outside this class should divide by WindReach.
    float PushAt(float worldX) const;

    // How unsettled the weather is, in [0,1].
    //
    // Cover and rain together. Either alone gets it wrong in a way that shows:
    // cover on its own has an overcast afternoon blowing as hard as a storm, rain
    // on its own leaves a dry gale perfectly still.
    float Bluster() const;

    // Where the year has got to.
    //
    // A hook and not yet a calendar: there is no year in this world, so this
    // answers spring and a blend of nothing until `Day::yearDays` is given a
    // value. It is here rather than waiting because everything downstream of a
    // season — a palette, a rate of leaf fall, a window in which fruit sets — can
    // then be written, exercised and looked at now, and turning the seasons on
    // later is a change to this function body alone.
    //
    // A pure function of the clock, like every other answer in this module, so
    // two views of the same world agree about what time of year it is.
    struct Season {
        int index   = 0;    // 0 spring, 1 summer, 2 autumn, 3 winter.
        float blend = 0.0f; // How far into the turn towards the next one, in [0,1].
    };

    Season Turn() const;

    // Overrides the season until it is cleared, for looking at one rather than
    // waiting a year for it. Negative clears.
    void ForceSeason(int index) { forcedSeason_ = index; }
    int ForcedSeason() const { return forcedSeason_; }

    // Holds one kind of weather, for looking at one rather than waiting for it.
    //
    // The same hook ForceSeason is, and it exists for the same reason. Which
    // weather is blowing is a pure function of the spell index and the world seed
    // — deliberately, so the sequence never has to be stored — and the only way to
    // see a storm is therefore to wait for the sky to offer one. That is no way to
    // judge what a storm does, and everything a storm does is something to be
    // judged by eye: the rain, the shade, the gusts, and everything blowing about
    // in them.
    //
    // Negative hands the sky back to its own sequence, which is the state it
    // starts in and the one the world is actually played in.
    void ForceMood(int index) { forcedMood_ = (index >= 0) ? (index % kMoodCount) : -1; }
    int ForcedMood() const { return forcedMood_; }

    // Steps through the moods and then back to the sky's own weather, which is one
    // more stop than there are moods.
    void CycleMood() { forcedMood_ = (forcedMood_ + 2 > kMoodCount) ? -1 : forcedMood_ + 1; }

    // What is blowing right now, by name. The forced mood where one is held, and
    // otherwise whatever the spell is doing — including the name of the weather it
    // is crossing into.
    const char *MoodName() const { return now_.name; }

    // What a place gets on average: daylight over one whole turn of the day, and
    // rain over the moods in the table weighted by how often each comes up.
    //
    // Measured from the settings rather than written down, for the same reason
    // every cutoff in this project is. They exist because anything that has to
    // account for time it did not watch needs to know what it missed — a plant
    // growing while the player was elsewhere, a pool drying out — and a guess at
    // these makes the unwatched world quietly run at a different speed from the
    // watched one.
    float MeanDaylight() const;
    float MeanRain() const;

    // Lowest a cloud base ever hangs, which is where any shadow it casts begins.
    // The light solver is told this so the shade lands under the cloud and not on
    // it.
    float ShadeBelow() const { return settings_.base + settings_.rainDrop; }

    // Colour of the air at a height, cloud cover included.
    Color AirAt(float worldY, float cover) const;

    // Colour a cloud takes at a light level, from the mood's own two lights.
    //
    // Public because it is the model of what a cloud looks like rather than a detail
    // of one way of drawing it: anything else that draws a cloud should read this
    // instead of inventing a second palette that drifts from it.
    Color CloudTint(float lit) const;

    // Light level band `index` of `count` starts at. Bands are spaced by the light
    // they carry rather than evenly, which is what keeps the steps even to look at:
    // the eye reads brightness closer to logarithmically than linearly.
    float BandLight(int index, int count) const;

    // Light reaching a point in the cloud, in [0,1].
    //
    // `depthToSun` is how much cloud lies between the point and the sun; `here` is
    // how much cloud is at the point itself. Beer-Lambert takes the first, the
    // powder term takes the second, and they are not interchangeable.
    float Lighting(float depthToSun, float here) const;

    // The gradient, drawn as flat bands across the view. Replaces clearing the
    // frame rather than being drawn over it.
    void DrawAtmosphere(Rectangle view) const;

    // The stars.
    //
    // Drawn *after* the light rather than under it, and this is the whole reason
    // they read at all. A star is a light, not a lit surface: inside the multiply it
    // can never come out brighter than the night sky's own radiance, which at
    // midnight is about a tenth — so every star was a grey square on a nearly black
    // ground, and its colour went with its brightness.
    //
    // The price is that the two things that should hide one no longer do it by being
    // drawn on top. Both are asked instead: the ground comes in as `ground`, and the
    // cloud is read out of the field at the star's own position.
    void DrawStars(Rectangle view, const Ground &ground) const;

    // The cloud band, drawn in world space through the same rasteriser the world
    // uses, so it belongs to the same picture. Costs nothing when the band is out
    // of view, which underground it always is.
    void DrawClouds(Rectangle view, int spacing) const;

    // How far past the edge of a view the rain has to be prepared for, in pixels.
    //
    // A slanted drop starting off the side of the screen still crosses it further
    // down, so the stretch of ground the rain can land on is wider than the view.
    // Exposed so a caller assembling that ground covers exactly what will be drawn
    // over rather than guessing at a margin.
    float RainReach() const;

    // Rain, as streaks from the cloud base down to whatever stops them.
    void DrawRain(Rectangle view, const Ground &ground) const;

private:
    // Which way a drop falls: down at its own speed, pushed sideways by the wind.
    // One place, because the reach and the drawing have to agree about the slant.
    Vector2 RainFall() const;

    // Cutoffs the cloud field has to clear, one per step of `cover` from empty sky
    // to full.
    //
    // Measured from the field when the sky is configured, the same way ore veins and
    // cave regions are, and for the same reason: a share of something has to be
    // measured against the distribution of the field that decides it, or the number
    // in the settings is a statement about Perlin noise rather than about the sky.
    // Perlin noise is nothing like uniform — it crowds hard around its midpoint — so
    // a cutoff of one minus the cover leaves a sky asked for a third full reading as
    // very nearly empty.
    static constexpr int kCutoffSteps = 33;

    // Cutoff for a share of sky, interpolated between the measured steps.
    float Cutoff(float cover) const;

    // The cloud field itself. One place, so the calibration and the drawing cannot
    // disagree about what is being measured.
    float Field(Vector2 world) const;

    // One pass over the cloud grid, shading each cell from its own depth and from
    // the cloud between it and the sun.
    //
    // Its own rasteriser rather than marching_squares::DrawPixelated, because that
    // one takes a single colour for a whole call and the entire point here is that
    // the colour is decided per cell. It follows the same world anchoring and the
    // same run batching, and it has to: a cloud drawn on a grid of its own would not
    // line up with the ground.
    void DrawShaded(const Grid &field, Vector2 towards, int bands) const;

    // The mood a spell of weather has, drawn from the spell's index. A pure function
    // of it, so the sequence is the same every run of the same world.
    Mood MoodOfSpell(long spell) const;

    // Just the daylight share at a moment: the sine and the ease across it, without
    // the sun's colour or its direction.
    //
    // Split out for the reckoning of how long the ground has stood in the sun, which
    // asks for it two dozen times a frame and would otherwise pay for three
    // exponentials it throws away every time.
    float SunLightAt(float seconds) const;

    // Works out how wet the world is and how long it has stood in the sun, by
    // reading the clock backwards. Called once from Advance; both answers are the
    // same everywhere, so nothing else has cause to.
    void Reckon();

    Settings settings_{};
    terrain::Settings terrain_{};

    float cutoff_[kCutoffSteps]{};

    // Field value the erosion pass is centred on: the cutoff for ordinary weather.
    // Measured in Configure, because the erosion has to know which band counts as
    // near the edge before it can be applied at all.
    float erosionAt_ = 0.5f;

    float time_ = 0.0f;

    // How far the day has been run on ahead of that clock, and how much of a skip is
    // still to be paid out.
    //
    // The day rides on its own offset rather than on `time_` itself, so running it
    // forward does not drag the cloud field sideways with it — six hours at this
    // wind is several screens of drift and a different spell of weather. Wrapped to
    // one day, so a long session cannot walk it out of float precision.
    float dayOffset_ = 0.0f;
    float daySkip_   = 0.0f;

    // Which season is being held, or negative for whatever the clock says.
    int forcedSeason_ = -1;
    int forcedMood_   = -1;

    // Derived from the clock by Advance. Held rather than recomputed because every
    // column asks for the same answer.
    Weather now_{};
    Daylight today_{};

    // How wet the world is and how long it has stood in the sun, both reckoned
    // backwards over the last quarter hour.
    //
    // Functions of the moment alone — the same everywhere — so they are worked out
    // once a frame here and not once per column that asks. What varies from place to
    // place is the climate they are added to, and that is read at the query.
    float wet_     = 0.0f;
    float drought_ = 0.0f;
};

} // namespace weather
