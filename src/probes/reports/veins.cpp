#include "core/config.h"
#include "core/grid.h"
#include "core/registry.h"
#include "probes/report.h"
#include "world/element.h"
#include "world/marching_squares.h"
#include "world/soil.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// `--veins out.png [zoom]` — every inclusion drawn at the width the generator
// actually makes it, in the rock and against a cave wall.
//
// It exists because the ores it is most needed for cannot be found. Diamond's
// probability is 0.00028 and `--ore` reports none at all over a six-thousand-pixel
// sweep of its own level, so "does a diamond read as a diamond" is a question no
// `--probe` picture of the world is going to answer this year. What the sheet does
// is put a vein of each one on the page at the size the row asks for, which is the
// same argument `--critters` makes about a creature nobody has met yet.
//
// **Both cases, side by side, because they are not the same picture.** A vein
// buried in rock is blotches against the mid tones of stone. A vein on the wall of
// a cave is blotches against the *shaded belly* of an overhang — the rock's own
// pass paints the union of everything, so a lump of ore sticking into open air is
// painted rock-coloured first and lit as a face. The second one is the one the
// player actually walks past, because ElementSpawn::wallBias deliberately puts ore
// there, and it is the one where two dark things can collapse into one dark smudge.
//
// What it draws with is the real painter and the real rasteriser — `soil::Paint`
// through `marching_squares::DrawPainted`. What it reproduces is three lines of
// World::PaintChunk: the rock is drawn from the union of both fields and the ore
// from its own, which is what DrawnUnioned says and is the whole mechanism under
// test. A sheet that painted the ore itself would be checking the sheet.
namespace {

// Room around each vein and how much of a row a name gets, in world pixels.
constexpr int kPad  = 16;
constexpr int kName = 84;

// The page. Dark, so that coal and diamond are legible on the same sheet.
constexpr Color kPage = {20, 22, 28, 255};

// The open air a wall-side vein stands against, which has to be neither rock nor
// any ore — the whole point of that panel is which of the three you are looking at.
constexpr Color kAir = {12, 30, 46, 255};

// Which row this is, since the table is an array and a reference into it knows.
Element ElementOf(const ElementDef &def) {
    return static_cast<Element>(&def - kElements);
}

// How wide the generator makes one vein of it, in world pixels. The size of a vein
// is what ElementSpawn::veinCells is written in and this is the only conversion.
float widthOf(const ElementDef &def) {
    return def.spawn.veinCells * static_cast<float>(config::kResolution);
}

// A field whose value crosses `threshold` on a circle and falls away from it at one
// unit per pixel.
//
// The gradient is what makes this honest: marching_squares::Texel::depth is the
// value over the length of the gradient, so a field built at a slope of exactly one
// hands the painter the true distance to the vein's edge. Anything else would draw
// the taper at the wrong width and the sheet would flatter or libel the row.
//
// The lattice covers the whole *panel* and never merely the disc, which is a fault
// this had for exactly one render: sized to the vein, a narrow ore's grid stopped
// short of its own panel, the rock was drawn over part of it and not the rest, and
// every small ore appeared shifted sideways in a panel of the wrong width.
Grid Disc(Rectangle panel, Vector2 centre, float radius, float threshold, int cols, int rows) {
    Grid field({panel.x - config::kResolution, panel.y - config::kResolution}, cols, rows, config::kResolution);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const Vector2 at = field.PointAt(i, j);
            const float away = std::sqrt((at.x - centre.x) * (at.x - centre.x) + (at.y - centre.y) * (at.y - centre.y));

            field.SetValue(i, j, threshold + (radius - away));
        }
    }

    return field;
}

// The rock the vein sits in: everywhere, or stopping at a vertical face through the
// middle of the panel. The same slope of one per pixel, for the same reason.
Grid Stone(const Grid &shape, float threshold, float wallX, bool everywhere) {
    Grid field(shape.Origin(), shape.Cols(), shape.Rows(), shape.Spacing());

    for (int i = 0; i < shape.Cols(); i++) {
        for (int j = 0; j < shape.Rows(); j++) {
            field.SetValue(i, j, everywhere ? threshold + 100.0f : threshold + (wallX - shape.PointAt(i, j).x));
        }
    }

    return field;
}

// What World::Occupancy stores: the strongest of the fields, each measured against
// its own threshold, so the union crosses zero where the nearest of them crosses.
Grid Union(const Grid &a, float aLevel, const Grid &b, float bLevel) {
    Grid field(a.Origin(), a.Cols(), a.Rows(), a.Spacing());

    for (int i = 0; i < a.Cols(); i++) {
        for (int j = 0; j < a.Rows(); j++) {
            field.SetValue(i, j, std::max(a.ValueAt(i, j) - aLevel, b.ValueAt(i, j) - bLevel));
        }
    }

    return field;
}

