#include "debug_view.h"

#include "config.h"
#include "element.h"
#include "marching_squares.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace debug_view {
namespace {

constexpr int kLabelSize = 10;

// Chunk borders, by what the chunk is carrying.
constexpr Color kPlainChunk  = {120, 128, 140, 200};
constexpr Color kEditedChunk = {235, 146, 52, 230};
constexpr Color kFloodedChunk = {56, 152, 236, 230};

// Column of band markers, kept against the left edge of the view. The inset
// leaves room for the height labels, which share that edge and are the scale
// the bars are being read against.
constexpr float kGutterInset = 52.0f;
constexpr float kGutterBar   = 8.0f;
constexpr float kGutterStep  = 14.0f;

// Horizontal step at which a wobbling band edge is resampled. Fine enough that
// the line reads as a curve, coarse enough that a screen's width is a few dozen
// segments rather than a thousand.
constexpr float kEdgeStep = 16.0f;

// A band edge as it should be written down. The sentinel is a height nobody
// will ever stand at and reads as noise printed literally.
const char *BandLevel(float level) {
    if (level <= -kUnboundedDepth) return "sky";
    if (level >= kUnboundedDepth) return "deep";

    return TextFormat("%d", static_cast<int>(level));
}

void DrawLabel(const char *text, Vector2 at, Color color) {
    // Backed with a slab, because the overlay is read against rock, water and
    // sky in turn and no single text colour stays legible on all three.
    const int width = MeasureText(text, kLabelSize);

    DrawRectangle(static_cast<int>(at.x) - 2, static_cast<int>(at.y) - 1, width + 4, kLabelSize + 2,
                  {20, 22, 28, 190});
    DrawText(text, static_cast<int>(at.x), static_cast<int>(at.y), kLabelSize, color);
}

// One edge of a band, followed across the view at its true height.
void DrawBandEdge(const ElementSpawn &spawn, float level, Rectangle view, Color color) {
    if (level <= -kUnboundedDepth || level >= kUnboundedDepth) return;

    float previousX = view.x;
    float previousY = level + BandWobble(spawn, previousX);

    for (float x = view.x + kEdgeStep; x <= view.x + view.width + kEdgeStep; x += kEdgeStep) {
        const float y = level + BandWobble(spawn, x);

        DrawLineV({previousX, previousY}, {x, y}, color);

        previousX = x;
        previousY = y;
    }
}

} // namespace

void ReadToggles(Toggles &toggles) {
    if (IsKeyPressed(KEY_V)) toggles.vertices = !toggles.vertices;
    if (IsKeyPressed(KEY_F3)) toggles.chunks = !toggles.chunks;
    if (IsKeyPressed(KEY_F4)) toggles.layers = !toggles.layers;
    if (IsKeyPressed(KEY_F5)) toggles.light = !toggles.light;

    // L rather than a function key, and beside F5 rather than on it: the two answer
    // opposite halves of the same question and are wanted together. F5 draws what the
    // light *is* and covers the world doing it; this draws where its edges are and
    // leaves the world visible under them, which is the only way to see a shadow
    // moving against a boundary that is not.
    if (IsKeyPressed(KEY_L)) toggles.limits = !toggles.limits;
    if (IsKeyPressed(KEY_F6)) toggles.unlit = !toggles.unlit;
    if (IsKeyPressed(KEY_F7)) toggles.fastWeather = !toggles.fastWeather;

    // F8 is the day skip and F9 turns the season, both actions rather than states
    // and both read by the caller beside the other one. This is F10 because it
    // was F9 and collided with the season: one press did both, and what a player
    // saw was the sprite sheet dropping over the world every time they asked for
    // autumn.
    if (IsKeyPressed(KEY_F10)) toggles.atlas = !toggles.atlas;
    if (IsKeyPressed(KEY_F11)) toggles.bodies = !toggles.bodies;
}

