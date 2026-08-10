#include "world.h"

#include "marching_squares.h"

#include <algorithm>
#include <cmath>

namespace {

// Chunks are kept this many chunks beyond the view before being released, so
// that walking back and forth across a border does not regenerate them every
// frame.
constexpr int kKeepMargin = 2;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

} // namespace

World::World(const terrain::Settings &settings, int spacing) : settings_(settings), spacing_(spacing) {}

std::int64_t World::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

void World::ToChunk(Vector2 world, int &outCx, int &outCy) const {
    const int span = kChunkCells * spacing_;

    outCx = FloorDiv(static_cast<int>(std::floor(world.x)), span);
    outCy = FloorDiv(static_cast<int>(std::floor(world.y)), span);
}

Vector2 World::SnapToLattice(Vector2 world) const {
    const float step = static_cast<float>(spacing_);
    return {std::round(world.x / step) * step, std::round(world.y / step) * step};
}

void World::LatticeRange(Rectangle region, int &outI0, int &outJ0, int &outI1, int &outJ1) const {
    const float step = static_cast<float>(spacing_);

    outI0 = static_cast<int>(std::floor(region.x / step));
    outJ0 = static_cast<int>(std::floor(region.y / step));
    outI1 = static_cast<int>(std::ceil((region.x + region.width) / step));
    outJ1 = static_cast<int>(std::ceil((region.y + region.height) / step));
}

const World::Chunk *World::Find(int cx, int cy) const {
    const auto it = chunks_.find(Key(cx, cy));
    return (it != chunks_.end()) ? &it->second : nullptr;
}

World::Chunk &World::Emplace(int cx, int cy) {
    const Vector2 origin = {cx * ChunkSpan(), cy * ChunkSpan()};

    Chunk chunk;
    chunk.fields.reserve(kElementCount);
    for (std::size_t e = 0; e < kElementCount; e++) {
        chunk.fields.emplace_back(origin, kChunkVertices, kChunkVertices, spacing_);
    }

    terrain::Fill(chunk.fields[ElementIndex(Element::Rock)], settings_);

    return chunks_.emplace(Key(cx, cy), std::move(chunk)).first->second;
}

void World::Update(Rectangle view) {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            if (Find(cx, cy) == nullptr) Emplace(cx, cy);
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (it->second.edited || it->second.holdsWater) {
            ++it;
            continue;
        }

        int cx = 0;
        int cy = 0;
        ToChunk(it->second.fields[0].Origin(), cx, cy);

        const bool far = cx < minCx - kKeepMargin || cx > maxCx + kKeepMargin || cy < minCy - kKeepMargin ||
                         cy > maxCy + kKeepMargin;

        it = far ? chunks_.erase(it) : std::next(it);
    }
}

void World::Reset() {
    chunks_.clear();
}

float World::ValueAt(Element element, Vector2 world) const {
    const Vector2 vertex = SnapToLattice(world);

    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    const Chunk *chunk = Find(cx, cy);

    // Rock beyond the resident area still answers, from the noise it would have
    // been generated with. Liquid has no such fallback: it is state, not a
    // function of position, so outside the resident area there is none.
    if (chunk == nullptr) {
        return (element == Element::Rock) ? terrain::Density(vertex, settings_) : 0.0f;
    }

    const Grid &field = chunk->fields[ElementIndex(element)];

    int i = 0;
    int j = 0;
    field.ToLocal(vertex, i, j);

    if (!field.InBounds(i, j)) {
        return (element == Element::Rock) ? terrain::Density(vertex, settings_) : 0.0f;
    }

    return field.ValueAt(i, j);
}

void World::WriteVertex(Element element, Vector2 vertex, float value) {
    // A lattice position on a chunk border exists in more than one chunk. Every
    // copy is updated, otherwise a later read could pick the stale one and the
    // amount of liquid in the world would depend on which chunk answered.
    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    for (int dx = -1; dx <= 0; dx++) {
        for (int dy = -1; dy <= 0; dy++) {
            const auto it = chunks_.find(Key(cx + dx, cy + dy));
            if (it == chunks_.end()) continue;

            Chunk &chunk = it->second;
            Grid &field  = chunk.fields[ElementIndex(element)];

            int i = 0;
            int j = 0;
            field.ToLocal(vertex, i, j);

            if (!field.InBounds(i, j)) continue;

            field.SetValue(i, j, value);

            if (element == Element::Water && value > water::kDryMass) chunk.holdsWater = true;
        }
    }
}

bool World::IsSolidAt(Vector2 world) const {
    return ValueAt(Element::Rock, world) > RockThreshold();
}

