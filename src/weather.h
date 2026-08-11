#pragma once

#include "raylib.h"
#include "terrain.h"

// The sky over the world: the air itself, the cloud standing in it, the rain it
// brings and the shade it casts.
//
// Clouds here are not scenery painted behind the world. They are one field with
// three consequences, and all three are read from the same value:
//
//   what is drawn in the sky
//   how much daylight reaches the ground beneath   (light::Medium::cover)
//   whether it is raining, and how hard
//
// Which means the couplings are not wired up after the fact; they cannot come
// apart, because there is only one thing there. What the sky is doing over a
// stretch of world is decided by three inputs, and each is one term:
//
//   the front     a slow broad field drifting past, so weather arrives and passes
//   the climate   wet ground is cloudier than dry ground        (terrain::Climate)
//   the land      high ground is cloudier than low ground       (humidityLift)
//
// And rain feeds back into the cloud that produced it: a raining column hangs
// lower, packs its shading tighter and is drawn darker. Same field, so it costs
// nothing and cannot disagree with itself.
//
// Nothing is simulated. The whole sky is a pure function of position and elapsed
// time, exactly as the terrain is a pure function of position, so there is no
// state to keep and two views of the same world at the same moment agree.
namespace weather {

// Margin value the outside edge of a cloud sits at.
//
// The cloud grid holds a *margin* — how far the cloud field stands above the
// cutoff for the sky it is in — rather than a density clamped into [0,1]. That is
// what lets the same grid be drawn several times at rising thresholds and give
// properly nested shapes; a clamped density saturates at one through the whole
// interior and every threshold above zero would draw the same outline.
inline constexpr float kCloudEdge = 0.0f;

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

// How a cloud takes the light.
//
// The light at a point in a cloud is marched for, not inferred from the outline:
// step from the point towards the sun, add up the cloud passed through, and apply
// Beer's law to what is left. A lobe standing proud of the mass is bright because
// the march out of it is short; the hollow beside it is dark because the march is
// long; the underside is darkest because the whole cloud is above it.
//
// That one rule gives everything a hand-drawn cloud has — the lit crown, the
// shaded belly, the rim where one lobe overlaps the next — without any of them
// being drawn. It also means a heavy cloud is dark *because it is thick*, which is
// a property of the cloud itself and not of where the player happens to stand.
//
// The result is quantised into `layers` levels, so what reaches the screen is
// flat bands of colour rather than a smooth ramp. That is the pixel-art half of
// it, and it is a step at the end rather than the model itself.
struct Shading {
    // How many layers a cloud is built from. Three is a flat sticker, eight is a
    // smooth gradient with no pixel-art character left; five or six reads as a
    // drawn cloud.
    int layers = 6;

    // Share of its outermost extent the innermost layer covers. Small values give
    // a tight bright core and a broad shaded body.
    float coreShrink = 0.74f;

    // Pixels each successive layer is shifted towards the sun.
    float sunOffset = 7.0f;

    // Direction the sun lies in. Y grows downward, so a negative Y is overhead.
    // Need not be normalised.
    Vector2 sun = {0.55f, -0.84f};

    // How much of the light is lost per layer of cloud it passes through.
    //
    // Beer's law: what reaches a layer is exp(-absorption * depth), with depth
    // counted from the sun-facing surface. This is the whole of the shading model
    // and it is why the bands are close together near the light and spread out in
    // the shadow, which is how a real cloud shades and not how a linear ramp does.
    float absorption = 1.9f;

    // The two lights a cloud is under: the sun on it, and the sky all around it.
    // A shadowed part of a cloud is not black, it is sky-coloured, which is the
    // single most important thing about painting one.
    Color sunlight = {255, 250, 240, 255};
    Color ambient  = {132, 152, 190, 255};

    // The ambient a fully raining cloud sits in instead.
    //
    // Needed as its own colour rather than as a darkening of the one above, because
    // the shaded side of a cloud can never be darker than the light falling on it,
    // and a rain cloud's underside plainly is darker than the sky beside it. What
    // changes under a storm is not how much of the ambient reaches the cloud — it is
    // what the ambient is.
    Color rainAmbient = {72, 80, 99, 255};