void DrawGroundCollision(const World &world, Rectangle view) {
    const auto step = static_cast<float>(world.Spacing());

    // Walked on the lattice, since that is the grid both answers are built on and
    // a walk on any other one would be sampling the disagreement rather than
    // reporting it.
    const int first = static_cast<int>(std::floor(view.x / step)) - 1;
    const int last  = static_cast<int>(std::ceil((view.x + view.width) / step)) + 1;

    bool had = false;

    float lastSolidX = 0.0f;
    float lastSolidY = 0.0f;
    float lastDrawnX = 0.0f;
    float lastDrawnY = 0.0f;

    for (int i = first; i <= last; i++) {
        const float x = static_cast<float>(i) * step;

        float crossing = 0.0f;
        if (!world.SurfaceOf(x, crossing)) {
            had = false;
            continue;
        }

        // Where the ground is drawn: the crossing rounded onto the texel grid,
        // which is the top edge of the topmost square actually laid down.
        const float drawn = marching_squares::DrawnTop(crossing, config::kPixelSize);

        // And where a body stops: the first lattice vertex a fall would find
        // solid, which is the crossing rounded *up* to the lattice.
        const float solid = std::ceil(crossing / step) * step;

        if (had) {
            DrawLineV({lastDrawnX, lastDrawnY}, {x, drawn}, Fade(SKYBLUE, 0.85f));
            DrawLineV({lastSolidX, lastSolidY}, {x, solid}, Fade(RED, 0.85f));

            // Marked where they actually part by more than a texel, so a screen of
            // agreement stays quiet and the handful of columns that disagree are
            // the thing the eye lands on.
            if (std::fabs(solid - drawn) > config::kPixelSize) {
                DrawLineV({x, drawn}, {x, solid}, Fade(ORANGE, 0.9f));
            }
        }

        lastDrawnX = x;
        lastDrawnY = drawn;
        lastSolidX = x;
        lastSolidY = solid;
        had        = true;
    }
}

// The solved light on its own, in grey, over everything.
//
// Not the light multiplied into the world -- that is the game, and the game is
// exactly what hides the light. A shadow's shape cannot be judged against ground
// that already has a texture, a colour and a shape of its own, and the sky is the
// worst of it: it is drawn by the backdrop and normally shows whatever the backdrop
// felt like, so the one place a cloud's shadow is easiest to read is the one place
// the field was never visible.
//
// Here the field is drawn flat, opaque, across its whole region, sky included, with
// the colour taken out. White is lit, grey is partly shadowed, black is unlit, and
// nothing else is on screen to argue with it. It is the view to take a picture of
// when a shadow looks wrong.
void DrawLight(const World &world, Rectangle view) {
    const light::Field &field = world.Light();

    const Texture2D texture = field.Screen();
    if (texture.id == 0) return;

    // Luminance, so a warm sun and a blue sky do not read as two different amounts of
    // light when they are the same amount of light.
    static Shader grey  = {};
    static bool attempted = false;

    if (!attempted) {
        attempted = true;

        grey = LoadShaderFromMemory(nullptr, R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;

void main() {
    vec3 lit = texture(texture0, fragTexCoord).rgb;
    float value = dot(lit, vec3(0.2126, 0.7152, 0.0722));

    finalColor = vec4(value, value, value, 1.0);
}
)");
    }

    const Rectangle source = {0.0f, 0.0f, static_cast<float>(field.Cols()),
                              static_cast<float>(field.Rows())};

    const Rectangle target = {field.Origin().x, field.Origin().y,
                              field.Cols() * field.Spacing(), field.Rows() * field.Spacing()};

    if (grey.id != 0) BeginShaderMode(grey);

    DrawTexturePro(texture, source, target, {0.0f, 0.0f}, 0.0f, WHITE);

    if (grey.id != 0) EndShaderMode();

    // A sparse rule, so a shape has a scale to be read against and the edge of the
    // solved region can be seen for what it is rather than mistaken for a shadow.
    constexpr int kRule = 32;

    const float spacing = field.Spacing();

    for (int i = 0; i < field.Cols(); i += kRule) {
        const float x = field.Origin().x + i * spacing;
        DrawLineV({x, target.y}, {x, target.y + target.height}, {90, 96, 110, 70});
    }

    for (int j = 0; j < field.Rows(); j += kRule) {
        const float y = field.Origin().y + j * spacing;
        DrawLineV({target.x, y}, {target.x + target.width, y}, {90, 96, 110, 70});
    }

    DrawRectangleLinesEx(target, 2.0f, {240, 120, 60, 160});
}

