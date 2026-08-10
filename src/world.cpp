#include "world.h"

#include "marching_squares.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Chunks are kept this many chunks beyond the view before being released, so
// that walking back and forth across a border does not regenerate them every
// frame.
constexpr int kKeepMargin = 2;

// Chunks holding something the noise cannot reproduce are pinned instead, but
// not for ever: past this distance they go too. Otherwise crossing a flooded
// world pins every chunk it contains, and memory grows with the distance
// walked rather than with the size of the view.
//
// What comes back is the generated world, so only hand edits and liquid that
// has moved are lost, and only far out of sight. The number bounds the memory
// the world can ever hold: it is the radius, in chunks, of everything kept.
constexpr int kDropMargin = 12;

// How sharply one field is held under another where the two must not overlap.
// Large enough that the clamp only bites within a cell of the boundary, which
// leaves the free surface everywhere else to the material itself.
constexpr float kClampGain = 6.0f;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

} // namespace

World::World(const terrain::Settings &settings, int spacing) : settings_(settings), spacing_(spacing) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].rules.occupies) exclusionOrder_.push_back(static_cast<Element>(e));
    }

    std::sort(exclusionOrder_.begin(), exclusionOrder_.end(),
              [](Element a, Element b) { return Def(a).rules.precedence < Def(b).rules.precedence; });

    CalibrateSpawn();
}

void World::CalibrateSpawn() {
    // Each generated material's cutoff is measured rather than guessed: sample
    // its noise over a wide area, keep the samples that land where the material
    // is allowed, and take the quantile that leaves `coverage` of them above
    // it.
    //
    // Declaring the cutoff directly instead would tie it to the shape of the
    // noise, and every change to frequency or octaves would quietly change how
    // much ore the world holds.
    constexpr int kSamplesPerAxis = 96;
    constexpr float kSampledWidth = 4000.0f;
    constexpr float kSampledDepth = 4000.0f;

    std::vector<float> values;
    values.reserve(kSamplesPerAxis * kSamplesPerAxis);

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementSpawn &spawn = kElements[e].spawn;

        // Nothing clears a cutoff of one, which is the right answer for a
        // material that is not generated from its own noise at all.
        spawnCutoff_[e] = 1.0f;
        if (spawn.generator != Generator::Vein && spawn.generator != Generator::Pool) continue;

        // Sampled across the band rather than across the world, so a narrow
        // band is measured from points that actually fall inside it. Measuring
        // over the whole world instead would count the depths the material
        // never reaches as merely unlucky, and a deep ore would come out far
        // rarer than its coverage asks for.
        const float top    = std::max(spawn.band.top, 0.0f);
        const float bottom = std::min(spawn.band.bottom, top + kSampledDepth);
        const float depth  = (bottom > top) ? (bottom - top) : kSampledDepth;

        values.clear();

        for (int i = 0; i < kSamplesPerAxis; i++) {
            for (int j = 0; j < kSamplesPerAxis; j++) {
                const Vector2 p = {(i / static_cast<float>(kSamplesPerAxis) - 0.5f) * kSampledWidth,
                                   top + (j / static_cast<float>(kSamplesPerAxis)) * depth};

                if (!SpawnEligible(spawn, p)) continue;

                values.push_back(terrain::Sample(p, spawn.shape));
            }
        }

        if (values.empty()) continue;

        const float coverage = std::clamp(spawn.coverage, 0.0f, 1.0f);
        const auto index     = static_cast<std::size_t>((1.0f - coverage) * (values.size() - 1));

        std::nth_element(values.begin(), values.begin() + index, values.end());
        spawnCutoff_[e] = values[index];
    }
}

bool World::SpawnEligible(const ElementSpawn &spawn, Vector2 world) const {
    if (BandDepth(spawn, world) < 0.0f) return false;

    switch (spawn.space) {
    case SpawnSpace::Anywhere: return true;
    case SpawnSpace::InsideGround: return terrain::Density(world, settings_) > RockThreshold();
    case SpawnSpace::OpenSpace: return terrain::Density(world, settings_) <= RockThreshold();
    }

    return true;
}