// What the painter was actually handed, gathered by standing in for it.
//
// marching_squares::DrawPainted works out a texel's depth and normal and hands
// them to whatever painter it was given; a painter that records them and answers
// with nothing is therefore the exact reading the real paint gets, through the
// real routine, with no second copy of the arithmetic to drift. Nothing is
// submitted either — DrawPainted skips a colour with no alpha — so this needs no
// render target.
struct Tally {
    soil::Paint paint;

    long *seen  = nullptr;
    long *drawn = nullptr;
    std::vector<float> *depths = nullptr;

    Color operator()(const marching_squares::Texel &texel) const {
        (*seen)++;
        (*drawn) += soil::Shows(paint, texel) ? 1 : 0;

        depths->push_back(texel.depth);

        return BLANK;
    }
};

// A vein material's field over a window of the world, on the world's own lattice.
//
// Built from World::SpawnValue rather than from a resident chunk, for the reason
// --ore reads the same way: what is being measured is the row's own claim, and a
// hole somebody dug is not part of it.
Grid Window(const World &world, Element what, Rectangle area, int spacing) {
    const int cols = static_cast<int>(area.width) / spacing + 1;
    const int rows = static_cast<int>(area.height) / spacing + 1;

    Grid field({area.x, area.y}, cols, rows, spacing);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) field.SetValue(i, j, world.SpawnValue(what, field.PointAt(i, j)));
    }

    return field;
}