    // How much of the light a fully raining cloud keeps, and how much tighter its
    // layers pack.
    //
    // Both, because a rain cloud is not merely a grey cloud: it is deeper, so more
    // of it is far from the light, and the shading crowds towards its underside.
    // Packing the layers is what carries that second half.
    float rainDarken = 0.52f;
    float rainPack   = 0.45f;

    // How far a fully laden column is pushed down the layer stack, as a share of
    // the whole stack.
    //
    // This is what makes a cloud's darkness its own rather than the region's. The
    // layers are drawn one shape at a time and take one colour each, so the colour
    // cannot vary along a layer — but the *field* can, and holding a heavy column's
    // field back means fewer of the bright inner layers reach it and more of the
    // dark outer ones show. Per cell, so the heavy cloud goes dark while the wisp
    // beside it does not, whatever the weather is doing to the pair of them.
    //
    // Applied to the shading layers only, never to the outermost. That one draws the
    // silhouette, and a heavy cloud has to be larger than a light one, not smaller.
    float weightSink = 0.75f;
};

struct Settings {
    Atmosphere air{};
    Shading shading{};

    // The band of sky cloud fills. Y grows downward, so `ceiling` is the top of it
    // and `base` the underside. Both are absolute heights, well above
    // `terrain::SurfaceSettings::level`, since cloud that could be walked into
    // would be fog.
    float ceiling = -620.0f;
    float base    = -300.0f;

    // How much lower the underside hangs where it is raining at full strength. A
    // rain cloud sits closer to the ground than a fair-weather one, and this is
    // the visible half of that.
    float rainDrop = 90.0f;

    // The cloud shape itself, sampled as billow noise rather than as plain fbm.
    // Stretched sideways, because a cloud is wider than it is tall.
    terrain::NoiseShape shape{};

    // Pixels per second the cloud field drifts. Weather that does not move reads
    // as wallpaper.
    float wind = 16.0f;

    // A second field, far broader and far slower, deciding whether this stretch of
    // world is clear or overcast at all.
    //
    // Without it the sky holds the same amount of cloud everywhere for ever, and
    // rain stops being an event that arrives and becomes a property of the map.
    terrain::NoiseShape front{};
    float frontWind = 6.0f;

    // Share of the sky filled where the front is neutral and the ground below is
    // of average humidity: the weather this world has by default.
    float cover = 0.28f;

    // How far the front and the climate can push that share either way. The front
    // is the larger of the two on purpose — where you are should colour the
    // weather, not decide it, or half the world would never see rain.
    float frontInfluence    = 0.40f;
    float humidityInfluence = 0.22f;

    // Margin over which the outermost edge of a cloud softens, in field units.
    float softness = 0.13f;

    // How much harder it is to be cloud at the very top and bottom of the band than
    // through the middle of it, in field units.
    //
    // Applied as a penalty on the cutoff rather than as a scale on the field. The
    // difference matters: scaling thins the cloud everywhere at once, so a sky asked
    // for a third full comes out nearly empty, while raising the bar leaves the
    // middle of the band exactly as full as it was asked to be and tapers only the
    // edges — which is the shape a cloud has.
    float bandTaper = 0.14f;

    // Share of the cloud band a column has to be filled with to count as fully
    // laden. Weight is measured against this, so it is the number that decides what
    // "a heavy cloud" means here.
    float weightFull = 0.55f;

    // Weight a cloud needs before it rains, and the weight at which it rains as
    // hard as it can.
    //
    // This is the "cloud favours rain" rule, and it is the only rule there is:
    // rain is not rolled for separately, it is what a thick enough sky does.
    float rainAt   = 0.52f;
    float rainFull = 0.82f;

    // Share of the daylight a fully covered column holds back, in [0,1]. Anything
    // above one is the same as one, which is a sky the daylight does not get through
    // at all.
    //
    // The one number that decides what a cloudy day costs the world below, and it has
    // to be read against the exposure curve rather than as a brightness. Light
    // reaches the screen through 1 - exp(-value * exposure), and daylight sits so far
    // up that curve that most of a reduction in radiance is compressed away: holding
    // back half of it leaves the ground 4% darker on screen, three quarters of it
    // 22% darker. A figure that looks drastic written here is a moderate shadow to
    // look at, which is why the useful range is so close to the top.
    float shade = 0.78f;