void DrawLightLimits(const World &world, Rectangle view) {
    const light::Field &field = world.Light();

    if (field.Cols() <= 0 || field.Rows() <= 0) return;

    const float spacing = field.Spacing();
    const Vector2 at    = field.Origin();

    const Rectangle region = {at.x, at.y, field.Cols() * spacing, field.Rows() * spacing};

    // --- the probe lattices, coarse first -------------------------------------
    //
    // Coarsest brightest, on the argument the whole overlay rests on: a lattice of
    // spacing 2^n cells re-phased by a jump of two cells has moved by 2^(1-n) of its
    // own period, so the coarse levels are where a jump is a large fraction of the
    // structure and the fine ones are where it is a rounding. If a boundary is being
    // crossed, it is being crossed on one of the lines drawn brightest.
    //
    // Three, and not all of them. Level zero stands its probes two cells apart, which
    // over five hundred columns is a grey wash rather than a grid.
    const std::vector<light::Field::Level> &stack = field.Levels(0);

    const int drawn = std::min<int>(3, static_cast<int>(stack.size()));

    for (int d = 0; d < drawn; d++) {
        const light::Field::Level &level = stack[stack.size() - 1 - d];

        // Down towards the fine levels, so the coarsest reads as the structure and
        // the two under it as the grid it sits in.
        const unsigned char ink = static_cast<unsigned char>(200 >> d);

        const Color line = {120, 210, 255, ink};

        for (int i = 0; i < level.gx; i++) {
            const float x = field.LevelProbe(level, i, 0).x;
            if (x < view.x || x > view.x + view.width) continue;

            DrawLineV({x, std::max(region.y, view.y)},
                      {x, std::min(region.y + region.height, view.y + view.height)}, line);
        }

        // Only for the coarsest, and only across. The y lattice is the same two cells
        // at every level -- halving happens in x alone -- so drawing it three times
        // draws one grid three times.
        if (d > 0) continue;

        for (int j = 0; j < level.gy; j += 8) {
            const float y = field.LevelProbe(level, 0, j).y;
            if (y < view.y || y > view.y + view.height) continue;

            DrawLineV({std::max(region.x, view.x), y},
                      {std::min(region.x + region.width, view.x + view.width), y}, line);
        }
    }

    // --- the cloud deck, and how much of it the region actually holds ---------
    //
    // The cloud is matter, stamped into the medium between the deck's two edges --
    // and the stamp is clipped to the region, `max(top, 0)` and `min(bottom, rows-1)`.
    // So the deck is only as thick as the part of it that fell inside, and a region
    // whose top edge crosses the deck's changes the optical depth of every cloud on
    // screen at once. That is a boundary the player crosses by walking, and neither
    // the sky nor the light says a word about it.
    //
    // Two rectangles: where the deck is, and where it was stamped. When they are the
    // same rectangle the deck is whole. When the dashed one is shorter, it is not.
    const float deckTop    = world.Sky().DeckTop();
    const float deckBottom = world.Sky().DeckBottom();

    if (deckBottom > deckTop) {
        const Rectangle deck = {region.x, deckTop, region.width, deckBottom - deckTop};

        DrawRectangleLinesEx(deck, 1.0f, {160, 200, 255, 120});

        const float stamped = std::max(deckTop, region.y);
        const float ends    = std::min(deckBottom, region.y + region.height);

        if (ends > stamped) {
            DrawRectangleRec({region.x, stamped, region.width, ends - stamped}, {160, 200, 255, 24});
        }
    }

    // --- the region, and the corner that jumps --------------------------------
    DrawRectangleLinesEx(region, 3.0f, {240, 120, 60, 220});

    // The origin in cells rather than in pixels, because the snap is counted in
    // cells and a figure in pixels hides whether it landed on the stride.
    const int originI = static_cast<int>(std::lround(at.x / spacing));
    const int originJ = static_cast<int>(std::lround(at.y / spacing));

    constexpr float kArm = 40.0f;

    DrawLineEx({at.x - kArm, at.y}, {at.x + kArm, at.y}, 3.0f, {255, 255, 255, 230});
    DrawLineEx({at.x, at.y - kArm}, {at.x, at.y + kArm}, 3.0f, {255, 255, 255, 230});

    // --- what none of the above can be read off -------------------------------
    const light::Settings &settings = world.LightSettings();

    const char *lines[] = {
        TextFormat("region  %d x %d cells @ %.0f px   origin cell %d, %d", field.Cols(), field.Rows(),
                   static_cast<double>(spacing), originI, originJ),
        TextFormat("cascades  %d   coarsest %d x %d probes, %d dirs, step %d cells",
                   static_cast<int>(stack.size()), stack.empty() ? 0 : stack.back().gx,
                   stack.empty() ? 0 : stack.back().gy, stack.empty() ? 0 : stack.back().dirs,
                   stack.empty() ? 0 : stack.back().step),
        TextFormat("sky  radiance %.3f %.3f %.3f   horizon %.2f  zenith %.2f",
                   static_cast<double>(settings.sky.radiance.r), static_cast<double>(settings.sky.radiance.g),
                   static_cast<double>(settings.sky.radiance.b), static_cast<double>(settings.sky.horizon),
                   static_cast<double>(settings.sky.zenith)),
        TextFormat("bounce %s   cross blur %s   exposure %.2f", settings.bounce ? "on" : "off",
                   settings.crossBlur ? "on" : "off", static_cast<double>(settings.exposure)),

        // The line to watch while walking. `skipped` is canopies the sheet is not
        // holding, so their shade is not in the boundary condition at all — if it
        // moves off zero as you walk, the daylight is being driven by a draw cache.
        // How much of the deck the region is holding. Short of the whole of it and
        // every cloud on screen is thinner than it looks.
        TextFormat("deck  y %.0f..%.0f   region y %.0f..%.0f   %s   cloud %s", static_cast<double>(world.Sky().DeckTop()),
                   static_cast<double>(world.Sky().DeckBottom()), static_cast<double>(region.y),
                   static_cast<double>(region.y + region.height),
                   (world.Sky().DeckTop() >= region.y && world.Sky().DeckBottom() <= region.y + region.height)
                       ? "whole"
                       : "CLIPPED",
                   world.SkyCover() ? "on" : "off"),
    };

    // Clear of the top of the screen, where the head-up display already is: the
    // region's corner is almost always above the view, so text placed at it clamps to
    // the top and lands on top of that.
    const float textX = std::max(region.x, view.x) + 8.0f;
    float textY       = std::max(region.y, view.y) + 108.0f;

    for (const char *line : lines) {
        DrawText(line, static_cast<int>(textX), static_cast<int>(textY), kLabelSize, {255, 255, 255, 230});

        textY += kLabelSize + 4.0f;
    }
}