float World::GeneratedValue(Element element, Vector2 world) const {
    const ElementDef &def     = Def(element);
    const ElementSpawn &spawn = def.spawn;

    switch (spawn.generator) {
    case Generator::None: break;

    case Generator::Terrain: return terrain::Density(world, settings_);

    case Generator::Vein: {
        // Shifted so the element's own threshold is the deciding line: the
        // measured cutoff lands exactly on it, and the field stays continuous
        // either side of it so the contour can still interpolate.
        float value = terrain::Sample(world, spawn.shape) - spawnCutoff_[ElementIndex(element)] + def.threshold;

        // Held under the band the same way, so the field reaches the threshold
        // exactly on the band's edge and a vein tapers out over `fade` instead
        // of being sliced along one height.
        value = std::min(value, def.threshold + BandDepth(spawn, world) / std::max(spawn.band.fade, 1.0f));

        // Bounded against the ground, so a vein ends on the terrain's own
        // contour rather than a fraction of a cell past it, hanging in the air
        // of a cave it happened to cross.
        if (spawn.space == SpawnSpace::InsideGround) {
            const float ground = terrain::Density(world, settings_);
            value              = std::min(value, def.threshold + (ground - RockThreshold()) * kClampGain);
        }

        return value;
    }

    case Generator::Pool:
        // A liquid field holds mass, not the distance to a surface, so a vertex
        // is filled or it is not and the automaton takes it from there. Fading
        // it across the band edge would lay down a sheet of half-filled cells
        // that drains the moment the world starts, which is not what a still
        // water table looks like.
        if (!SpawnEligible(spawn, world)) return 0.0f;

        return (terrain::Sample(world, spawn.shape) > spawnCutoff_[ElementIndex(element)]) ? water::kMaxMass : 0.0f;
    }

    return 0.0f;
}

float World::ExclusionHeadroom(Element element, Vector2 world) const {
    const ElementDef &def = Def(element);
    if (!def.rules.occupies) return kUnboundedDepth;

    float headroom = kUnboundedDepth;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &other = kElements[e];
        if (!other.rules.occupies || other.rules.precedence <= def.rules.precedence) continue;

        const float claim = GeneratedValue(static_cast<Element>(e), world);
        headroom          = std::min(headroom, def.threshold + (other.threshold - claim) * kClampGain);
    }

    return headroom;
}

float World::SpawnValue(Element element, Vector2 world) const {
    return std::min(GeneratedValue(element, world), ExclusionHeadroom(element, world));
}

std::int64_t World::Key(int cx, int cy) {
    return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy);
}

void World::ToChunk(Vector2 world, int &outCx, int &outCy) const {
    const int span = kChunkCells * spacing_;

    outCx = FloorDiv(static_cast<int>(std::floor(world.x)), span);
    outCy = FloorDiv(static_cast<int>(std::floor(world.y)), span);
}

void World::ChunkRange(Rectangle region, int &outMinCx, int &outMinCy, int &outMaxCx, int &outMaxCy) const {
    ToChunk({region.x, region.y}, outMinCx, outMinCy);
    ToChunk({region.x + region.width, region.y + region.height}, outMaxCx, outMaxCy);
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

    // Each material is first laid down exactly as its own generator describes
    // it, with no regard for what else wants the same space.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].spawn.generator == Generator::None) continue;

        Grid &field = chunk.fields[e];

        for (int i = 0; i < field.Cols(); i++) {
            for (int j = 0; j < field.Rows(); j++) {
                field.SetValue(i, j, GeneratedValue(static_cast<Element>(e), field.PointAt(i, j)));
            }
        }
    }

    // Then each is cut back around whatever outranks it, in ascending order of
    // precedence, so that a material is only ever measured against values no
    // earlier pass has already touched. That is what makes this agree exactly
    // with SpawnValue, which computes the same thing one vertex at a time for
    // positions no chunk holds.
    for (const Element element : exclusionOrder_) {
        const ElementDef &def = Def(element);
        Grid &field           = chunk.fields[ElementIndex(element)];

        for (std::size_t o = 0; o < kElementCount; o++) {
            const ElementDef &other = kElements[o];
            if (!other.rules.occupies || other.rules.precedence <= def.rules.precedence) continue;

            const Grid &claim = chunk.fields[o];

            for (int i = 0; i < field.Cols(); i++) {
                for (int j = 0; j < field.Rows(); j++) {
                    const float headroom = def.threshold + (other.threshold - claim.ValueAt(i, j)) * kClampGain;
                    field.SetValue(i, j, std::min(field.ValueAt(i, j), headroom));
                }
            }
        }
    }

    return chunks_.emplace(Key(cx, cy), std::move(chunk)).first->second;
}