int Run(const probes::Bench &bench) {
    const char *path = bench.argv[2];

    const int zoom = (bench.argc >= 4) ? std::max(1, std::atoi(bench.argv[3])) : 4;

    const ElementDef &stone = kElements[ElementIndex(Element::Rock)];

    // Every panel the same size, set by the widest vein in the table, so that a
    // narrow ore reads as narrow. Panels scaled to fit each vein would hide the one
    // thing the sheet is for: an emerald seam is eighteen pixels and a coal seam is
    // forty-eight, and drawn to the same width they would look alike.
    float widest = 0.0f;
    int veins    = 0;

    for (const ElementDef &def : kElements) {
        if (def.paint.vein.Solid()) continue;

        widest = std::max(widest, widthOf(def));
        veins++;
    }

    if (veins == 0) {
        std::printf("no material is drawn as a vein\n");

        return 1;
    }

    const int panel = static_cast<int>(widest) + kPad * 2;
    const int cellW = kName + panel * 2 + kPad;
    const int cellH = panel;

    // A lattice generous enough for the panel plus the cell either side of it that
    // DrawPainted reads for a square's neighbours.
    const int cols = panel / config::kResolution + 3;
    const int rows = cols;

    RenderTexture2D sheet = LoadRenderTexture(cellW, cellH * veins);

    BeginTextureMode(sheet);
    ClearBackground(kPage);

    int r = 0;

    for (const ElementDef &def : kElements) {
        if (def.paint.vein.Solid()) continue;

        const float radius = widthOf(def) * 0.5f;

        const soil::Paint ore  = soil::For(def, 0);
        const soil::Paint rock = soil::For(stone, 0);

        const float top = static_cast<float>(r * cellH);

        DrawText(def.name, 8, static_cast<int>(top) + cellH / 2 - 5, 10, {236, 240, 248, 255});

        for (int side = 0; side < 2; side++) {
            const bool buried = side == 0;

            const Rectangle at = {static_cast<float>(kName + side * panel), top, static_cast<float>(panel),
                                  static_cast<float>(cellH)};

            DrawRectangleRec(at, buried ? Color{30, 33, 40, 255} : kAir);

            // Drawn in the world's own coordinates and blitted nowhere: the camera
            // puts the panel's corner at the panel's place on the page, so the texel
            // grid the blotches are hashed on is the world's, exactly as in the game.
            const Vector2 centre = {at.x + at.width * 0.5f, at.y + at.height * 0.5f};

            const Grid vein  = Disc(at, centre, radius, def.threshold, cols, rows);
            const Grid host  = Stone(vein, stone.threshold, centre.x, buried);
            const Grid whole = Union(host, stone.threshold, vein, def.threshold);

            // The rock, from the union — so the ore's own outline is painted stone
            // before the ore is asked about, which is what leaves it something to
            // show through. This is World::PaintChunk's exclusion walk, at two ranks.
            marching_squares::DrawPainted(whole, 0.0f, rock, BLANK, stone.paint.texel, at);

            // Then the ore, from its own field alone. See DrawnUnioned.
            marching_squares::DrawPainted(vein, def.threshold, ore, BLANK, def.paint.texel, at);
        }

        r++;
    }

    EndTextureMode();

    Image out = LoadImageFromTexture(sheet.texture);
    ImageFlipVertical(&out);

    if (zoom > 1) ImageResizeNN(&out, out.width * zoom, out.height * zoom);

    const bool wrote = ExportImage(out, path);

    UnloadImage(out);
    UnloadRenderTexture(sheet);

    std::printf("%d veins, %d x %d at zoom %d -> %s\n", veins, cellW, cellH * veins, zoom, path);
    std::printf("left: in rock   right: at a cave wall\n\n");

    std::printf("%-8s %6s %6s %6s %7s %9s %8s %7s %8s %7s\n", "", "width", "share", "fringe", "rim", "depth p90",
                "reaches", "drawn", "texels", "swept");

    // And then the same question asked of the world instead of of a disc, which is
    // the check the sheet cannot make about itself.
    //
    // `depth p90` is what marching_squares::Texel::depth actually comes out at
    // inside a generated vein. It is the field's value over the length of its own
    // gradient — a real distance where a field is a distance and a linearisation
    // where it is a shape, which an ore's noise is. That number decides whether a
    // rim written in pixels means anything at all, and there is no way to guess it:
    // an ore's field crosses its cutoff steeply and flattens off a texel later, so a
    // vein forty-eight pixels across can hand the painter a depth of three.
    //
    // `texels` is how many of them the sweep found, and it is printed for §23.1's
    // reason: a figure taken over forty texels of one lucky diamond is noise, and a
    // report that hid the count would let it be tuned against.
    //
    // `reaches` is the share of the vein at or past the rim — the part drawn at the
    // full density its row asks for. At nothing, the row's `share` is a number that
    // never happens anywhere and `drawn` is the only figure that means anything.
    for (const ElementDef &def : kElements) {
        if (def.paint.vein.Solid()) continue;

        const soil::Paint ore = soil::For(def, 0);

        long seen  = 0;
        long drawn = 0;
        std::vector<float> depths;

        // Along the level the row says the material is densest at, which is where
        // there is any of it to measure — and onward until there is enough of it to
        // be worth reading.
        //
        // A fixed sweep is what this had first and it was wrong by two orders: four
        // thousand pixels of coal is five thousand texels and four thousand pixels
        // of diamond is *thirty-eight*, which is one lucky vein, and every figure
        // taken off it moved when the sweep moved. Walking until the count is up is
        // the only arrangement under which one table can hold an ore found in every
        // hillside and one found twice a county. How far it had to walk is printed
        // as well, because that distance is itself the interesting fact about the
        // rarest rows.
        constexpr long kEnough = 2000;
        constexpr float kStep  = 600.0f;
        constexpr float kTall  = 900.0f;
        constexpr float kGiveUp = 60000.0f;

        float swept = 0.0f;

        for (float x = 0.0f; x < kGiveUp && seen < kEnough; x += kStep, swept = x) {
            const Rectangle area = {x, def.spawn.band.peak - kTall * 0.5f, kStep, kTall};

            const Grid field = Window(*bench.world, ElementOf(def), area, config::kResolution);

            marching_squares::DrawPainted(field, def.threshold,
                                          Tally{.paint = ore, .seen = &seen, .drawn = &drawn, .depths = &depths},
                                          BLANK, def.paint.texel, area);
        }

        if (depths.empty()) {
            std::printf("%-8s %6.0f %5.0f%% %5.0f%% %6.0f %9s %8s %7s %8d %6.0fk\n", def.name, widthOf(def),
                        100.0f * def.paint.vein.share, 100.0f * def.paint.vein.fringe, ore.vein.rim, "-", "-", "-", 0,
                        swept / 1000.0f);
            continue;
        }

        std::sort(depths.begin(), depths.end());

        const float p90 = depths[depths.size() * 9 / 10];

        const long reaches =
            static_cast<long>(depths.end() - std::lower_bound(depths.begin(), depths.end(), ore.vein.rim));

        std::printf("%-8s %6.0f %5.0f%% %5.0f%% %6.0f %9.1f %7.0f%% %6.0f%% %8ld %6.0fk\n", def.name, widthOf(def),
                    100.0f * def.paint.vein.share, 100.0f * def.paint.vein.fringe, ore.vein.rim, p90,
                    100.0 * reaches / std::max<long>(seen, 1), 100.0 * drawn / std::max<long>(seen, 1), seen,
                    swept / 1000.0f);
    }

    return wrote ? 0 : 1;
}

const probes::Report row = {
    .name  = "--veins",
    .wants = 3,
    .shows = false,
    .blurb = "--veins out.png [zoom] - every inclusion at its own width, in rock and at a wall",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