void DrawChunks(const World &world, Rectangle view) {
    for (const World::ChunkView &chunk : world.ChunksIn(view)) {
        const bool pinned = chunk.edited || chunk.holdsLiquid;

        // Edited wins over flooded: a chunk that was dug is unrecoverable,
        // while one that is merely wet comes back the moment it drains.
        const Color color = chunk.edited ? kEditedChunk : (chunk.holdsLiquid ? kFloodedChunk : kPlainChunk);

        DrawRectangleLinesEx(chunk.bounds, pinned ? 2.0f : 1.0f, color);

        // Coordinates rather than an index, so a border seen on screen can be
        // matched against the chunk the world is keying on.
        DrawLabel(TextFormat("%d,%d%s%s", chunk.cx, chunk.cy, chunk.edited ? " edited" : "",
                             chunk.holdsLiquid ? " wet" : ""),
                  {chunk.bounds.x + 4.0f, chunk.bounds.y + 4.0f}, color);
    }
}

void DrawLayers(const World &world, Rectangle view) {
    const float step = static_cast<float>(config::kHeightGridStep);

    // The height grid itself: a line every step, brighter every major interval,
    // labelled with the world height it stands at. This is the scale the bands
    // in the element table are written against, so it is what makes a number
    // there something that can be checked by eye.
    for (float y = std::floor(view.y / step) * step; y <= view.y + view.height; y += step) {
        const bool major = std::fmod(std::abs(y), static_cast<float>(config::kHeightGridMajor)) < step / 2.0f;

        DrawLineV({view.x, y}, {view.x + view.width, y}, major ? Color{90, 96, 110, 180} : Color{90, 96, 110, 70});

        if (major) DrawLabel(TextFormat("y %d", static_cast<int>(y)), {view.x + 4.0f, y + 2.0f}, RAYWHITE);
    }

    // The surface, which is not a band but is the height every band is placed
    // relative to when the table is written.
    const terrain::Settings &settings = world.Settings();

    DrawLineV({view.x, settings.surface.level}, {view.x + view.width, settings.surface.level}, {120, 200, 120, 200});
    DrawLabel("surface level", {view.x + view.width - 90.0f, settings.surface.level + 2.0f}, {160, 220, 160, 255});

    // Then one marker column per generated material, each measured against that
    // scale. Shallowest first, so the column of bars reads top to bottom in the
    // order the materials are met on the way down.
    std::vector<const ElementDef *> banded;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (def.spawn.generator == Generator::None || !def.spawn.band.Bounded()) continue;

        banded.push_back(&def);
    }

    std::sort(banded.begin(), banded.end(),
              [](const ElementDef *a, const ElementDef *b) { return a->spawn.band.top < b->spawn.band.top; });

    // Labels are kept in one column to the side of every bar, rather than each
    // beside its own. Bands overlap in depth by design, so a label placed at
    // its own band's top lands on top of its neighbour's as soon as two bands
    // start above the screen.
    const float labels = view.x + kGutterInset + banded.size() * kGutterStep + 6.0f;
    float taken        = view.y;

    float gutter = view.x + kGutterInset;

    for (const ElementDef *def : banded) {
        const Color color = def->contour;

        DrawBandEdge(def->spawn, def->spawn.band.top, view, color);
        DrawBandEdge(def->spawn, def->spawn.band.bottom, view, color);

        // Clipped to the view, so a band running off the top or bottom of the
        // screen still shows as a bar reaching the edge rather than vanishing.
        const float top    = std::max(def->spawn.band.top, view.y);
        const float bottom = std::min(def->spawn.band.bottom, view.y + view.height);

        if (bottom > top) {
            // Filled by abundance rather than flat, so the bar shows where the
            // material is actually dense instead of merely where it is allowed.
            // The old flat bar was the overlay agreeing with a rule the generator
            // no longer follows.
            for (float y = top; y < bottom; y += 2.0f) {
                const Vector2 at = {view.x + kGutterInset, y};

                // Asked for even though every band that reaches here is absolute
                // — the list above keeps out anything unbounded, which is what a
                // cover's band is. It is here so that a relative band given
                // bounds later is drawn against the same scale it generates on,
                // rather than silently against sea level.
                const float share = BandAbundance(def->spawn, at, terrain::Depth(at, settings));

                DrawRectangleRec({gutter, y, kGutterBar, 2.0f}, Fade(Body(*def), 0.15f + 0.7f * share));
            }

            DrawRectangleLinesEx({gutter, top, kGutterBar, bottom - top}, 1.0f, color);

            const float at = std::max(top + 2.0f, taken);
            taken          = at + kLabelSize + 4.0f;

            const char *from = BandLevel(def->spawn.band.top);
            const char *to   = BandLevel(def->spawn.band.bottom);

            // Brightened rather than taken as it is. Half the materials here
            // are dark by nature, and a label in coal's own colour is unreadable
            // against the slab it is written on.
            DrawLabel(TextFormat("%s  %s..%s  peak %d", def->name, from, to,
                                 static_cast<int>(def->spawn.band.peak)),
                      {labels, at}, ColorBrightness(Body(*def), 0.55f));
        }

        // The peak, drawn brighter than the edges, since it is the height that
        // now decides where the material is worth digging for.
        DrawBandEdge(def->spawn, def->spawn.band.peak, view, ColorBrightness(Body(*def), 0.5f));

        gutter += kGutterStep;
    }
}

} // namespace debug_view