void World::Update(Rectangle view) {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            if (Find(cx, cy) == nullptr) Emplace(cx, cy);
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        int cx = 0;
        int cy = 0;
        ToChunk(it->second.fields[0].Origin(), cx, cy);

        // Distance to the region in chunks, zero for one inside it.
        const int distance = std::max({minCx - cx, cx - maxCx, minCy - cy, cy - maxCy, 0});

        const bool pinned = it->second.edited || it->second.holdsLiquid;
        const int margin  = pinned ? kDropMargin : kKeepMargin;

        it = (distance > margin) ? chunks_.erase(it) : std::next(it);
    }
}

void World::Reset() {
    chunks_.clear();
}

int World::PinnedChunks() const {
    int pinned = 0;

    for (const auto &[key, chunk] : chunks_) {
        if (chunk.edited || chunk.holdsLiquid) pinned++;
    }

    return pinned;
}

std::vector<World::ChunkView> World::ChunksIn(Rectangle view) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    const float span = ChunkSpan();

    std::vector<ChunkView> resident;

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const Chunk *chunk = Find(cx, cy);
            if (chunk == nullptr) continue;

            resident.push_back({cx, cy, {cx * span, cy * span, span, span}, chunk->edited, chunk->holdsLiquid});
        }
    }

    return resident;
}

float World::ValueAt(Element element, Vector2 world) const {
    const Vector2 vertex = SnapToLattice(world);

    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    const Chunk *chunk = Find(cx, cy);

    // A generated material still answers beyond the resident area, from the
    // same noise it would have been built with. One that is only ever placed by
    // hand is state, not a function of position, so out there it has none.
    if (chunk == nullptr) return SpawnValue(element, vertex);

    const Grid &field = chunk->fields[ElementIndex(element)];

    int i = 0;
    int j = 0;
    field.ToLocal(vertex, i, j);

    if (!field.InBounds(i, j)) return SpawnValue(element, vertex);

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

            if (Def(element).rules.flows && value > water::kDryMass) chunk.holdsLiquid = true;
        }
    }
}

void World::MarkEdited(Vector2 vertex) {
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

bool World::IsSolidAt(Vector2 world) const {
    // The union of every element that blocks bodies, so two solids never have
    // to know about each other and a new one starts colliding the moment its
    // row says so.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (!kElements[e].rules.blocksBodies) continue;
        if (ValueAt(static_cast<Element>(e), world) > kElements[e].threshold) return true;
    }

    return false;
}

std::optional<Element> World::OccupantAt(Vector2 world) const {
    std::optional<Element> occupant;
    int rank = 0;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];

        if (!def.rules.occupies) continue;
        if (ValueAt(static_cast<Element>(e), world) <= def.threshold) continue;

        // Exclusion leaves exactly one claimant, so this loop normally finds
        // one material and stops mattering. Taking the highest rank anyway
        // keeps the answer defined if a hand edit ever leaves two.
        if (!occupant.has_value() || def.rules.precedence > rank) {
            occupant = static_cast<Element>(e);
            rank     = def.rules.precedence;
        }
    }

    return occupant;
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
            // Weighted by each material's buoyancy, so a body floats higher
            // in one liquid than another without this code naming either.
            for (std::size_t e = 0; e < kElementCount; e++) {
                if (kElements[e].rules.buoyancy <= 0.0f) continue;

                const float share =
                    std::min(ValueAt(static_cast<Element>(e), {x, y}), 1.0f) * kElements[e].rules.buoyancy;

                wettest = std::max(wettest, share);
            }
        }

        total += wettest;
        rows++;
    }

    return (rows > 0) ? static_cast<float>(total / rows) : 0.0f;
}