bool World::OverlapsSolid(Rectangle rect) const {
    const float step = static_cast<float>(spacing_);

    const float left   = rect.x;
    const float right  = rect.x + rect.width;
    const float top    = rect.y;
    const float bottom = rect.y + rect.height;

    for (float x = std::floor(left / step) * step; x <= right + step; x += step) {
        for (float y = std::floor(top / step) * step; y <= bottom + step; y += step) {
            // Vertices occupy a square of one spacing centred on themselves, so
            // only those whose square meets the rectangle count.
            if (x + step / 2.0f < left || x - step / 2.0f > right) continue;
            if (y + step / 2.0f < top || y - step / 2.0f > bottom) continue;

            if (IsSolidAt({x, y})) return true;
        }
    }

    return false;
}

float World::SubmergedFraction(Rectangle rect) const {
    const float step = static_cast<float>(spacing_);

    const float left   = rect.x;
    const float right  = rect.x + rect.width;
    const float top    = rect.y;
    const float bottom = rect.y + rect.height;

    // Measured as the share of the body's height that meets liquid, taking the
    // wettest sample across the width of each row rather than averaging the row.
    //
    // Averaging over the whole rectangle makes a body in a channel narrower
    // than itself read as barely wet, because the dry columns beside it drag
    // the mean down. It then never reaches the swimming threshold and cannot
    // climb a column of water it is plainly standing in.
    double total = 0.0;
    int rows     = 0;

    for (float y = std::floor(top / step) * step; y <= bottom + step; y += step) {
        if (y + step / 2.0f < top || y - step / 2.0f > bottom) continue;

        float wettest = 0.0f;

        for (float x = std::floor(left / step) * step; x <= right + step; x += step) {
            if (x + step / 2.0f < left || x - step / 2.0f > right) continue;

            // Clamped, because a compressed cell holds more than one unit and
            // would otherwise report a body as more than fully submerged.
            wettest = std::max(wettest, std::min(ValueAt(Element::Water, {x, y}), 1.0f));
        }

        total += wettest;
        rows++;
    }

    return (rows > 0) ? static_cast<float>(total / rows) : 0.0f;
}

void World::Paint(Element element, Vector2 world, float radius, bool add) {
    const Rectangle brush = {world.x - radius, world.y - radius, radius * 2.0f, radius * 2.0f};

    int i0 = 0;
    int j0 = 0;
    int i1 = 0;
    int j1 = 0;
    LatticeRange(brush, i0, j0, i1, j1);

    const float step = static_cast<float>(spacing_);

    for (int i = i0; i <= i1; i++) {
        for (int j = j0; j <= j1; j++) {
            const Vector2 vertex = {i * step, j * step};
            if (!CheckCollisionPointCircle(vertex, world, radius)) continue;

            // Painted samples are pinned to the extremes of the range rather
            // than nudged, so a brush stroke reads as a definite edit and not
            // as a faint gradient.
            WriteVertex(element, vertex, add ? 1.0f : 0.0f);

            // Rock is carved and filled by hand, so its chunks stop following
            // the noise. Liquid marks its own chunks through WriteVertex.
            if (element == Element::Rock) {
                int cx = 0;
                int cy = 0;
                ToChunk(vertex, cx, cy);

                for (int dx = -1; dx <= 0; dx++) {
                    for (int dy = -1; dy <= 0; dy++) {
                        const auto it = chunks_.find(Key(cx + dx, cy + dy));
                        if (it != chunks_.end()) it->second.edited = true;
                    }
                }
            }
        }
    }
}

void World::StepWater(Rectangle active) {
    int i0 = 0;
    int j0 = 0;
    int i1 = 0;
    int j1 = 0;
    LatticeRange(active, i0, j0, i1, j1);

    const int cols = i1 - i0 + 1;
    const int rows = j1 - j0 + 1;
    if (cols <= 0 || rows <= 0) return;

    if (scratch_.cols != cols || scratch_.rows != rows) scratch_.Resize(cols, rows);

    const float step = static_cast<float>(spacing_);

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
            const int cell       = scratch_.Index(i, j);

            scratch_.mass[cell]    = ValueAt(Element::Water, vertex);
            scratch_.blocked[cell] = (ValueAt(Element::Rock, vertex) > RockThreshold()) ? 1 : 0;
        }
    }

    water::Step(scratch_, waterSettings_);

    // Every chunk overlapping the region is assumed dry until the write-back
    // says otherwise, so a chunk that drains can be released again instead of
    // staying pinned for the rest of the session.
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ToChunk({active.x, active.y}, minCx, minCy);
    ToChunk({active.x + active.width, active.y + active.height}, maxCx, maxCy);

    // Widened by one chunk because a write to a vertex on the region's border
    // also lands in the chunk before it. Without the margin such a chunk would
    // keep a stale flag and stay pinned in memory after draining.
    for (int cx = minCx - 1; cx <= maxCx + 1; cx++) {
        for (int cy = minCy - 1; cy <= maxCy + 1; cy++) {
            const auto it = chunks_.find(Key(cx, cy));
            if (it != chunks_.end()) it->second.holdsWater = false;
        }
    }

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
            WriteVertex(Element::Water, vertex, scratch_.mass[scratch_.Index(i, j)]);
        }
    }
}

