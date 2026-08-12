// Contact sheet of drawn plants, and the sizes they came out.
//
// The art in this project is generated rather than authored, so the only way to
// judge it is to look at a lot of it at once. In the world a tree is thirty
// texels tall behind a hillside at whatever the light is doing, and the question
// being asked — did the notches come out as notches, are there enough greens, is
// this the same tree eight times — is a question about the image.
//
// Neither a window nor a graphics device is involved: canopy::Render works on
// the CPU, and raylib's ExportImage is stb_image_write. So this runs anywhere and
// needs nothing running.
//
// It also measures, because the eye is not reliable about this. Twice during the
// first pass a crown was judged too narrow by looking at it and turned out to be
// within a texel of the width the table asked for; what was actually wrong was
// something else. The table below settles that in one line.

#include "canopy.h"
#include "config.h"
#include "flora.h"
#include "raylib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Behind the plants. A mid blue-grey rather than white or a chequerboard: what is
// being judged is a canopy, and a canopy reads differently against every
// background. This one is near the sky the game actually draws.
constexpr Color kBackdrop = {116, 156, 176, 255};

void Blit(Image &sheet, const std::vector<Color> &pixels, int w, int h, int originX, int originY, int zoom) {
    auto *canvas = static_cast<Color *>(sheet.data);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const Color colour = pixels[static_cast<std::size_t>(y) * w + x];
            if (colour.a == 0) continue;

            for (int qy = 0; qy < zoom; qy++) {
                for (int qx = 0; qx < zoom; qx++) {
                    const int px = originX + x * zoom + qx;
                    const int py = originY + y * zoom + qy;

                    if (px < 0 || py < 0 || px >= sheet.width || py >= sheet.height) continue;

                    canvas[static_cast<std::size_t>(py) * sheet.width + px] = colour;
                }
            }
        }
    }
}

const char *StageName(flora::Stage stage) {
    switch (stage) {
    case flora::Stage::Sapling: return "sapling";
    case flora::Stage::Young: return "young";
    case flora::Stage::Mature: return "mature";
    default: return "old";
    }
}

const char *SeasonName(flora::Season season) {
    switch (season) {
    case flora::Season::Spring: return "spring";
    case flora::Season::Summer: return "summer";
    case flora::Season::Autumn: return "autumn";
    default: return "winter";
    }
}

} // namespace

int main(int argc, char **argv) {
    int perSpecies    = 6;
    int zoom          = 3;
    auto stage        = flora::Stage::Mature;
    auto season       = flora::Season::Summer;
    std::string where = "build/arvores.png";

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool more       = i + 1 < argc;

        if (arg == "-n" && more) perSpecies = std::atoi(argv[++i]);
        else if (arg == "-z" && more) zoom = std::atoi(argv[++i]);
        else if (arg == "-o" && more) where = argv[++i];
        else if (arg == "-s" && more) stage = static_cast<flora::Stage>(std::atoi(argv[++i]));
        else if (arg == "-e" && more) season = static_cast<flora::Season>(std::atoi(argv[++i]));
    }

    perSpecies = (perSpecies < 1) ? 1 : perSpecies;
    zoom       = (zoom < 1) ? 1 : zoom;

    const int cellW = canopy::SlotWidth() * zoom + 6;
    const int cellH = canopy::SlotHeight() * zoom + 6;

    const int rows = static_cast<int>(flora::kSpeciesCount);

    Image sheet = GenImageColor(perSpecies * cellW, rows * cellH, kBackdrop);

    std::vector<Color> pixels;

    std::printf("%s, %s, %d each, %dx\n\n", StageName(stage), SeasonName(season), perSpecies, zoom);
    std::printf("%-8s %-14s %-14s %s\n", "row", "table w x h", "drawn w x h", "covered");

    for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
        const flora::SpeciesDef &def = flora::kSpecies[s];

        long filled = 0;
        long area   = 0;

        for (int i = 0; i < perSpecies; i++) {
            flora::Plant plant;

            plant.species = static_cast<flora::Species>(s);

            // Spread over the sizes the scatter actually rolls, so the row shows
            // the range a wood of them would arrive in rather than one size eight
            // times. The seed is the plant's own, exactly as in the world.
            plant.id = static_cast<std::int64_t>(i) * 7919 + static_cast<std::int64_t>(s) * 104729;
            plant.scale =
                (perSpecies > 1) ? 0.70f + 0.42f * (static_cast<float>(i) / static_cast<float>(perSpecies - 1)) : 1.0f;

            plant.mirrored = (i % 2) == 1;

            int w = 0;
            int h = 0;
            Vector2 anchor{};

            canopy::Render(plant, stage, season, pixels, w, h, anchor);

            // Sat on a common floor line, so the sizes in a row can be read
            // against each other the way they would stand on the ground.
            Blit(sheet, pixels, w, h, i * cellW + (cellW - w * zoom) / 2,
                 static_cast<int>(s) * cellH + cellH - 3 - h * zoom, zoom);

            int minx = w, maxx = -1, miny = h, maxy = -1;

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (pixels[static_cast<std::size_t>(y) * w + x].a == 0) continue;

                    filled++;

                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }


            if (maxx >= minx) area += static_cast<long>(maxx - minx + 1) * (maxy - miny + 1);
        }

        // One more at exactly the size the table names, drawn only to be
        // measured. The row above spreads over the sizes the scatter rolls, so
        // its largest is a plant that rolled big — comparing that against the
        // table would flatter every species by a tenth and hide the fault this
        // column exists to catch, which is a crown that came out narrower than it
        // was asked to be.
        flora::Plant nominal;

        nominal.species = static_cast<flora::Species>(s);
        nominal.id      = 4242 + static_cast<std::int64_t>(s) * 104729;
        nominal.scale   = 1.0f;

        int w = 0;
        int h = 0;
        Vector2 anchor{};

        canopy::Render(nominal, stage, season, pixels, w, h, anchor);

        int minx = w, maxx = -1, miny = h, maxy = -1;

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (pixels[static_cast<std::size_t>(y) * w + x].a == 0) continue;

                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }

        // The table is in world pixels and the drawing is in texels, so the two
        // are only comparable through the plant grid.
        const float pixel = config::kFloraPixel;
        const std::size_t which = flora::StageIndex(stage);

        std::printf("%-8s %6.0f x %-5.0f %6d x %-5d %3.0f%%\n", def.name, def.canopyWidth[which] / pixel,
                    def.height[which] / pixel, (maxx >= minx) ? maxx - minx + 1 : 0,
                    (maxy >= miny) ? maxy - miny + 1 : 0,
                    (area > 0) ? 100.0 * static_cast<double>(filled) / static_cast<double>(area) : 0.0);
    }

    ExportImage(sheet, where.c_str());
    UnloadImage(sheet);

    std::printf("\nrows, top to bottom:");
    for (const flora::SpeciesDef &def : flora::kSpecies) std::printf(" %s", def.name);
    std::printf("\n");

    return 0;
}