void World::ClearVertex(Vector2 vertex, Yield &yield) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (!def.rules.occupies && !def.rules.flows) continue;

        const Element element = static_cast<Element>(e);
        const float value     = ValueAt(element, vertex);

        // Already empty. Skipping it matters beyond the wasted write: marking
        // the chunk edited is what pins it in memory, and a brush swept through
        // open sky would otherwise pin everything it passed over.
        if (value <= 0.0f) continue;

        if (value > def.threshold) yield[e]++;

        WriteVertex(element, vertex, 0.0f);
        if (!def.rules.flows) MarkEdited(vertex);
    }
}

void World::ApplyBrush(Vector2 world, float radius, std::optional<Element> place, Yield &yield) {
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

            // A liquid is poured into a space, not pressed into one. It does
            // not clear what it lands on; it simply does not land there.
            if (place.has_value() && Def(*place).rules.flows) {
                if (!OccupantAt(vertex).has_value()) WriteVertex(*place, vertex, water::kMaxMass);
                continue;
            }

            // Digging and placing a solid start the same way. Two materials
            // never share a vertex, so placing is a replacement, and digging is
            // this edit without its second half.
            ClearVertex(vertex, yield);

            if (!place.has_value()) continue;

            // Painted samples are pinned to the top of the range rather than
            // nudged, so a brush stroke reads as a definite edit and not as a
            // faint gradient.
            WriteVertex(*place, vertex, 1.0f);
            MarkEdited(vertex);
        }
    }
}

void World::Place(Element element, Vector2 world, float radius) {
    Yield discarded{};
    ApplyBrush(world, radius, element, discarded);
}

World::Yield World::Excavate(Vector2 world, float radius) {
    Yield yield{};
    ApplyBrush(world, radius, std::nullopt, yield);

    return yield;
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

    // Every chunk overlapping the region is assumed dry until the write-back
    // says otherwise, so a chunk that drains can be released again instead of
    // staying pinned for the rest of the session.
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(active, minCx, minCy, maxCx, maxCy);

    // Widened by one chunk because a write to a vertex on the region's border
    // also lands in the chunk before it. Without the margin such a chunk would
    // keep a stale flag and stay pinned in memory after draining.
    for (int cx = minCx - 1; cx <= maxCx + 1; cx++) {
        for (int cy = minCy - 1; cy <= maxCy + 1; cy++) {
            const auto it = chunks_.find(Key(cx, cy));
            if (it != chunks_.end()) it->second.holdsLiquid = false;
        }
    }

    // One pass per flowing material. They share the scratch buffer and are
    // simulated independently: each sees the others only as obstacles, through
    // the blocksLiquid rule.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (!kElements[e].rules.flows) continue;

        const Element element = static_cast<Element>(e);

        for (int i = 0; i < cols; i++) {
            for (int j = 0; j < rows; j++) {
                const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
                const int cell       = scratch_.Index(i, j);

                scratch_.mass[cell]    = ValueAt(element, vertex);
                scratch_.blocked[cell] = BlocksLiquidAt(vertex) ? 1 : 0;
            }
        }

        water::Step(scratch_, waterSettings_);

        for (int i = 0; i < cols; i++) {
            for (int j = 0; j < rows; j++) {
                const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
                WriteVertex(element, vertex, scratch_.mass[scratch_.Index(i, j)]);
            }
        }
    }
}

bool World::BlocksLiquidAt(Vector2 world) const {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (!kElements[e].rules.blocksLiquid) continue;
        if (ValueAt(static_cast<Element>(e), world) > kElements[e].threshold) return true;
    }

    return false;
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
            for (std::size_t e = 0; e < kElementCount; e++) {
                if (kElements[e].rules.flows) total += ValueAt(static_cast<Element>(e), {i * step, j * step});
            }
        }
    }

    return static_cast<float>(total);
}