float World::TotalWater(Rectangle region) const {
    int i0 = 0;
    int j0 = 0;
    int i1 = 0;
    int j1 = 0;
    LatticeRange(region, i0, j0, i1, j1);

    const float step = static_cast<float>(spacing_);

    double total = 0.0;
    for (int i = i0; i <= i1; i++) {
        for (int j = j0; j <= j1; j++) {
            total += ValueAt(Element::Water, {i * step, j * step});
        }
    }

    return static_cast<float>(total);
}

// How sharply the liquid is clamped as it approaches rock. Large enough that
// the clamp only bites within a cell of the surface, and the free surface
// elsewhere is decided by the liquid alone.
namespace {
constexpr float kRockClampGain = 6.0f;
} // namespace

Grid World::LiquidRenderField(int cx, int cy, Element element) const {
    const Chunk *chunk = Find(cx, cy);
    const float span   = ChunkSpan();

    if (chunk == nullptr) return Grid({cx * span, cy * span}, 1, 1, spacing_);

    const Grid &mass = chunk->fields[ElementIndex(element)];
    const Grid &rock = chunk->fields[ElementIndex(Element::Rock)];

    // Full size, border column and row included.
    //
    // Cells sit between samples, so the chunk's 33 samples describe 32 cells
    // that tile its span exactly, and the sample shared with the next chunk is
    // what lets the two meet without a gap. Dropping it to avoid drawing the
    // shared sample twice leaves the last cell of every chunk belonging to
    // nobody, which shows up as a lattice of empty lines across the liquid.
    Grid field(mass.Origin(), mass.Cols(), mass.Rows(), spacing_);

    const float threshold = StyleOf(element).threshold;

    for (int i = 0; i < field.Cols(); i++) {
        for (int j = 0; j < field.Rows(); j++) {
            // The liquid is drawn from its own mass and nothing else.
            //
            // An earlier rule filled a cell whenever another one above it held
            // liquid, on the reasoning that liquid settles from the bottom. It
            // held only for settled bodies, where the mass is already full and
            // the rule changes nothing, and it misfired everywhere else: a
            // trace strung down a column drew as a solid waterfall, and a cell
            // above a pool with spray over it and full water beneath drew as a
            // spike standing out of the surface.
            const float amount = mass.ValueAt(i, j);

            // Clamped by the distance to the rock surface, expressed so that it
            // equals the liquid's own threshold exactly where the rock reaches
            // its threshold.
            //
            // That single identity is what keeps the two aligned: both contours
            // cross their thresholds at the same place, so the liquid meets the
            // ground along the very line the ground is drawn on. Interpolating
            // the two fields independently instead leaves the liquid hanging
            // above the rock or sunk into it by a fraction of a cell, and the
            // gap moves as either field changes.
            const float headroom = threshold + (RockThreshold() - rock.ValueAt(i, j)) * kRockClampGain;

            field.SetValue(i, j, std::min(amount, headroom));
        }
    }

    return field;
}

void World::DrawTerrain(Rectangle view) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    const ElementStyle &style = StyleOf(Element::Rock);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const Chunk *chunk = Find(cx, cy);
            if (chunk == nullptr) continue;

            const Grid &field = chunk->fields[ElementIndex(Element::Rock)];

            marching_squares::DrawFilled(field, style.threshold, style.fill);
            marching_squares::DrawContour(field, style.threshold, style.contour);
        }
    }
}

void World::DrawLiquids(Rectangle view) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    for (std::size_t e = 0; e < kElementCount; e++) {
        const Element element = static_cast<Element>(e);
        if (element == Element::Rock) continue;

        const ElementStyle &style = kElementStyles[e];

        // Drawn fully opaque. Transparency belongs to the layer as a whole, and
        // applying it here would blend each piece against its neighbours again.
        const Color body    = {style.fill.r, style.fill.g, style.fill.b, 255};
        const Color surface = {style.contour.r, style.contour.g, style.contour.b, 255};

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                if (Find(cx, cy) == nullptr) continue;

                const Grid field = LiquidRenderField(cx, cy, element);

                marching_squares::DrawFilled(field, style.threshold, body);
                marching_squares::DrawContour(field, style.threshold, surface);
            }
        }
    }
}

void World::DrawVertexOverlay(Rectangle view, float vertexSize, Color filledColor, Color emptyColor) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;

    ToChunk({view.x, view.y}, minCx, minCy);
    ToChunk({view.x + view.width, view.y + view.height}, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const Chunk *chunk = Find(cx, cy);
            if (chunk == nullptr) continue;

            marching_squares::DrawVertices(chunk->fields[ElementIndex(Element::Rock)], RockThreshold(), vertexSize,
                                           filledColor, emptyColor);
        }
    }
}
