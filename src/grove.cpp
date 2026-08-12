#include "grove.h"

#include "config.h"

#include <algorithm>
#include <cmath>

namespace {

// How far beyond the view plants are grown, in world pixels. Wide enough that a
// tree is drawn into the sheet several frames before it could be seen, at a
// walking pace of a couple of hundred pixels a second.
constexpr float kLead = 240.0f;

// Positions snap to the plant grid, world-anchored, the same way every square in
// the world is placed. Without it a tree drawn at a fractional offset lands its
// texels on different screen pixels from one frame to the next, and the whole
// canopy crawls as the view scrolls.
float Snap(float value) {
    return std::floor(value / config::kFloraPixel) * config::kFloraPixel;
}

} // namespace

void Grove::Configure(const flora::Settings &settings, const terrain::Settings &terrain) {
    settings_ = settings;
    terrain_  = terrain;

    flora::Calibrate(settings_);

    sheet_.Create();
}

void Grove::Unload() { sheet_.Unload(); }

void Grove::ReadGround(const World &world, Rectangle view) {
    const auto spacing = static_cast<float>(world.Spacing());
    const auto &rules  = settings_.layer[flora::LayerIndex(flora::Layer::Canopy)];

    // A canopy hanging out of its cell, the cell itself, and then the reach of
    // the two questions asked either side of a trunk. Asking past the end of the
    // buffer is answered by its edge rather than by anything worse, but a tree
    // placed against a wrong edge value is a tree in the wrong place, so the run
    // is sized to cover every question rather than to be caught by the clamp.
    const float margin =
        flora::Margin(flora::Layer::Canopy, settings_) + rules.cellSpan + rules.slopeSpan + spacing;

    const int first = static_cast<int>(std::floor((view.x - margin) / spacing));
    const int last  = static_cast<int>(std::ceil((view.x + view.width + margin) / spacing));
    const int count = std::max(last - first + 1, 1);

    surface_.assign(static_cast<std::size_t>(count), 0.0f);

    // The skyline and not the surface as built. It is memoised per column, so
    // after one pass over a stretch of world this is a lookup each — and it is
    // the answer that does not move when somebody digs, which is what keeps a
    // wood from rearranging itself around a hole.
    for (int i = 0; i < count; i++) surface_[static_cast<std::size_t>(i)] = world.Skyline(first + i);

    ground_ = {.top     = surface_.data(),
               .count   = count,
               .originX = static_cast<float>(first) * spacing,
               .spacing = spacing};
}

void Grove::Update(const World &world, Rectangle view) {
    ReadGround(world, view);

    // Grown a little wider than the view, so a tree coming over the edge has a
    // frame or two to be drawn into the sheet before anybody could see that it
    // was not there yet.
    flora::Scatter(flora::Layer::Canopy, view.x - kLead, view.x + view.width + kLead, settings_, terrain_, ground_,
                   plants_);
}

void Grove::Draw(flora::Season season) const {
    if (!sheet_.Ready()) return;

    const float pixel = config::kFloraPixel;

    sheet_.Begin();

    for (const flora::Plant &plant : plants_) {
        // Mature until there is growth to say otherwise, which is what an
        // untouched wood is.
        const canopy::Sprite *sprite = sheet_.Acquire(plant, flora::Stage::Mature, season);

        // Nothing yet: the frame's drawing budget is spent and this tree will be
        // there on one of the next few. Skipped rather than drawn some other way,
        // because the only thing worse than a tree arriving a frame late is a
        // different tree standing in for it.
        if (sprite == nullptr) continue;

        Rectangle source = sprite->source;

        // The foot of the trunk is the point a plant is placed by, so mirroring
        // has to move the anchor as well as flip the pixels: what was `anchor`
        // from the left edge is that far from the right one once the source is
        // read backwards.
        const float anchorX = plant.mirrored ? (sprite->source.width - sprite->anchor.x) : sprite->anchor.x;

        if (plant.mirrored) source.width = -source.width;

        const Rectangle target = {Snap(plant.base.x - anchorX * pixel), Snap(plant.base.y - sprite->anchor.y * pixel),
                                  sprite->source.width * pixel, sprite->source.height * pixel};

        DrawTexturePro(sheet_.Texture(), source, target, {0.0f, 0.0f}, 0.0f, WHITE);
    }
}

void Grove::DrawSheet() const {
    if (!sheet_.Ready()) return;

    const Texture2D texture = sheet_.Texture();

    DrawRectangle(0, 0, texture.width, texture.height, {18, 20, 26, 235});
    DrawTexture(texture, 0, 0, WHITE);
    DrawRectangleLines(0, 0, texture.width, texture.height, {120, 200, 140, 255});
}