Grid World::OccupancyField(const Chunk &chunk, int minPrecedence) const {
    const Grid &any = chunk.fields[0];

    Grid margin(any.Origin(), any.Cols(), any.Rows(), spacing_);

    for (int i = 0; i < margin.Cols(); i++) {
        for (int j = 0; j < margin.Rows(); j++) {
            // Below anything a material could reach, so a chunk with none of
            // the asked-for materials reads as open space rather than as filled.
            float filled = -kUnboundedDepth;

            for (std::size_t e = 0; e < kElementCount; e++) {
                if (!kElements[e].rules.occupies || kElements[e].rules.precedence < minPrecedence) continue;

                filled = std::max(filled, chunk.fields[e].ValueAt(i, j) - kElements[e].threshold);
            }

            margin.SetValue(i, j, filled);
        }
    }

    return margin;
}

Grid World::LiquidRenderField(int cx, int cy, Element element) const {
    const Chunk *chunk = Find(cx, cy);
    const float span   = ChunkSpan();

    if (chunk == nullptr) return Grid({cx * span, cy * span}, 1, 1, spacing_);

    const Grid &mass   = chunk->fields[ElementIndex(element)];
    const Grid &filled = OccupancyField(*chunk);

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

            // Clamped by how deeply the solids fill the vertex, expressed so
            // that it equals the liquid's own threshold exactly where they
            // reach theirs.
            //
            // That single identity is what keeps the two aligned: both contours
            // cross their thresholds at the same place, so the liquid meets the
            // ground along the very line the ground is drawn on. Interpolating
            // the two fields independently instead leaves the liquid hanging
            // above the rock or sunk into it by a fraction of a cell, and the
            // gap moves as either field changes.
            const float headroom = threshold - filled.ValueAt(i, j) * kClampGain;

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
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    // Painted from the back, each material drawn as itself together with
    // everything that outranks it, so that a boundary between two materials is
    // drawn once, by the one that owns it.
    //
    // Drawing each material from its own field instead leaves the one
    // underneath outlining the hole punched in it, and a rim of rock contour
    // appears around every ore pocket. Worse, the two contours are two
    // different fields interpolated on the same lattice, so they part company
    // by a fraction of a cell and the rim is not even of constant width.
    //
    // Only the drawing overlaps here. The fields stay exclusive, and what is
    // drawn underneath is covered by the material that displaced it.
    for (const Element element : exclusionOrder_) {
        const ElementDef &def = Def(element);

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                const Chunk *chunk = Find(cx, cy);
                if (chunk == nullptr) continue;

                const Grid field = OccupancyField(*chunk, def.rules.precedence);

                marching_squares::DrawFilled(field, 0.0f, def.fill);
                marching_squares::DrawContour(field, 0.0f, def.contour);
            }
        }
    }

    // Anything that neither flows nor claims its space has nothing to be drawn
    // behind or in front of, so it is drawn plainly from its own field.
    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (def.rules.flows || def.rules.occupies) continue;

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                const Chunk *chunk = Find(cx, cy);
                if (chunk == nullptr) continue;

                marching_squares::DrawFilled(chunk->fields[e], def.threshold, def.fill);
                marching_squares::DrawContour(chunk->fields[e], def.threshold, def.contour);
            }
        }
    }
}

void World::DrawLiquids(Rectangle view) const {
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    for (std::size_t e = 0; e < kElementCount; e++) {
        const Element element = static_cast<Element>(e);
        if (!kElements[e].rules.flows) continue;

        const ElementDef &style = kElements[e];

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
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const Chunk *chunk = Find(cx, cy);
            if (chunk == nullptr) continue;

            // Occupancy rather than any one material, so the overlay answers
            // the question the world now answers: whether the space is taken,
            // by whatever happens to have taken it.
            marching_squares::DrawVertices(OccupancyField(*chunk), 0.0f, vertexSize, filledColor, emptyColor);
        }
    }
}
