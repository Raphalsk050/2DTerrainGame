#pragma once

#include "raylib.h"
#include "terrain.h"

// The sky over the world: cloud, the rain it brings, and the shade it casts.
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
// lower and is drawn darker. Same field, so it costs nothing and cannot disagree
// with itself.
//
// Nothing is simulated. The whole sky is a pure function of position and elapsed
// time, exactly as the terrain is a pure function of position, so there is no
// state to keep and two views of the same world at the same moment agree.
namespace weather {

// Field value the edge of a cloud sits at. Clouds are drawn by the same marching
// squares routine the world is, so they need a threshold like any other material.
inline constexpr float kCloudLevel = 0.5f;

struct Settings {
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

    // The cloud shape itself. Stretched sideways, because a cloud is wider than it
    // is tall and an isotropic field gives a row of cotton balls.
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
    float cover = 0.34f;

    // How far the front and the climate can push that share either way. The front
    // is the larger of the two on purpose — where you are should colour the
    // weather, not decide it, or half the world would never see rain.
    float frontInfluence    = 0.40f;
    float humidityInfluence = 0.22f;

    // Field distance over which a cloud's edge softens. Zero gives a shape cut out
    // of paper; too much and the cloud has no edge at all.
    float softness = 0.18f;

    // How much harder it is to be cloud at the very top and bottom of the band than
    // through the middle of it, in field units.
    //
    // Applied as a penalty on the cutoff rather than as a scale on the field. The
    // difference matters: scaling thins the cloud everywhere at once, so a sky asked
    // for a third full comes out nearly empty, while raising the bar leaves the
    // middle of the band exactly as full as it was asked to be and tapers only the
    // edges — which is the shape a cloud has.
    float bandTaper = 0.14f;

    // Cover a stretch of sky needs before it rains, and the cover at which it
    // rains as hard as it can.
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
    //
    // Still short of one. An overcast sky is dimmer than a clear one, not dark, and
    // the ground under a cloud has to stay readable.
    float shade = 0.78f;

    // Colours. A cloud lit from above, the same cloud with rain in it, and the
    // rain itself.
    Color tint     = {238, 242, 250, 255};
    Color rainTint = {120, 130, 152, 255};
    Color rainLine = {168, 190, 222, 255};

    // Rain, as it falls. Speed in pixels per second, length of one streak, and how
    // many streaks per thousand pixels of width at full rain.
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

    // How hard it is raining, in [0,1].
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

    // Cloud density at a point of sky, in [0,1]. The column is passed in so that a
    // caller walking a grid can hoist it out of the inner loop.
    float DensityAt(Vector2 world, const Column &column) const;

    // Share of the sky a column's cloud fills, which is what casts the shadow.
    //
    // Read from the cloud field rather than from `Column::cover`, so the shadow on
    // the ground follows the cloud that casts it and not the weather it belongs
    // to. A shadow that covered the gaps between clouds would not read as cloud at
    // all.
    float CoverAt(float worldX) const;

    // Share of the daylight a column's cloud holds back. This is what the light
    // solver is given.
    float ShadeAt(float worldX) const;

    // How hard it is raining in a column, in [0,1].
    float RainAt(float worldX) const;

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

    Settings settings_{};
    terrain::Settings terrain_{};

    float cutoff_[kCutoffSteps]{};

    float time_ = 0.0f;
};

} // namespace weather
