// Contact sheet of drawn plants, for judging the art without hunting for a tree
// in the world. No window and no GL: canopy::Render is CPU, and ExportImage is
// stb_image_write.

#include "canopy.h"
#include "flora.h"
#include "raylib.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
    const int perSpecies = (argc > 1) ? std::atoi(argv[1]) : 8;
    const auto stage     = (argc > 2) ? static_cast<flora::Stage>(std::atoi(argv[2])) : flora::Stage::Mature;
    const auto season    = (argc > 3) ? static_cast<flora::Season>(std::atoi(argv[3])) : flora::Season::Summer;

    // A fourth argument lays snow on every crown, which is how the snowfield trees
    // are judged: it is baked into the sprite, so a screenshot of the world is the
    // only other way to see one and a screenshot needs a snowfield to stand in.
    const bool snowy = argc > 4 && std::atoi(argv[4]) != 0;

    const int cellW = canopy::SlotWidth() + 4;
    const int cellH = canopy::SlotHeight() + 4;

    const int columns = perSpecies;
    const int rows    = static_cast<int>(flora::kSpeciesCount);

    Image sheet = GenImageColor(columns * cellW, rows * cellH, Color{116, 156, 176, 255});

    auto *canvas = static_cast<Color *>(sheet.data);

    std::vector<Color> pixels;

    for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
        for (int i = 0; i < perSpecies; i++) {
            flora::Plant plant;

            plant.species = static_cast<flora::Species>(s);

            // Spread over the sizes the scatter actually rolls.
            plant.id    = static_cast<std::int64_t>(i) * 7919 + static_cast<std::int64_t>(s) * 104729;
            plant.scale = 0.72f + 0.38f * (static_cast<float>(i) / static_cast<float>(perSpecies - 1));

            int w = 0;
            int h = 0;
            Vector2 anchor{};

            canopy::Render(plant, stage, season, snowy, pixels, w, h, anchor);

            // Sat on a common floor line so the sizes can be compared.
            const int originX = i * cellW + (cellW - w) / 2;
            const int originY = static_cast<int>(s) * cellH + cellH - 2 - h;

            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const Color c = pixels[static_cast<std::size_t>(y) * w + x];
                    if (c.a == 0) continue;

                    const int px = originX + x;
                    const int py = originY + y;

                    if (px < 0 || py < 0 || px >= sheet.width || py >= sheet.height) continue;

                    canvas[static_cast<std::size_t>(py) * sheet.width + px] = c;
                }
            }
        }

        std::printf("%-7s drawn\n", flora::kSpecies[s].name);
    }

    ExportImage(sheet, "sheet.png");
    UnloadImage(sheet);

    std::printf("wrote sheet.png (%d x %d)\n", columns * cellW, rows * cellH);

    return 0;
}