    // Rain, as it falls. Speed in pixels per second, length of one streak, and how
    // many streaks per thousand pixels of width at full rain.
    Color rainLine    = {168, 190, 222, 255};
    float rainSpeed   = 620.0f;
    float rainLength  = 22.0f;
    float rainDensity = 90.0f;

    // Distance a drop falls before it starts again at the cloud, in pixels.
    //
    // A fixed distance, and deliberately not the gap between the cloud and the
    // ground beneath it. That gap depends on how low the cloud is hanging, which
    // depends on how hard it is raining, so a drop's cycle would stretch as a shower
    // passed and its speed would fall away with the rain. A drop should fall at the
    // speed a drop falls at whatever the weather is doing.
    //
    // Set well past the deepest valley, so a drop is never seen starting again in
    // mid-air; one that reaches the ground before then simply stops being drawn.
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
    // Share of this stretch of sky that is filled, in [0,1]. Front, climate and
    // elevation, added together.
    float cover = 0.0f;

    // The cloud field has to stand above this to be cloud at all here.
    float cutoff = 1.0f;

    // How much cloud actually stands in this column, in [0,1]: its thickness down
    // the band, which is the water it is carrying.
    //
    // This is the line between the weather and the cloud. `cover` is a property of
    // the region and says how much cloud it gets; this is a property of the cloud
    // and answers differently one lobe to the next.
    float weight = 0.0f;

    // How hard it is raining, in [0,1] — from the weight, so rain falls out of the
    // bottom of a cloud heavy enough to drop it rather than out of a stretch of map
    // that has been declared wet.
    float rain = 0.0f;
};

// The sky, configured against the world it stands over.
class Sky {
public:
    // Measures the cloud field's own distribution as well as storing the settings,
    // so that `Settings::cover` means the share of sky it says it does.
    void Configure(const Settings &settings, const terrain::Settings &terrain);

    // Drifts the fields. The only mutable state in the whole module is this one
    // number.
    void Advance(float dt) { time_ += dt; }
    float Time() const { return time_; }

    const Settings &Config() const { return settings_; }

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

    // Share of the daylight a column's cloud holds back. This is what the light
    // solver is given.
    float ShadeAt(float worldX) const;

    // How hard it is raining in a column, in [0,1].
    float RainAt(float worldX) const;

    // Lowest a cloud base ever hangs, which is where any shadow it casts begins.
    // The light solver is told this so the shade lands under the cloud and not on
    // it.
    float ShadeBelow() const { return settings_.base + settings_.rainDrop; }

    // Colour of the air at a height, cloud cover included.
    Color AirAt(float worldY, float cover) const;

    // How a cloud is built out of layers. Public because this is the model of what a
    // cloud looks like, not a detail of one way of drawing it: DrawClouds is one
    // renderer over these three, and anything else that wants to draw a cloud should
    // read the same description rather than inventing a second one that drifts.
    //
    // Layer zero is the whole cloud; the last is the core facing the sun.
    float LayerMargin(const Column &column, int index, int count) const;
    Vector2 LayerShift(int index) const;
    Color LayerTint(const Column &column, int index, int count) const;

    // The gradient, drawn as flat bands across the view. Replaces clearing the
    // frame rather than being drawn over it.
    void DrawAtmosphere(Rectangle view) const;

    // The cloud band, drawn in world space through the same rasteriser the world
    // uses, so it belongs to the same picture. Costs nothing when the band is out
    // of view, which underground it always is.
    void DrawClouds(Rectangle view, int spacing) const;

    // Rain, as streaks between the cloud base and the ground.
    void DrawRain(Rectangle view) const;

private:
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

    Settings settings_{};
    terrain::Settings terrain_{};

    float cutoff_[kCutoffSteps]{};

    float time_ = 0.0f;
};

} // namespace weather
