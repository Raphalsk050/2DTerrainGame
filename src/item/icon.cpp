#include "item/icon.h"

#include "ui/skin.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace {

// Where every authored asset lives. One constant, so a row says
// "blocks/tools/wood_pickaxe" and nothing in any table carries a directory or an
// extension — the same argument `mob::Dressed` makes about `assets/mobs/`.
const char *kUnder = "assets/";

struct Held {
    Texture2D texture{};

    // Whether the load has been attempted, which is not the same as whether it worked.
    // A row with no art and a row whose file is missing must both stop trying, or a
    // missing file is opened sixty times a second for as long as the game runs.
    bool tried = false;

    bool Ready() const { return texture.id != 0; }
};

std::unordered_map<std::string, Held> &Kept() {
    static std::unordered_map<std::string, Held> kept;

    return kept;
}

Texture2D Load(const std::string &path) {
    Texture2D texture = LoadTexture(path.c_str());

    if (texture.id == 0) return texture;

    // The one place in this project that is not point sampled, and it is a deliberate
    // exception rather than an oversight.
    //
    // Everything else drawn from a file — `sheet::Load`, the terrain cache — is drawn
    // at one texel per world pixel, where nearest sampling is not an approximation but
    // the exact answer (CLAUDE.md §5.5). These are the opposite case: a 64-pixel
    // drawing has to fit a 36-pixel slot, so better than half the source is not going
    // to survive whatever is done to it. Nearest picks which pixels live by where the
    // grid happens to fall, which eats the one-pixel black outline the art is drawn
    // with in some columns and keeps it in others; a mip chain averages instead, which
    // is what a smaller copy of a drawing actually is.
    //
    // Generated once here rather than by resaving the assets smaller. The file stays
    // the size it was drawn at, and only the scale it is drawn at moves.
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    return texture;
}

} // namespace

const Texture2D *icon::Art(const ItemDef &def) {
    if (def.art == nullptr) return nullptr;

    Held &held = Kept()[def.art];

    if (!held.tried) {
        held.tried = true;

        held.texture = Load(std::string(kUnder) + def.art + ".png");

        if (!held.Ready()) {
            // Said once, and only once, because `tried` is already set.
            //
            // A warning here rather than a refusal because `item_checks.cpp` has
            // already refused at startup for a path that is not there — anything
            // reaching this line is a file that exists and would not decode, which is
            // a different fault and one the row's own `picture` covers for.
            TraceLog(LOG_WARNING, "item art '%s%s.png' would not load", kUnder, def.art);
        }
    }

    return held.Ready() ? &held.texture : nullptr;
}

void icon::Draw(const Stack &stack, Vector2 at, float pixel) {
    if (stack.Empty()) return;

    const float side = kPictureSide * pixel;

    if (stack.holds == Holds::Item) {
        if (const Texture2D *art = Art(Def(stack.AsItem()))) {
            // The whole canvas onto the whole box, never the drawing's own bounding
            // box onto it. A pickaxe fills 48 pixels of its 64 and a shovel 28, and
            // fitting each to what it happens to occupy would draw the shovel half as
            // wide again as the pickaxe it is meant to hang beside. One window for
            // every tool is what keeps fifteen separate files reading as one set.
            const Rectangle source = {0.0f, 0.0f, static_cast<float>(art->width),
                                      static_cast<float>(art->height)};

            DrawTexturePro(*art, source, {at.x, at.y, side, side}, {0.0f, 0.0f}, 0.0f, WHITE);

            return;
        }
    }

    DrawPicture(PictureOf(stack), at, pixel);
}

void icon::DrawWear(const Stack &stack, Vector2 at, float pixel) {
    if (stack.Empty() || !stack.Wears()) return;

    const float side = kPictureSide * pixel;

    // A twelfth of the box, and never under two screen pixels. Minecraft's is two
    // pixels of a sixteen-pixel slot, which is an eighth — that reads as a band rather
    // than as a bar at this size, where the box is three times as wide.
    const float thick = std::max(2.0f, std::round(side / 12.0f));

    const Rectangle groove = {std::floor(at.x), std::floor(at.y + side - thick), side, thick};

    // Opaque black under it, because the bar is drawn over whatever the slot holds and
    // a coloured line on a coloured picture is not a line. The same argument
    // `hud::Label` and the count in the corner both make.
    DrawRectangleRec(groove, skin::kShadow);

    const float share = std::clamp(stack.Whole(), 0.0f, 1.0f);

    // Green through amber to red as it goes, which is Minecraft's own ramp: the hue
    // runs a third of the way round the wheel and nothing else about the colour moves.
    // One number to read, and it is readable without reading it.
    const Color face = ColorFromHSV(120.0f * share, 0.78f, 0.92f);

    // At least a sliver while there is anything left at all. A tool with one blow in it
    // and a bar that has rounded to nothing is a tool the player believes is spent, and
    // they put it away instead of using the blow they have.
    const float filled = (share > 0.0f) ? std::max(1.0f, std::floor(groove.width * share)) : 0.0f;

    if (filled > 0.0f) DrawRectangleRec({groove.x, groove.y, filled, groove.height}, face);
}

void icon::Discard() {
    for (auto &[path, held] : Kept()) {
        if (held.Ready()) UnloadTexture(held.texture);

        held = {};
    }

    Kept().clear();
}
