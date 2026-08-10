#include "debug_view.h"

#include "config.h"
#include "element.h"

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

    DrawLineV({view.x, settings.skyDepth}, {view.x + view.width, settings.skyDepth}, {120, 200, 120, 200});
    DrawLabel("sky floor", {view.x + view.width - 70.0f, settings.skyDepth + 2.0f}, {160, 220, 160, 255});

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
            DrawRectangleRec({gutter, top, kGutterBar, bottom - top}, Fade(def->fill, 0.75f));
            DrawRectangleLinesEx({gutter, top, kGutterBar, bottom - top}, 1.0f, color);

            const float at = std::max(top + 2.0f, taken);
            taken          = at + kLabelSize + 4.0f;

            const char *from = BandLevel(def->spawn.band.top);
            const char *to   = BandLevel(def->spawn.band.bottom);

            // Brightened rather than taken as it is. Half the materials here
            // are dark by nature, and a label in coal's own colour is unreadable
            // against the slab it is written on.
            DrawLabel(TextFormat("%s  %s..%s", def->name, from, to), {labels, at},
                      ColorBrightness(def->fill, 0.55f));
        }

        gutter += kGutterStep;
    }
}

} // namespace debug_view
