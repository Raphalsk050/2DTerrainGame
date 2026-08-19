#include "world/world.h"

#include "core/config.h"
#include "world/marching_squares.h"
#include "core/pool.h"
#include "core/profile.h"
#include "rlgl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
// Nothing built is lost by this any more. A hand edit is remembered as the
// vertex it changed and replayed when the chunk is rebuilt, so dropping an
// edited chunk out here costs the work of generating it again and nothing else
// — the pin is only there to spare a player standing near what they have built
// from paying that over and over.
//
// Liquid that has moved is still lost, and deliberately: it is simulation
// rather than intent, and where a pool has crept to is not something worth
// remembering about a place nobody can see. What comes back is the pool the
// generator describes.
constexpr int kDropMargin = 12;

// How sharply one field is held under another where the two must not overlap.
// Large enough that the clamp only bites within a cell of the boundary, which
// leaves the free surface everywhere else to the material itself.
constexpr float kClampGain = 6.0f;

// Added to the world's seed for the grass's own texture, so its grain is not the
// soil's grain read at the same place.
constexpr int kSodSeed = 4231;

// Lattice rows above a chunk that the sod's column walk primes itself from, and
// the depth it assumes where those rows are solid all the way up.
//
// Three, because what the walk measures stops mattering past sod::kSodDepth and
// that is under two rows of the lattice; the third is the one that says whether
// the second was itself buried.
constexpr int kSodSeedRows  = 3;
constexpr float kSodBuried = 1.0e6f;

// How far down a column is followed looking for the ground before it is called
// a shaft. Past this the sky can no longer reach anyway, so the answer would
// change nothing.
constexpr float kSkylineScan = 1400.0f;

// Depth below the terrain's surface level over which daylight dims to nothing.
//
// A property of the light rather than of the ground: the generator now describes
// a surface at a definite height, and how far past it the sky still reaches is a
// separate question from where the rock is.
constexpr float kSkyFade = 96.0f;

// How far above the generator's own surface the skyline scan starts.
//
// The scan has to begin in open air, and the generator answers where the surface
// is directly, so this only has to cover the one thing that answer omits: the
// fold, which can lift the real surface a little above the height the column
// reports.
constexpr float kSkylineHeadroom = 64.0f;

int FloorDiv(int value, int divisor) {
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

// A colour out of the element table as linear light.
//
// Channels are taken as they are written rather than lifted out of the screen's
// own curve first. The table is authored by eye against what the world looks
// like, so agreeing with it is worth more here than being right about gamma.
light::Radiance Glow(Color color, float strength) {
    constexpr float kByte = 1.0f / 255.0f;

    return {color.r * kByte * strength, color.g * kByte * strength, color.b * kByte * strength};
}

// The colour a material outlines itself with, or nothing where outlines are
// switched off. Transparent is what DrawPixelated already reads as "leave the
// fill unbroken", so there is no second path to keep working.
Color Outline(Color contour) {
    return config::kDrawContours ? contour : BLANK;
}

// The grass, painted with the same equation the materials are and over a ramp
// that changes with the column.
//
// The shading is soil's own and not a second copy of it: what makes the top of a
// sod read as lit and its underside as shaded is exactly what makes the top of a
// ledge read that way, and the sod's own field supplies the face. All this adds
// is which greens the ramp is made of, which is the one thing that is not a
// property of the shape.
struct SodPainter {
    const sod::Look *looks = nullptr;
    int count               = 0;
    int firstColumn         = 0;
    float spacing           = 1.0f;
    int seed                = 0;

    Color operator()(const marching_squares::Texel &texel) const {
        const int column = static_cast<int>(std::floor(texel.at.x / spacing)) - firstColumn;

        const soil::Paint paint{.ramp   = looks[std::clamp(column, 0, count - 1)].ramp,
                                .grain  = sod::kGrassGrain,
                                .patch  = sod::kGrassPatch,
                                .strata = sod::kGrassStrata,
                                .bedded = true,
                                .seed   = seed};

        return paint(texel);
    }
};


} // namespace

World::World(const terrain::Settings &settings, int spacing) : settings_(settings), spacing_(spacing) {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].rules.occupies) exclusionOrder_.push_back(static_cast<Element>(e));
    }

    std::sort(exclusionOrder_.begin(), exclusionOrder_.end(),
              [](Element a, Element b) { return Def(a).rules.precedence < Def(b).rules.precedence; });

    // A stride between the materials' texture seeds, so that the grain on the
    // soil is not the grain on the rock beneath it read at the same place. Added
    // to the world's own seed for the reason every other field here is: one
    // number moves the whole world and the fields stay decorrelated.
    constexpr int kPaintStride = 977;

    for (std::size_t e = 0; e < kElementCount; e++) {
        paint_[e] = soil::For(kElements[e], settings_.seed + static_cast<int>(e) * kPaintStride);

        // And the same material standing behind the ground, which is that painter
        // with the light taken out of it — see behind_. The ramp is scaled rather
        // than the four authored tones, which comes to the same thing because Build
        // interpolates them linearly, and saves authoring a second set of tones for
        // every material in the table.
        behind_[e] = paint_[e];

        for (Color &tone : behind_[e].ramp.tone) {
            tone.r = static_cast<unsigned char>(tone.r * kBehindShare);
            tone.g = static_cast<unsigned char>(tone.g * kBehindShare);
            tone.b = static_cast<unsigned char>(tone.b * kBehindShare);
        }
    }


    // The generator's own cutoffs first, since everything below asks it where
    // the ground is and an uncalibrated one answers with no caves at all.
    terrain::Calibrate(settings_);

    CalibrateSpawn();

    // Default weather, so the sky is never standing over a world it was not told
    // about. SetWeather replaces the settings and keeps the same terrain.
    sky_.Configure(weather::Settings{}, settings_);

    // And the ranges behind it, on the same terms and for the same reason.
    vista_.Configure(vista::Settings{}, settings_);
}

namespace {

// The measurement's own numbers. Lifted out of the loop that used to hold them so
// that opening a material, sampling it and closing it can be three calls made on
// three different frames — see World::BeginRebuild.
constexpr int kAboveCutoff = 60;
constexpr int kMinPerAxis  = 48;
constexpr int kMaxPerAxis  = 384;

// Multiples of one feature the grid steps by, one per axis.
//
// Irrational, and this is not a detail. Perlin noise is built on a lattice and is
// exactly zero at every corner of it, so a grid stepping a whole number of
// features reads the same corner over and over: every sample comes back at the
// midpoint of the field, the cutoff lands in the middle of the distribution
// instead of its tail, and the world fills with ore. Stepping by an irrational
// multiple lands each sample at a different phase, and using a different one per
// axis stops the two from lining up with each other either.
constexpr float kStrideX = 1.618f;
constexpr float kStrideY = 1.303f;

// Depth the grid starts at, far enough down to be in rock rather than straddling
// the surface, where half the samples would be sky.
constexpr float kSampledTop = 600.0f;

// How many rows of one material's grid a slice takes before it looks at the clock.
//
// Sixteen, which at a full grid is a row per worker with a few to spare and about
// three milliseconds of work — fine enough that a slice overruns its budget by
// less than a frame, and coarse enough that the workers are not being woken for a
// few microseconds at a time.
constexpr int kRowBlock = 16;

} // namespace

void World::OpenMaterial() {
    measuring_.perAxis = 0;
    measuring_.row     = 0;

    // Walks straight past anything that is not measured, setting its cutoff to one
    // on the way: nothing clears that, which is the right answer for a material
    // that is not generated from its own noise at all — the covers, which are
    // measured from the surface, and the liquids, which stand at a level the
    // aquifer settings give them.
    while (measuring_.material < kElementCount) {
        const ElementSpawn &spawn = kElements[measuring_.material].spawn;

        spawnCutoff_[measuring_.material] = 1.0f;

        if (spawn.generator != Generator::Vein) {
            measuring_.material++;
            continue;
        }

        const float probability = std::clamp(spawn.probability, 0.0f, 1.0f);

        if (probability <= 0.0f) {
            measuring_.material++;
            continue;
        }

        measuring_.probability = probability;

        // Enough samples that `kAboveCutoff` of them land above the cutoff, before
        // the eligibility test throws any away.
        measuring_.perAxis = std::clamp(static_cast<int>(std::ceil(std::sqrt(kAboveCutoff / probability))),
                                        kMinPerAxis, kMaxPerAxis);

        // More than one feature apart, so no two samples read the same hump.
        measuring_.feature = terrain::kFeatureSpan / std::max(SpawnNoise(spawn).frequency, 0.01f);

        measuring_.values.clear();
        measuring_.values.reserve(static_cast<std::size_t>(measuring_.perAxis) * measuring_.perAxis);

        return;
    }
}

void World::CloseMaterial() {
    if (!measuring_.values.empty()) {
        const auto index =
            static_cast<std::size_t>((1.0f - measuring_.probability) * (measuring_.values.size() - 1));

        std::nth_element(measuring_.values.begin(), measuring_.values.begin() + index, measuring_.values.end());

        spawnCutoff_[measuring_.material] = measuring_.values[index];
    }

    measuring_.material++;
    measuring_.perAxis = 0;
    measuring_.row     = 0;
    measuring_.done++;
}

bool World::StepCalibration(float budget) {
    const double until = GetTime() + budget;

    while (measuring_.material < kElementCount) {
        if (measuring_.perAxis == 0) OpenMaterial();
        if (measuring_.material >= kElementCount) break;

        const ElementSpawn &spawn = kElements[measuring_.material].spawn;

        const int perAxis   = measuring_.perAxis;
        const float feature = measuring_.feature;

        while (measuring_.row < perAxis) {
            const int first = measuring_.row;
            const int block = std::min(kRowBlock, perAxis - first);

            measuring_.rows.resize(static_cast<std::size_t>(kRowBlock));

            // A row per worker, each writing into a list of its own. The samples are
            // gathered on this side afterwards, so nothing is shared while the rows
            // are being taken — and this is where the seconds went: the grid is up to
            // three hundred and eighty-four squared samples of the surface, per ore,
            // and it was walked one sample at a time on one core.
            pool::For(
                block,
                [&](int k) {
                    std::vector<float> &into = measuring_.rows[static_cast<std::size_t>(k)];
                    into.clear();

                    const int i = first + k;

                    for (int j = 0; j < perAxis; j++) {
                        const Vector2 p = {(i - perAxis / 2) * feature * kStrideX,
                                           kSampledTop + j * feature * kStrideY};

                        const terrain::Ground rock = terrain::SampleGround(p, settings_);
                        if (!SpawnEligible(spawn, p, rock.density)) continue;

                        // The wall lift is part of what the cutoff will be compared
                        // against, so it has to be part of what the cutoff is measured
                        // from. Measured without it, biasing a vein towards the cave
                        // walls would not move the ore there — it would simply add
                        // ore, and the probability in the table would quietly stop
                        // being the share it says it is.
                        into.push_back(terrain::Sample(p, SpawnNoise(spawn)) + WallLift(spawn, rock.solid));
                    }
                },
                1, 1);

            for (int k = 0; k < block; k++) {
                const std::vector<float> &row = measuring_.rows[static_cast<std::size_t>(k)];

                measuring_.values.insert(measuring_.values.end(), row.begin(), row.end());
            }

            measuring_.row += block;

            // The clock is read between blocks and never inside one, so a slice
            // overruns by at most a block. Gathering the quantile is left for the
            // next slice rather than being squeezed in here: it is a partial sort of
            // a hundred thousand floats, and it belongs to the material rather than
            // to whichever slice happened to finish its last row.
            if (GetTime() >= until) return false;
        }

        CloseMaterial();
    }

    measuring_.running = false;

    return true;
}

void World::BeginRebuild(const terrain::Settings &settings) {
    settings_ = settings;

    // The same measurement the constructor takes and in the same order: everything
    // below asks the generator where the ground is, and an uncalibrated one answers
    // with a world that is not this one.
    terrain::Calibrate(settings_);

    // The sky is a function of the terrain as well as of the weather — it reads the
    // skyline to know where the ground is — so it is told about the new country. Its
    // own settings are kept: which weather blows is not a property of which world
    // this is. Copied first, because Configure writes over the member it would
    // otherwise be reading from.
    const weather::Settings weather = sky_.Config();

    sky_.Configure(weather, settings_);

    // The horizon with it. Its palette is read out of the climate at a column, so a
    // range left configured against the old country is the wrong colour in the new
    // one — and it is the sort of wrong that looks like a bad seed rather than like
    // a bug. See CLAUDE.md §14.1.
    const vista::Settings ranges = vista_.Config();

    vista_.Configure(ranges, settings_);

    Reset();

    measuring_         = {};
    measuring_.running = true;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementSpawn &spawn = kElements[e].spawn;

        if (spawn.generator == Generator::Vein && spawn.probability > 0.0f) measuring_.total++;
    }
}

World::Making World::StepRebuild(float budget) {
    Making making;

    if (!measuring_.running) return making;

    making.done = StepCalibration(budget);

    // Read after the slice and not before it. Before, the walk has not yet stepped
    // past the materials that are *not* measured — the rock, the covers, the water
    // — so the first line of the first world named the rock, which is the one
    // material in the table this measurement has nothing to do with. After, it is
    // whichever seam the next slice will spend itself on.
    const char *name = (measuring_.material < kElementCount) ? StyleOf(static_cast<Element>(measuring_.material)).name
                                                             : "the last of the seams";

    if (making.done) {
        making.share = 1.0f;
        std::snprintf(making.what, sizeof(making.what), "%s", "the ground is measured");

        return making;
    }

    // Materials finished, plus how far into the open one this is. A bar that only
    // moved when a material finished would sit still for a second at a time, which
    // is the thing a bar exists to stop.
    const float within = (measuring_.perAxis > 0) ? static_cast<float>(measuring_.row) / measuring_.perAxis : 0.0f;
    const float total  = static_cast<float>(std::max(measuring_.total, 1));

    making.share = std::clamp((measuring_.done + within) / total, 0.0f, 1.0f);

    std::snprintf(making.what, sizeof(making.what), "measuring how much %s this world holds", name);

    return making;
}

void World::CalibrateSpawn() {
    // Each generated material's cutoff is measured rather than guessed: sample its
    // own noise where the material is allowed to be, and take the quantile that
    // leaves `probability` of the samples above it.
    //
    // Declaring the cutoff directly instead would tie it to the shape of the
    // noise, and every change to frequency or octaves would quietly change how
    // much ore the world holds.
    //
    // Two things decide whether the measurement means anything, and getting either
    // wrong is silent — the ore simply is not there.
    //
    // The samples have to be at least one noise feature apart. Two samples inside
    // one hump of the field are one sample, and a quantile taken over correlated
    // samples is really the maximum of a much smaller set. For a rare ore that
    // maximum is the top of its own field, so the cutoff comes out at a value
    // nothing can clear.
    //
    // And the tail has to be populated, since the quantile for a probability of one
    // in a thousand rests entirely on the samples above it. The grid is therefore
    // sized from that probability: a rarer material is measured over a
    // proportionally larger stretch of world.
    //
    // Where that stretch sits does not matter. A material's noise is decorrelated
    // from the terrain's, so its distribution is the same at any depth, and
    // `probability` is the share it reaches where nothing is held against it — its
    // peak. The band's thinning is applied on top of this cutoff by BandPenalty
    // rather than folded into it.
    //
    // The ceiling is what bounds the cost of all this, and it is reached only by
    // the rarest ores: a fifth of a second at startup, once, against an ore that
    // silently does not generate.
    measuring_         = {};
    measuring_.running = true;

    // Run to the end. There is no frame to keep drawing yet — the window has only
    // just opened — so the budget is anything at all, and the work is the same work
    // BeginRebuild spreads over frames when there is a screen waiting on it.
    while (!StepCalibration(1e9f)) {
    }
}

bool World::SpawnEligible(const ElementSpawn &spawn, Vector2 world, float ground) const {
    // The space test alone. Whether a material's own level reaches a given depth
    // is now a question of how much of it there is rather than of whether it is
    // allowed there at all, and that is answered by BandPenalty. The exception is
    // a liquid, whose band is a real boundary, and which tests it for itself.
    switch (spawn.space) {
    case SpawnSpace::Anywhere: return true;
    case SpawnSpace::InsideGround: return ground > RockThreshold();
    case SpawnSpace::OpenSpace: return ground <= RockThreshold();
    }

    return true;
}

float World::GeneratedValue(Element element, Vector2 world, const terrain::Ground &ground,
                            const terrain::Climate &climate) const {
    const ElementDef &def     = Def(element);
    const ElementSpawn &spawn = def.spawn;

    switch (spawn.generator) {
    case Generator::None: break;

    case Generator::Terrain: return ground.density;

    case Generator::Vein: {
        // Shifted so the element's own threshold is the deciding line: the
        // measured cutoff lands exactly on it, and the field stays continuous
        // either side of it so the contour can still interpolate.
        //
        // The band raises that line away from the material's own level rather
        // than cutting the field off at it. An ore therefore thins out with
        // distance from its peak and survives outside its band only where its
        // noise happened to run high, which arrives as the occasional small
        // pocket a long way from home — the thing a hard edge made impossible.
        // Lifted towards the wall of a cave, which is what makes exploring one
        // pay better than digging through the rock beside it. See WallLift.
        float value = terrain::Sample(world, SpawnNoise(spawn)) + WallLift(spawn, ground.solid)
                    - spawnCutoff_[ElementIndex(element)] - BandPenalty(spawn, world, ground.depth) + def.threshold;

        // Bounded against the ground, so a vein ends on the terrain's own
        // contour rather than a fraction of a cell past it, hanging in the air
        // of a cave it happened to cross.
        if (spawn.space == SpawnSpace::InsideGround) {
            value = std::min(value, def.threshold + (ground.density - RockThreshold()) * kClampGain);
        }

        return value;
    }

    case Generator::Cover: {
        // The land's own surface here, which a cover with a snow line is measured
        // against. Free rather than a second evaluation of the surface: the depth
        // is signed and measured from it, so the height is the position minus it.
        const float surface = world.y - ground.depth;

        const float thickness = CoverThickness(spawn, world.x, surface, climate.temperature, climate.humidity);
        if (thickness <= 0.0f) return 0.0f;

        // How far inside the slab this vertex is, in pixels: the smaller of its
        // distance below the surface and its distance above the material's own
        // floor. A distance and not a yes-or-no, so the contour has a gradient to
        // interpolate through and the cover's edge lands on the same line the
        // rock's does rather than a fraction of a cell away from it.
        const float inside = std::min(ground.depth, thickness - ground.depth);

        float value = std::clamp(def.threshold + inside / terrain::kDensitySpan, 0.0f, 1.0f);

        // Bounded against the ground for the reason a vein is, and it matters more
        // here: the depth a cover is measured against knows nothing about caves,
        // so without this a shaft breaking the surface would be plugged with soil
        // and every cave mouth would wear a brown lid.
        if (spawn.space == SpawnSpace::InsideGround) {
            value = std::min(value, def.threshold + (ground.density - RockThreshold()) * kClampGain);
        }

        return value;
    }

    case Generator::Pool: {
        // Groundwater, and so a level rather than a scattering: what is wet is the
        // open space below the water table over this stretch of world, and nothing
        // above it. See terrain::AquiferSettings for why it has to be built that
        // way — a liquid placed as anything other than its own resting state is a
        // shape the automaton immediately pulls down, and it does it again every
        // time the chunk is rebuilt.
        //
        // The element's own spawn noise is not consulted at all. A field of mass
        // scattered through a cave is not what water does; it is what a water
        // table looked like before there was one.
        if (!SpawnEligible(spawn, world, ground.density)) return 0.0f;

        const terrain::WaterTable table = terrain::TableAt(world.x, settings_);

        // Cells of water standing over this vertex.
        const float under = (world.y - table.level) / static_cast<float>(spacing_);
        if (under <= 0.0f) return 0.0f;

        // The one vertex the surface passes through carries the share of its cell
        // that lies under the waterline. That fraction is what the contour
        // interpolates through, so the water is drawn level rather than stepping
        // down the lattice a cell at a time.
        //
        // And below it the column is *compressed*, which is the part that cannot
        // be left out. A settled column in this automaton is not uniform: two
        // stacked cells come to rest with the lower holding kMaxCompress more
        // than the upper, so equilibrium is a gradient and not a fill. Laying the
        // water down at full mass all the way down instead looks right and is
        // not: it is a column with too little at the bottom and too much at the
        // top, so the whole surface sinks the moment it is simulated. Measured, a
        // third of the water in a deep region moved. Written this way, none of it
        // does — which is the entire point of describing the water as a level,
        // and it is undone by getting the shape of the level wrong.
        return std::min(under, 1.0f) * water::kMaxMass + std::max(under - 1.0f, 0.0f) * water::kMaxCompress;
    }
    }

    return 0.0f;
}

float World::ExclusionHeadroom(Element element, Vector2 world, const terrain::Ground &ground,
                               const terrain::Climate &climate) const {
    const ElementDef &def = Def(element);
    if (!def.rules.occupies) return kUnboundedDepth;

    float headroom = kUnboundedDepth;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &other = kElements[e];
        if (!other.rules.occupies || other.rules.precedence <= def.rules.precedence) continue;

        const float claim = GeneratedValue(static_cast<Element>(e), world, ground, climate);
        headroom          = std::min(headroom, def.threshold + (other.threshold - claim) * kClampGain);
    }

    return headroom;
}

float World::SpawnValue(Element element, Vector2 world) const {
    // Both sampled once here and handed to everything below. The ground is much
    // the most expensive field in the generator and every material bounded
    // against it wants the same answer at the same position; the climate costs
    // the surface a second time on top of its own two fields, and every cover
    // asks for it.
    const terrain::Ground ground   = terrain::SampleGround(world, settings_);
    const terrain::Climate climate = terrain::ClimateAt(world.x, settings_);

    return std::min(GeneratedValue(element, world, ground, climate),
                    ExclusionHeadroom(element, world, ground, climate));
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
    // Held per thread, because the passes that walk the lattice are split across
    // the machine's cores and one shared answer between them would be a race.
    //
    // A miss is never remembered: a chunk that is not resident now may be
    // emplaced a moment later, and a remembered absence would have to be
    // invalidated by every insert. See World::chunkAge_ for what invalidates a
    // remembered hit.
    struct Recent {
        const World *world = nullptr;
        long long age      = -1;
        int cx             = 0;
        int cy             = 0;
        const Chunk *chunk = nullptr;
    };

    static thread_local Recent recent;

    if (recent.chunk != nullptr && recent.world == this && recent.age == chunkAge_ && recent.cx == cx
        && recent.cy == cy) {
        return recent.chunk;
    }

    const auto it = chunks_.find(Key(cx, cy));
    if (it == chunks_.end()) return nullptr;

    recent = {.world = this, .age = chunkAge_, .cx = cx, .cy = cy, .chunk = &it->second};

    return recent.chunk;
}

World::Chunk World::Build(int cx, int cy) const {
    const Vector2 origin = {cx * ChunkSpan(), cy * ChunkSpan()};

    Chunk chunk;
    chunk.fields.reserve(kElementCount);
    for (std::size_t e = 0; e < kElementCount; e++) {
        chunk.fields.emplace_back(origin, kChunkVertices, kChunkVertices, spacing_);
    }

    // The base terrain first, and once per vertex. It is by far the most
    // expensive field the generator produces, and every material bounded against
    // the ground asks it the same question at the same position, so sampling it
    // per material per vertex cost five times what it had to.
    std::vector<terrain::Ground> ground(static_cast<std::size_t>(kChunkVertices) * kChunkVertices);

    // The climate once per *column*, which is the whole of what it depends on.
    //
    // It is not a cheap sample: two fields of its own, plus the surface again for
    // the elevation the lapse rate is read against — and the surface is the eight
    // octaves the paragraph above is about. Per vertex it would have cost this
    // chunk thirty-three times what it does, for an answer that is the same all
    // the way down the column by construction.
    std::array<terrain::Climate, kChunkVertices> climate{};

    for (int i = 0; i < kChunkVertices; i++) {
        const float x = origin.x + static_cast<float>(i * spacing_);

        climate[static_cast<std::size_t>(i)] = terrain::ClimateAt(x, settings_);

        for (int j = 0; j < kChunkVertices; j++) {
            const Vector2 vertex = {x, origin.y + static_cast<float>(j * spacing_)};

            ground[static_cast<std::size_t>(i) * kChunkVertices + j] = terrain::SampleGround(vertex, settings_);
        }
    }

    // Then each material is laid down exactly as its own generator describes it,
    // with no regard for what else wants the same space.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].spawn.generator == Generator::None) continue;

        Grid &field = chunk.fields[e];

        for (int i = 0; i < field.Cols(); i++) {
            const terrain::Climate &here = climate[static_cast<std::size_t>(i)];

            for (int j = 0; j < field.Rows(); j++) {
                const terrain::Ground &under = ground[static_cast<std::size_t>(i) * kChunkVertices + j];

                field.SetValue(i, j, GeneratedValue(static_cast<Element>(e), field.PointAt(i, j), under, here));
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

    // And last, whatever was done to it by hand, which no amount of noise can
    // produce. Last because a brush writes over the finished field rather than
    // into the contest that produced it, and that is exactly what a live edit
    // does — so a chunk rebuilt from these is the chunk that was thrown away.
    ApplyEdits(chunk, cx, cy);

    return chunk;
}

World::Chunk &World::Emplace(int cx, int cy) {
    Chunk chunk = Build(cx, cy);

    return Settle(cx, cy, std::move(chunk));
}

World::Chunk &World::Settle(int cx, int cy, Chunk &&chunk) {
    // A column answered before its chunk was resident was answered from the noise
    // alone, and a built chunk replays whatever was edited into it, so the band's
    // memory of these columns is dropped as they come into play.
    const float left = static_cast<float>(cx) * ChunkSpan();

    ForgetSod(left - static_cast<float>(spacing_), left + ChunkSpan() + static_cast<float>(spacing_));

    // A chunk that has just been built has never been drawn, and one being
    // rebuilt is not the one whose picture was taken.
    DropPainted(cx, cy);

    return chunks_.emplace(Key(cx, cy), std::move(chunk)).first->second;
}

void World::Remember(Vector2 vertex, std::optional<Element> element, bool behind) {
    // Which layer this is about, kept beside what was left there — see Edit::front.
    // The flag and not the optional is what says the layer was touched at all.
    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    const float step = static_cast<float>(spacing_);

    const int i = static_cast<int>(std::lround(vertex.x / step));
    const int j = static_cast<int>(std::lround(vertex.y / step));

    std::vector<Edit> &bucket = edits_[Key(cx, cy)];

    // Overwritten rather than appended. A vertex has one state per layer, so
    // digging out what was placed there leaves one record saying it is empty, and
    // a stroke swept back and forth over the same spot costs what one stroke
    // costs.
    //
    // The layer written is the only one touched. That is the whole of what makes a
    // wall survive the block in front of it being dug: the two live in one record
    // and neither erases the other.
    //
    // A linear scan, and it can afford to be: a bucket holds at most the
    // vertices of one chunk, and a stroke of any usable size covers a hundredth
    // of them.
    for (Edit &edit : bucket) {
        if (edit.i != i || edit.j != j) continue;

        if (behind) {
            edit.behind = element;
            edit.back   = true;
        } else {
            edit.element = element;
            edit.front   = true;
        }

        return;
    }

    if (behind) {
        bucket.push_back({.i = i, .j = j, .behind = element, .back = true});
    } else {
        bucket.push_back({.i = i, .j = j, .element = element, .front = true});
    }
}

void World::FromKey(std::int64_t key, int &outI, int &outJ) {
    outI = static_cast<int>(key >> 32);
    outJ = static_cast<int>(static_cast<std::uint32_t>(key));
}

void World::Disturb(Vector2 vertex) {
    const float step = static_cast<float>(spacing_);

    const int i = static_cast<int>(std::lround(vertex.x / step));
    const int j = static_cast<int>(std::lround(vertex.y / step));

    // Overwritten rather than kept alongside, so turning the same ground over
    // twice starts the clock again instead of leaving the first record to expire
    // on its own. Which is what happens out in the world: ground disturbed again
    // is disturbed ground, however established it had become.
    sown_[Key(i, j)] = sky_.Time();
}

void World::ApplyEdits(Chunk &chunk, int cx, int cy) const {
    if (edits_.empty()) return;

    const float step = static_cast<float>(spacing_);

    for (int dx = 0; dx <= 1; dx++) {
        for (int dy = 0; dy <= 1; dy++) {
            const auto found = edits_.find(Key(cx + dx, cy + dy));
            if (found == edits_.end()) continue;

            for (const Edit &edit : found->second) {
                const Vector2 vertex = {static_cast<float>(edit.i) * step, static_cast<float>(edit.j) * step};

                int i = 0;
                int j = 0;
                chunk.fields[0].ToLocal(vertex, i, j);

                // Filed under a neighbouring chunk and not reaching into this
                // one. Only the vertices on a shared border do.
                if (!chunk.fields[0].InBounds(i, j)) continue;

                // Emptied and then refilled, because that is what the brush did:
                // two materials never share a vertex, so placing one is a
                // replacement and digging is the same edit without its second
                // half.
                //
                // **Only where the record speaks for this layer.** Which is what
                // `front` is for, and leaving it out was a wall that ate the ground
                // it was put up against: a record made by hanging a wall says
                // nothing about the block in front of it, and an empty `element`
                // was read as saying there is none — so the next time the chunk was
                // built, the hillside the generator had put there was cleared and
                // the wall was all that was left. It survived a session only
                // because an edited chunk is pinned; walk far enough away to drop
                // it, come back, and the hillside was gone.
                //
                // Solids only. Liquid is simulation rather than intent — it moves
                // on its own, so where it was poured says nothing about where it
                // is — and it has its own reason to keep a chunk resident.
                if (edit.front) {
                    for (std::size_t e = 0; e < kElementCount; e++) {
                        if (!kElements[e].rules.occupies) continue;

                        chunk.fields[e].SetValue(i, j, 0.0f);
                    }

                    if (edit.element.has_value()) {
                        chunk.fields[ElementIndex(*edit.element)].SetValue(i, j, 1.0f);
                    }
                }

                // The layer behind, cleared and refilled on its own terms and only
                // where the record is about it. The generator never puts a wall
                // anywhere — they are all hand-built — so a record that speaks for
                // this layer and holds nothing is an empty wall, and there is
                // nothing to fall back to.
                if (edit.back) {
                    for (std::size_t e = 0; e < kElementCount; e++) {
                        if (!kElements[e].rules.background) continue;

                        chunk.fields[e].SetValue(i, j, 0.0f);
                    }

                    if (edit.behind.has_value()) {
                        chunk.fields[ElementIndex(*edit.behind)].SetValue(i, j, 1.0f);
                    }
                }

                // So the rebuilt chunk is pinned exactly as the original was, and
                // so anything that looks only at hand-touched chunks — the rain's
                // surface, for one — still finds it.
                chunk.edited = true;
            }
        }
    }
}

void World::Update(Rectangle view) {
    PROFILE_ZONE("world.Update");

    // Widened by the margin the light will snap its own region out to.
    //
    // A cell the light asks about outside a resident chunk is answered from the
    // noise, and answering one that way costs as much as generating a hundred:
    // every material has to be evaluated and then contested against every other.
    // A few hundred of them along the edge of the region cost more than the
    // whole solve, and nothing about it looked wrong, which is the worst kind
    // of slow.
    // Chunks are held over the *light's* region, which is bigger than the view, and
    // the figure has to be derived rather than picked.
    //
    // StepLight rounds its half-width up to a power of two, so the region can be
    // anything up to twice the view across before it rounds again -- which puts its
    // edge up to half a view beyond the screen on each side. Every cell out there is
    // still asked about, and one asked outside a resident chunk is answered from the
    // noise: every material evaluated and then contested against every other, at
    // about a hundred times the cost of reading a generated one.
    //
    // Set too small this does not look wrong, it just runs. Measured at 64 cells of
    // margin against a region 512 cells across, the fill was **235 ms of a 279 ms
    // frame** while the solve it was feeding took 3.5, and nothing on screen said so.
    //
    // So it is not a margin any more. The light is asked where it will look — see
    // LitRegion, which is the one place that rule lives — and the answer is covered
    // whole, with a little over for the snap and for the region the medium is still
    // holding on to from a wider view it has not shrunk back from yet.
    const float step = static_cast<float>(spacing_);

    const Lit lit = LitRegion(view);

    const float over = step * 4.0f;

    float left  = std::min(view.x, static_cast<float>(lit.i0) * step) - over;
    float top   = std::min(view.y, static_cast<float>(lit.j0) * step) - over;
    float right = std::max(view.x + view.width, static_cast<float>(lit.i0 + lit.cols) * step) + over;
    float low   = std::max(view.y + view.height, static_cast<float>(lit.j0 + lit.rows) * step) + over;

    // And whatever the medium is standing on now, which the hysteresis in StepLight
    // can hold wider than the view asks for.
    if (medium_.cols > 0 && medium_.rows > 0) {
        left  = std::min(left, medium_.origin.x - over);
        top   = std::min(top, medium_.origin.y - over);
        right = std::max(right, medium_.origin.x + static_cast<float>(medium_.cols) * step + over);
        low   = std::max(low, medium_.origin.y + static_cast<float>(medium_.rows) * step + over);
    }

    const Rectangle covered = {left, top, right - left, low - top};

    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(covered, minCx, minCy, maxCx, maxCy);

    {
        PROFILE_ZONE("chunks");

        // Built across the cores and settled here.
        //
        // Walking into new country brings a whole column of chunks in at once,
        // and generating one is the most expensive single thing the world does —
        // eight octaves of surface per vertex, then every material laid down and
        // contested against every other. They do not depend on each other, so the
        // only part that has to happen in order is putting them into the map.
        pending_.clear();

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                if (Find(cx, cy) == nullptr) pending_.push_back({cx, cy});
            }
        }

        if (!pending_.empty()) {
            built_.clear();
            built_.resize(pending_.size());

            // One chunk per block and no floor under the count: a chunk costs a
            // millisecond or two to generate, so two of them are already worth
            // splitting and there is nothing to be gained by taking them in runs.
            pool::For(
                static_cast<int>(pending_.size()),
                [&](int k) { built_[static_cast<std::size_t>(k)] = Build(pending_[k].cx, pending_[k].cy); }, 2, 1);

            for (std::size_t k = 0; k < pending_.size(); k++) {
                Settle(pending_[k].cx, pending_[k].cy, std::move(built_[k]));
            }
        }
    }

    // The grass over what is now in play, worked out once here rather than in the
    // draw, because the tufts standing on it are drawn by somebody else and the
    // two have to be told the same thing.
    ReadSod(view);

    // Anything released below may be the chunk the recent-chunk pointer refers
    // to, and there is no cheap way to tell which, so it is dropped either way.
    ForgetRecent();

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        int cx = 0;
        int cy = 0;
        ToChunk(it->second.fields[0].Origin(), cx, cy);

        // Distance to the region in chunks, zero for one inside it.
        const int distance = std::max({minCx - cx, cx - maxCx, minCy - cy, cy - maxCy, 0});

        const bool pinned = it->second.edited || it->second.holdsLiquid;
        const int margin  = pinned ? kDropMargin : kKeepMargin;

        if (distance > margin) {
            DropPainted(cx, cy);

            it = chunks_.erase(it);
            continue;
        }

        it = std::next(it);
    }
}

void World::Reset() {
    ForgetRecent();

    chunks_.clear();
    skyline_.clear();

    // Regenerating moves the ground, so nothing the band remembers about it
    // survives, and neither does any picture taken of it.
    sodColumns_.clear();

    for (Painted &slot : painted_) slot.holds = false;

    paintedOf_.clear();

    // The edits too, or the world would come back with everything ever built in
    // it still standing. Regenerating means from the noise alone.
    edits_.clear();

    // And what was outstanding about them. A world with nothing built in it has
    // no turned earth waiting to green over.
    sown_.clear();
}

int World::PinnedChunks() const {
    int pinned = 0;

    for (const auto &[key, chunk] : chunks_) {
        if (chunk.edited || chunk.holdsLiquid) pinned++;
    }

    return pinned;
}

int World::RememberedEdits() const {
    std::size_t total = 0;
    for (const auto &[key, bucket] : edits_) total += bucket.size();

    return static_cast<int>(total);
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

World::Vertex World::Resolve(Vector2 world) const {
    Vertex vertex;

    vertex.at = SnapToLattice(world);

    int cx = 0;
    int cy = 0;
    ToChunk(vertex.at, cx, cy);

    vertex.chunk = Find(cx, cy);
    if (vertex.chunk == nullptr) return vertex;

    // Every field of a chunk is laid out the same, so the first one answers for
    // all of them.
    const Grid &field = vertex.chunk->fields[0];

    field.ToLocal(vertex.at, vertex.i, vertex.j);

    vertex.resident = field.InBounds(vertex.i, vertex.j);

    return vertex;
}

float World::ValueAt(const Vertex &vertex, Element element) const {
    if (!vertex.resident) return SpawnValue(element, vertex.at);

    return vertex.chunk->fields[ElementIndex(element)].ValueAt(vertex.i, vertex.j);
}

void World::WriteVertex(Element element, Vector2 vertex, float value) {
    // A lattice position on a chunk border exists in more than one chunk. Every
    // copy is updated, otherwise a later read could pick the stale one and the
    // amount of liquid in the world would depend on which chunk answered.
    int cx = 0;
    int cy = 0;
    ToChunk(vertex, cx, cy);

    const float span = ChunkSpan();
    const float step = static_cast<float>(spacing_);

    // A change to something a body stands on moves the surface the grass is
    // grown along, so what the band remembered about those columns is dropped.
    // A neighbour either side, because the surface is read by interpolating
    // between vertices and so a vertex has a say in the columns beside it.
    //
    // Liquid is not one of these. It moves every frame and nothing stands on it,
    // and invalidating on it would throw the whole band away whenever it rained.
    if (Def(element).rules.blocksBodies) ForgetSod(vertex.x - step, vertex.x + step);

    for (int dx = -1; dx <= 0; dx++) {
        for (int dy = -1; dy <= 0; dy++) {
            // Which local vertex this would be in that chunk, worked out the way
            // Grid::ToLocal does. Only a vertex on a seam falls inside more than
            // one chunk, so this settles three of the four neighbours without
            // asking the map about them — and the map was being asked four times
            // per vertex written, for every vertex of the water pass.
            const int i = static_cast<int>(std::lround((vertex.x - static_cast<float>(cx + dx) * span) / step));
            const int j = static_cast<int>(std::lround((vertex.y - static_cast<float>(cy + dy) * span) / step));

            if (i < 0 || i >= kChunkVertices || j < 0 || j >= kChunkVertices) continue;

            Chunk *held = Find(cx + dx, cy + dy);
            if (held == nullptr) continue;

            Chunk &chunk = *held;
            Grid &field  = chunk.fields[ElementIndex(element)];

            field.SetValue(i, j, value);

            // Every silhouette this chunk remembers is a maximum over the fields
            // of materials that occupy their vertex, so writing one invalidates
            // them all. A liquid occupies nothing and leaves them standing, which
            // is what keeps them from being thrown away every frame it rains.
            if (Def(element).rules.occupies) {
                chunk.silhouettes.clear();

                // And the picture made from them, which is the same fact one step
                // further on.
                DropPainted(cx + dx, cy + dy);
            }

            // A wall is in no silhouette — it contests nothing, so there is no
            // maximum for it to be part of — but it *is* in the picture, painted
            // behind the ground. Without this a wall put up appears only when
            // something else happens to drop the chunk's texture, which is the kind
            // of bug that looks like the click was missed.
            if (Def(element).rules.background) DropPainted(cx + dx, cy + dy);

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
            if (it == chunks_.end()) continue;

            // Only the chunks that actually hold a copy of the vertex, which is
            // four at a corner and one in the middle of a chunk. This used to
            // mark all four regardless, so a single block set down anywhere
            // pinned the three chunks around it as well — quadrupling the very
            // memory the drop margin exists to bound, and disagreeing with the
            // same walk in WriteVertex, which has always bounds-checked.
            int i = 0;
            int j = 0;
            it->second.fields[0].ToLocal(vertex, i, j);

            if (it->second.fields[0].InBounds(i, j)) it->second.edited = true;
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

bool World::ClearVertex(Vector2 vertex, Yield &yield) {
    bool removed = false;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (!def.rules.occupies && !def.rules.flows) continue;

        const Element element = static_cast<Element>(e);
        const float value     = ValueAt(element, vertex);

        // Already empty. Skipping it matters beyond the wasted write: marking
        // the chunk edited is what pins it in memory, and a brush swept through
        // open sky would otherwise pin everything it passed over — and now
        // remember every vertex it passed over as well.
        if (value <= 0.0f) continue;

        if (value > def.threshold) yield[e]++;

        WriteVertex(element, vertex, 0.0f);

        if (!def.rules.flows) {
            MarkEdited(vertex);
            removed = true;
        }
    }

    return removed;
}

bool World::ClearWall(Vector2 vertex, Yield &yield) {
    bool removed = false;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (!def.rules.background) continue;

        const Element element = static_cast<Element>(e);
        const float value     = ValueAt(element, vertex);

        if (value <= 0.0f) continue;

        if (value > def.threshold) yield[e]++;

        WriteVertex(element, vertex, 0.0f);
        MarkEdited(vertex);
        Remember(vertex, std::nullopt, true);

        removed = true;
    }

    return removed;
}

bool World::VertexMeets(Vector2 vertex, Rectangle rect) const {
    // Empty stands for "nothing to keep clear", which is what a caller with no
    // body to protect passes. Tested rather than assumed, since a zero-width
    // rectangle at the origin would otherwise still claim the vertices around it.
    if (rect.width <= 0.0f || rect.height <= 0.0f) return false;

    const float half = static_cast<float>(spacing_) / 2.0f;

    if (vertex.x + half < rect.x || vertex.x - half > rect.x + rect.width) return false;
    if (vertex.y + half < rect.y || vertex.y - half > rect.y + rect.height) return false;

    return true;
}

World::Stroke World::ApplyStroke(const Reach &reach, std::optional<Element> place, int budget, Rectangle keepClear) {
    Stroke edit{};

    // Whether the stroke changed any ground at all, which is what the grass has
    // to be told about. Every way out of the loop below goes through `finish`, so
    // the telling cannot be skipped by the budget running out mid-stroke.
    bool turned = false;

    const auto finish = [&] {
        // The band was worked out at the top of the frame, before this stroke
        // existed. Left alone it would draw one frame of established grass over
        // ground the stroke has just turned over — and since a held brush turns
        // fresh ground every frame it moves, that single frame is a green fringe
        // running along the leading edge of the stroke for as long as it is swept.
        if (turned) ReadSown();

        return edit;
    };

    const float step = static_cast<float>(spacing_);

    for (int i = reach.i0; i <= reach.i1; i++) {
        for (int j = reach.j0; j <= reach.j1; j++) {
            const Vector2 vertex = {i * step, j * step};

            // A liquid is poured into a space, not pressed into one. It does
            // not clear what it lands on; it simply does not land there.
            if (place.has_value() && Def(*place).rules.flows) {
                if (OccupantAt(vertex).has_value()) continue;

                if (edit.filled >= budget) return finish();

                WriteVertex(*place, vertex, water::kMaxMass);
                edit.filled++;
                continue;
            }

            // A wall goes in behind whatever is already there and takes nothing
            // out. It contests no vertex, so there is nothing to clear and nothing
            // to hand back — which is what lets a wall be put up through a wall of
            // planks and be there when they come down.
            if (place.has_value() && Def(*place).rules.background) {
                const bool gained = ValueAt(*place, vertex) <= Def(*place).threshold;

                if (gained && edit.filled >= budget) return finish();

                WriteVertex(*place, vertex, 1.0f);
                MarkEdited(vertex);
                Remember(vertex, *place, true);

                if (gained) edit.filled++;
                continue;
            }

            if (place.has_value()) {
                // Not into the character standing there. Skipped outright rather
                // than refused as a stroke, so a brush wider than the body still
                // lays the ground around the feet and only leaves the body's own
                // room out of it — which is what a player aiming down at their own
                // feet to build a floor is asking for.
                if (Def(*place).rules.blocksBodies && VertexMeets(vertex, keepClear)) continue;

                // Whether this vertex is one the material does not already hold.
                // Only those are paid for, and only those may exhaust the
                // budget: a stroke laid over its own work costs nothing because
                // it gains nothing.
                const bool gained = ValueAt(*place, vertex) <= Def(*place).threshold;

                // Tested before anything is cleared. The other order digs out a
                // vertex and then finds there is nothing left to fill it with,
                // which leaves a hole the player was never charged for and never
                // asked for.
                if (gained && edit.filled >= budget) return finish();

                // What the vertex gave up, except any of the material now going
                // into it.
                //
                // ClearVertex counts everything it finds, and a brush passing
                // back over its own wall finds that wall — so without this a
                // stroke would hand back a vertex of stone for every vertex of
                // stone it rewrote, and a player could stand still and make
                // material out of nothing.
                const std::size_t self = ElementIndex(*place);
                const int held         = edit.freed[self];

                ClearVertex(vertex, edit.freed);
                edit.freed[self] = held;

                // Painted samples are pinned to the top of the range rather than
                // nudged, so a brush stroke reads as a definite edit and not as
                // a faint gradient.
                WriteVertex(*place, vertex, 1.0f);
                MarkEdited(vertex);
                Remember(vertex, *place, false);

                // Only where the vertex actually changed hands, and only for the
                // one material grass grows on. A brush swept back over its own
                // wall gains nothing, and a stroke held over the same spot would
                // otherwise keep resetting the clock on ground it was not
                // changing — so grass could never take under a player standing
                // still with the button down.
                //
                // Laying anything else over a lawn already puts the grass out, by
                // the ordinary rule that what the surface holds is what grows
                // there; digging that same block back off is what starts the
                // clock, and the other hand below is where that is noted.
                if (gained && *place == Element::Soil) {
                    Disturb(vertex);
                    turned = true;
                }

                if (gained) edit.filled++;
                continue;
            }

            // Digging is that same edit without its second half. Two materials
            // never share a vertex -- which is why a cell already holding one is
            // refused outright by PlaceCell rather than being cleared here.
            //
            // Only where there was something to dig. A brush swung through open
            // sky has not changed the world and must not be remembered as
            // having: the memory is what makes an edit outlive its chunk, and it
            // is the one thing here that never shrinks.
            //
            // The earth a dig uncovered is turned earth as much as the earth a
            // hand laid, so it is noted on the same terms: the floor of a scoop
            // taken out of a hillside was inside the ground a moment ago, and
            // grass has to reach it the way it reaches anything else.
            //
            // Where there is soil to uncover, and nowhere else. Read before the
            // vertex is emptied, since afterwards there is nothing left to ask.
            // Either the vertex is soil, which a dig into a hillside takes and
            // leaves more of underneath, or the one below it is, which is a dig
            // taking the last of whatever was standing on a lawn.
            //
            // What this spares is the mine. A tunnel driven through rock turns
            // over hundreds of vertices a second, not one of which any grass will
            // ever stand on, and every one of them would otherwise be carried in
            // the record and walked over once a frame until it expired.
            const bool overSoil = ValueAt(Element::Soil, vertex) > Def(Element::Soil).threshold
                               || ValueAt(Element::Soil, {vertex.x, vertex.y + step}) > Def(Element::Soil).threshold;

            if (ClearVertex(vertex, edit.freed)) {
                Remember(vertex, std::nullopt, false);

                if (overSoil) {
                    Disturb(vertex);
                    turned = true;
                }
            }
        }
    }

    return finish();
}

World::Reach World::CellReach(int cx, int cy) const {
    // Counted in vertices from the origin rather than derived from the cell's
    // rectangle. A cell owns config::kBuildCellVertices of them across, and the
    // one on its far edge belongs to the cell beyond — see Reach.
    const int across = config::kBuildCellVertices;

    return {.i0 = cx * across, .j0 = cy * across, .i1 = cx * across + across - 1, .j1 = cy * across + across - 1};
}


void World::ToCell(Vector2 world, int &outCx, int &outCy) {
    outCx = FloorDiv(static_cast<int>(std::floor(world.x + config::kCellOffset)), config::kBuildCell);
    outCy = FloorDiv(static_cast<int>(std::floor(world.y + config::kCellOffset)), config::kBuildCell);
}

Rectangle World::CellBounds(int cx, int cy) {
    const auto side = static_cast<float>(config::kBuildCell);

    return {cx * side - config::kCellOffset, cy * side - config::kCellOffset, side, side};
}

bool World::WalledAt(int cx, int cy) const {
    const Rectangle at = CellBounds(cx, cy);

    const Vector2 middle = {at.x + at.width * 0.5f, at.y + at.height * 0.5f};

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (!def.rules.background) continue;

        if (ValueAt(static_cast<Element>(e), middle) > def.threshold) return true;
    }

    return false;
}

bool World::CellClear(int cx, int cy, Rectangle keepClear) const {
    if (keepClear.width <= 0.0f || keepClear.height <= 0.0f) return true;

    // The cell's bounds are exactly the union of the squares its vertices own —
    // see config::kCellOffset — and VertexMeets tests one of those squares against a
    // rectangle. So the body reaching any vertex of the cell is the body reaching
    // the cell, asked once instead of nine times.
    return !CheckCollisionRecs(CellBounds(cx, cy), keepClear);
}

bool World::CellVacant(int cx, int cy) const {
    Yield holds{};

    CellHolds(cx, cy, holds);

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].rules.occupies && holds[e] > 0) return false;
    }

    return true;
}

World::Stroke World::PlaceCell(Element element, int cx, int cy, Rectangle keepClear) {
    // Something is already there. Refused whole rather than replacing it -- see
    // CellVacant, which is where the reasoning lives.
    //
    // Here and not only in the hand, so that no caller can be written later that
    // quietly gets the old behaviour back. It is the same argument that puts the
    // body's refusal below in the world rather than in the editor.
    //
    // Only for what *occupies*, and the two exceptions are the point rather than
    // edge cases. A wall contests no vertex and its whole purpose is to stand behind
    // a block (§11.2) -- refusing it where a block stands would make a wall
    // impossible to put up anywhere it is wanted. A liquid is poured into a space
    // rather than pressed into one and ApplyStroke already declines the vertices a
    // solid fills, so it needs nothing here.
    if (Def(element).rules.occupies && !CellVacant(cx, cy)) return {};

    // All of the cell, or none of it.
    //
    // This is where a cell and a brush have to part company, and it is the fault
    // behind "sometimes a block comes out with a piece missing". ApplyStroke skips
    // the vertices inside the player's body and lays the rest, which is exactly
    // right for a stroke covering an area — a floor laid around one's own feet is
    // what the player was asking for. It is wrong for a unit: the skipped vertices
    // come out as a bite taken out of the block, the block is charged for whole,
    // and nothing on screen says why it is the shape it is.
    //
    // So a cell the body is standing in is refused outright rather than delivered
    // broken. The player steps aside or jumps, which is what Minecraft and
    // Terraria both ask of them, and every block that does go down is a block.
    if (Def(element).rules.blocksBodies && !CellClear(cx, cy, keepClear)) return {};

    // The whole cell otherwise, so a click is never stopped part way through one.
    // What stops a player with nothing left is the caller, which asks whether
    // there is a block to spend before it asks the world for anything.
    return ApplyStroke(CellReach(cx, cy), element, config::kBuildCellArea, keepClear);
}

void World::CellHolds(int cx, int cy, Yield &out) const {
    const Reach reach = CellReach(cx, cy);
    const float step  = static_cast<float>(spacing_);

    bool front = false;

    for (int i = reach.i0; i <= reach.i1; i++) {
        for (int j = reach.j0; j <= reach.j1; j++) {
            // Resolved once and then read ten times, rather than snapped, divided
            // and looked up once per material — see World::Vertex. This runs every
            // frame a player holds the button, over up to sixty-four cells.
            const Vertex vertex = Resolve({static_cast<float>(i) * step, static_cast<float>(j) * step});

            for (std::size_t e = 0; e < kElementCount; e++) {
                const ElementDef &def = kElements[e];
                if (!def.rules.occupies && !def.rules.flows) continue;

                if (ValueAt(vertex, static_cast<Element>(e)) > def.threshold) {
                    out[e]++;
                    front = true;
                }
            }
        }
    }

    // The wall only where the space in front of it is open, which is the order the
    // spade takes them in.
    if (front) return;

    for (int i = reach.i0; i <= reach.i1; i++) {
        for (int j = reach.j0; j <= reach.j1; j++) {
            const Vertex vertex = Resolve({static_cast<float>(i) * step, static_cast<float>(j) * step});

            for (std::size_t e = 0; e < kElementCount; e++) {
                const ElementDef &def = kElements[e];
                if (!def.rules.background) continue;

                if (ValueAt(vertex, static_cast<Element>(e)) > def.threshold) out[e]++;
            }
        }
    }
}

World::Stroke World::ExcavateCell(int cx, int cy) {
    Stroke out = ApplyStroke(CellReach(cx, cy), std::nullopt, 0, {});

    // Whatever was in front of the wall, if anything was.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (out.freed[e] > 0) return out;
    }

    // Nothing was, so the wall is what the spade was pointed at.
    //
    // Terraria's order, and the only one that lets a wall be built behind a block
    // and taken down again: while the block is there the spade takes the block,
    // and the wall is reachable exactly when the space in front of it is open. A
    // spade that took both at once would make a wall impossible to keep, and one
    // that took neither would make it impossible to remove.
    const Reach reach = CellReach(cx, cy);
    const float step  = static_cast<float>(spacing_);

    for (int i = reach.i0; i <= reach.i1; i++) {
        for (int j = reach.j0; j <= reach.j1; j++) {
            ClearWall({static_cast<float>(i) * step, static_cast<float>(j) * step}, out.freed);
        }
    }

    return out;
}

void World::StepWater(Rectangle active) {
    PROFILE_ZONE("StepWater");

    int i0 = 0;
    int j0 = 0;
    int i1 = 0;
    int j1 = 0;
    LatticeRange(active, i0, j0, i1, j1);

    const int cols = i1 - i0 + 1;
    const int rows = j1 - j0 + 1;
    if (cols <= 0 || rows <= 0) return;

    if (scratch_.cols != cols || scratch_.rows != rows) scratch_.Resize(cols, rows);

    settled_.assign(static_cast<std::size_t>(cols) * rows, 0.0f);

    const float step = static_cast<float>(spacing_);

    // Every chunk overlapping the region is assumed dry until the write-back
    // says otherwise, so a chunk that drains can be released again instead of
    // staying pinned for the rest of the session.
    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(active, minCx, minCy, maxCx, maxCy);

    // Only the chunks the region covers *whole*, and that restriction is the
    // point of the loop rather than a detail of it.
    //
    // The flag says a chunk holds liquid the noise cannot reproduce, and clearing
    // it un-pins the chunk. A chunk the region only partly covers has liquid this
    // pass never looked at, so the write-back below cannot speak for it: clearing
    // it there dropped the chunks along the edge of the simulated band from the
    // long pinned distance to the short unpinned one, and what that looked like
    // was water that moved every time the player walked into a new chunk.
    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const auto it = chunks_.find(Key(cx, cy));
            if (it == chunks_.end()) continue;

            const float span = ChunkSpan();
            const float left = static_cast<float>(cx) * span;
            const float top  = static_cast<float>(cy) * span;

            const bool whole = left >= active.x && top >= active.y && left + span <= active.x + active.width
                            && top + span <= active.y + active.height;

            if (whole) it->second.holdsLiquid = false;
        }
    }

    // One pass per flowing material. They share the scratch buffer and are
    // simulated independently: each sees the others only as obstacles, through
    // the blocksLiquid rule.
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (!kElements[e].rules.flows) continue;

        const Element element = static_cast<Element>(e);

        {
            PROFILE_ZONE("water read");

            // The materials a liquid cannot pass through, gathered once rather
            // than tested per vertex.
            std::array<Element, kElementCount> walls{};
            std::size_t count = 0;

            for (std::size_t w = 0; w < kElementCount; w++) {
                if (kElements[w].rules.blocksLiquid) walls[count++] = static_cast<Element>(w);
            }

            // A column at a time across the cores, on the same terms as the
            // light's medium: the world is only read, and each column writes only
            // its own part of the scratch buffer.
            pool::For(cols, [&](int i) {
                for (int j = 0; j < rows; j++) {
                    const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
                    const int cell       = scratch_.Index(i, j);

                    // The chunk once for the liquid and for every wall that could
                    // stop it, rather than once per material — see World::Vertex.
                    const Vertex at = Resolve(vertex);

                    const float mass = ValueAt(at, element);

                    scratch_.mass[cell] = mass;
                    settled_[cell]      = mass;

                    bool blocked = false;

                    for (std::size_t w = 0; w < count && !blocked; w++) {
                        blocked = ValueAt(at, walls[w]) > Def(walls[w]).threshold;
                    }

                    scratch_.blocked[cell] = blocked ? 1 : 0;
                }
            });
        }

        {
            PROFILE_ZONE("water step");

            water::Step(scratch_, waterSettings_);
        }

        {
            PROFILE_ZONE("water write");

            for (int i = 0; i < cols; i++) {
                for (int j = 0; j < rows; j++) {
                    const int cell   = scratch_.Index(i, j);
                    const float mass = scratch_.mass[cell];

                    // A dry vertex the step did not touch has nothing to store:
                    // the value in the world is already this one. Skipping it is
                    // what keeps the write-back the size of the water rather than
                    // the size of the band — see World::settled_.
                    //
                    // A wet one is always written, unchanged or not, because the
                    // write is also what tells its chunk it is holding liquid and
                    // the flag was cleared at the top of this step.
                    if (mass <= water::kDryMass && mass == settled_[cell]) continue;

                    const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};

                    WriteVertex(element, vertex, mass);
                }
            }
        }
    }
}

float World::Skyline(int column) const {
    const auto found = skyline_.find(column);
    if (found != skyline_.end()) return found->second;

    // Worked out and not remembered while the band is being walked across the
    // cores, because the record is a hash map and one thread writing into it
    // while the others read is a race. The walk warms it first — see
    // World::WarmSkyline — so this is a path a parallel pass should never take,
    // and computing rather than refusing keeps it correct if one ever does.
    if (!skylineWritable_) return ScanSkyline(column);

    const float ground = ScanSkyline(column);

    skyline_.emplace(column, ground);

    return ground;
}

float World::ScanSkyline(int column) const {
    const float step = static_cast<float>(spacing_);
    const float x    = column * step;

    // Started just above the surface the generator reports for this column,
    // rather than at a fixed height. The surface is a function of the column
    // alone, so there is nothing to search for above it.
    //
    // Note what this leaves behind: the scan begins at an arbitrary height and
    // steps by the lattice, so the answer is `Height - headroom + k*step` and is
    // **not** on the lattice. Anything that compares this against a query which
    // snaps to a vertex — IsSolidAt does — is comparing two grids up to half a
    // step apart, and half a step at the edge of the ground is the difference
    // between rock and sky. Callers have to allow for it; Grove::Undermine says
    // how. Aligning the scan here would straighten it for everyone, but it also
    // moves where every raindrop lands, so it is its own change.
    float ground = terrain::Height(x, settings_) - kSkylineHeadroom;

    // Then followed down to the first ground it meets, since the surface is not
    // the whole answer: where an entrance has opened the ground up, the sky
    // reaches past it into the shaft.
    //
    // Read from the terrain function alone, not from the world: it is the shape
    // of the land that decides what the sky can see, and asking the world would
    // mean asking chunks far above the one being lit, which are not resident and
    // would be answered from this same noise anyway.
    //
    // Bounded, because a column open a long way down is a shaft, and past the
    // bound the answer stops mattering: nothing that deep is lit by the sky.
    const float bottom = ground + kSkylineScan;
    while (ground < bottom && !terrain::IsSolid({x, ground}, settings_, RockThreshold())) ground += step;

    return ground;
}

void World::WarmSkyline(int firstColumn, int lastColumn) {
    // Cheap for a column already in the record and the whole of the scan for one
    // that is not, which is why it happens here in order rather than inside the
    // walk that runs across the cores.
    for (int column = firstColumn; column <= lastColumn; column++) Skyline(column);
}

bool World::SolidVertex(const Chunk &chunk, int i, int j) const {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (!kElements[e].rules.blocksBodies) continue;
        if (chunk.fields[e].ValueAt(i, j) > kElements[e].threshold) return true;
    }

    return false;
}

void World::SurfaceProfile(int firstColumn, int count, std::vector<float> &out) const {
    out.assign(static_cast<std::size_t>(std::max(count, 0)), 0.0f);
    if (out.empty()) return;

    const float step = static_cast<float>(spacing_);

    // The land first. A pure function of the column and remembered between frames,
    // so after one pass over a stretch of world this is a lookup each.
    for (int i = 0; i < count; i++) out[static_cast<std::size_t>(i)] = Skyline(firstColumn + i);

    // Then what has been built on it, which the noise cannot know about and which
    // is the whole reason this is not simply the skyline.
    //
    // Only chunks that have been painted are opened. An untouched one holds exactly
    // the field the skyline already read, vertex for vertex, so a world nobody has
    // built in pays one walk of the chunk map and nothing else — and one that has
    // been built in pays a scan of the few chunks it was built in.
    //
    // Lowered only. A column dug out below the surface still answers with the
    // surface the noise describes, which is what the light solve does with the same
    // skyline and for the same reason: the noise is the only thing that can be asked
    // about a column whose chunks are not resident. Rain stopping a few pixels above
    // the floor of a fresh pit is a smaller wrong than rain falling through a roof.
    for (const auto &entry : chunks_) {
        const Chunk &chunk = entry.second;
        if (!chunk.edited) continue;

        const Grid &any = chunk.fields[0];

        const int base  = static_cast<int>(std::lround(any.Origin().x / step));
        const int first = std::max(firstColumn, base);
        const int last  = std::min(firstColumn + count, base + any.Cols());

        for (int column = first; column < last; column++) {
            float &surface = out[static_cast<std::size_t>(column - firstColumn)];

            for (int j = 0; j < any.Rows(); j++) {
                const float y = any.Origin().y + static_cast<float>(j) * step;

                // Below the answer already held, so nothing further down this column
                // can better it. The topmost solid is what is wanted, and the rows
                // are walked from the top.
                if (y >= surface) break;

                if (SolidVertex(chunk, column - base, j)) {
                    surface = y;
                    break;
                }
            }
        }
    }
}

void World::SetDaylight(light::Radiance noon, light::Radiance midnight) {
    dayLight_   = noon;
    nightLight_ = midnight;
}

void World::StepWeather(float dt) {
    sky_.Advance(dt);

    const weather::Daylight &today = sky_.Today();
    const float exposure           = std::max(lightSettings_.exposure, 1e-3f);

    // The day, mixed where the eye reads it rather than where the light is measured.
    //
    // This is the one arithmetic decision in the whole cycle and getting it wrong
    // makes the feature look broken. Light reaches the screen through
    // 1 - exp(-value * exposure), and daylight sits so far up that curve that it is
    // saturated: halving the radiance leaves the screen five per cent darker. Mixing
    // there gives an afternoon where nothing happens for hours and then a cliff into
    // night. So the two ends are exposed first, mixed as brightnesses, and the
    // result put back through the curve to find the radiance that produces it.
    //
    // Per channel, because the two ends are different colours and not one dimmed.
    auto expose  = [&](float value) { return 1.0f - std::exp(-value * exposure); };
    auto radiate = [&](float lit) { return -std::log1p(-std::clamp(lit, 0.0f, 1.0f - 1e-4f)) / exposure; };

    // Tinted by the colour the sun has left, but only partly. The sky picture
    // carries the saturated version of a sunset; the ground wants the suggestion of
    // one, or an evening comes out brown rather than golden.
    const float warm = 0.35f;

    auto channel = [&](float day, float night, float beam) {
        const float lit = expose(night) + (expose(day) - expose(night)) * today.light;

        return radiate(lit * (1.0f - warm + warm * beam));
    };

    skyLight_ = {channel(dayLight_.r, nightLight_.r, today.beam.x), channel(dayLight_.g, nightLight_.g, today.beam.y),
                 channel(dayLight_.b, nightLight_.b, today.beam.z)};
}

void World::AddLight(Vector2 world, light::Radiance radiance, float radius) {
    sparks_.push_back({world, radiance, radius});
}

// How much of the world the light will ask about, given the view it is handed.
//
// **One rule, and it has to be one.** The region is not the view: the corner is
// snapped, and the size is rounded up to a power of two, so it can reach up to a
// whole view's width past the screen. World::Update has to generate chunks over all
// of it, because a cell asked about outside a resident chunk is answered from the
// noise at about a hundred times the cost — and it used to work that reach out for
// itself, from a formula that only looked at the width.
//
// Measured, in a thousand-pixel window standing still: the light asked about 3,072
// pixels across and the chunks covered 2,768, so **thirteen per cent of the region
// was answered from the noise every frame** — a medium fill of **156 ms** against a
// solve of 3, and nothing on screen to say why. Full screen it happened to fit,
// which is why a flight at 1920 wide never saw it.
World::Lit World::LitRegion(Rectangle view) const {
    Lit lit;

    LatticeRange(view, lit.i0, lit.j0, lit.i1, lit.j1);

    // The probe grid is half the medium each way and the cascades halve its columns
    // once per level, so both halves have to be powers of two -- and both, not just
    // one, because the two quarter-turn quadrants swap which axis is being halved.
    const auto roundUp = [](int value) {
        int side = 2;
        while (side < value) side *= 2;

        return side;
    };

    // The corner is snapped first and the size worked out from where it landed, and
    // that order is the whole of this.
    //
    // Doing it the other way round -- size from the view, then snap the corner down
    // to a stride -- slides the region up to a whole stride left and up while leaving
    // its width alone, so the right and bottom of the screen fall outside it. Outside
    // is not dark, it is *nothing*: the field is only solved where it was asked for,
    // so what that draws is a hard black region covering most of the view, appearing
    // and disappearing as the snap happens to land. It is the same position from one
    // moment to the next and a different picture.
    //
    // Snapped at all because the two interleaved probe grids stand one cell apart: an
    // origin on an odd cell swaps them, and every row of the field changes at once.
    // Two, which is the smallest it can be: the two interleaved probe grids stand one
    // cell apart, so an odd origin swaps them and every row of the field changes at
    // once. Beyond that there is nothing to gain by snapping coarsely, and a great
    // deal to lose.
    //
    // Measured, at a fixed world position, as the region jumps one stride:
    //
    //     stride   direct light   with the bounce
    //          8         0.13 %            0.16 %
    //         32         0.04 %            2.41 %
    //
    // The transport does not care where the region stands -- four decimal places of
    // not caring. What moves is the bounce, and only because the strip a jump newly
    // exposes has no history behind it and has to build one again. That strip is the
    // stride wide, so the disturbance is the stride: walking the same distance costs
    // the same total either way, but a coarse snap saves it all up and spends it in
    // one flash, which is what is seen, while a fine one dribbles it away under the
    // noise floor.
    constexpr int kSnap = 2;

    lit.i0 = FloorDiv(lit.i0, kSnap) * kSnap;
    lit.j0 = FloorDiv(lit.j0, kSnap) * kSnap;

    // Margin past the far edge, so a light just off screen still reaches in and the
    // snap has somewhere to give.
    constexpr int kMargin = 48;

    const auto side = [&](int span) {
        return roundUp(std::max(1, (span + kMargin + 1) / 2)) * 2;
    };

    lit.cols = side(lit.i1 - lit.i0);
    lit.rows = side(lit.j1 - lit.j0);

    return lit;
}

void World::StepLight(Rectangle region) {
    PROFILE_ZONE("StepLight");

    const Lit lit = LitRegion(region);

    int i0 = lit.i0;
    int j0 = lit.j0;

    int cols = lit.cols;
    int rows = lit.rows;

    // And it does not shrink for a wobble.
    //
    // Rounding a span up to a power of two means a view that breathes by one cell
    // across the boundary doubles and halves the region, and every change of size
    // frees and reallocates thirty megabytes of buffers -- which is a visible freeze
    // -- and throws away the accumulated bounce, which is a flash of flat lighting
    // over the whole screen. Kept unless the need has fallen below half, which is a
    // real change of zoom rather than a tremble.
    if (medium_.cols >= cols && medium_.cols / 2 < cols) cols = medium_.cols;
    if (medium_.rows >= rows && medium_.rows / 2 < rows) rows = medium_.rows;

    const float step = static_cast<float>(spacing_);

    medium_.spacing = step;
    medium_.origin  = {i0 * step, j0 * step};

    if (medium_.cols != cols || medium_.rows != rows) {
        medium_.Resize(cols, rows);
    } else {
        medium_.Clear();
    }

    {
        PROFILE_ZONE("medium fill");

        // What each material says to the light, gathered once instead of tested at
        // every one of a hundred thousand cells.
        //
        // Three figures now where there were two, and the third is what makes this
        // global illumination: `albedo` is what the material reflects, and the solver
        // feeds the last frame's light back through it. It is taken from the
        // material's own mid tone rather than authored, because a material reflects
        // roughly what it looks like -- that is what looking like it means -- and a
        // field of its own across the whole table would be a hundred rows restating
        // the colour that is already sitting next to them.
        struct Lit {
            Element element;
            float threshold;
            float sigma;
            light::Radiance albedo;
            light::Radiance glow;
        };

        std::array<Lit, kElementCount> lit{};
        std::size_t count = 0;

        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementDef &def = kElements[e];
            if (def.light.opacity <= 0.0f && def.light.strength <= 0.0f) continue;

            // Opacity is the share one whole cell of travel stops; the solver wants
            // the coefficient behind it, because it integrates over whatever fraction
            // of a cell a ray actually crosses rather than assuming a whole one.
            // Fully opaque is capped rather than infinite: e^-32 is already nothing,
            // and an infinity would poison the closed form the tracer is built on.
            const float share = std::clamp(def.light.opacity, 0.0f, 1.0f);
            const float sigma = (share >= 0.999f) ? 32.0f : -std::log(1.0f - share);

            const Color tone = def.paint.tone[kElementTones / 2];

            lit[count++] = {.element   = static_cast<Element>(e),
                            .threshold = def.threshold,
                            .sigma     = sigma,
                            .albedo    = {tone.r / 255.0f, tone.g / 255.0f, tone.b / 255.0f},
                            .glow      = Glow(def.light.glow, def.light.strength)};
        }

        // A column at a time across the cores. Each one reads the world, which
        // nothing is writing while this runs, and writes only its own column.
        pool::For(cols, [&](int i) {
            for (int j = 0; j < rows; j++) {
                const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};

                // The chunk once for the whole column of materials — see World::Vertex.
                const Vertex at = Resolve(vertex);

                float stopped = 0.0f;
                light::Radiance reflects;
                light::Radiance given;

                for (std::size_t k = 0; k < count; k++) {
                    const Lit &def = lit[k];

                    if (ValueAt(at, def.element) <= def.threshold) continue;

                    // The densest wins, and it brings its own colour with it. A cell
                    // filled twice over is still one cell of wall, and what that wall
                    // reflects is what it is made of rather than the mean of whatever
                    // happens to be there. Light adds, because two things glowing in
                    // one place are brighter than either.
                    if (def.sigma > stopped) {
                        stopped  = def.sigma;
                        reflects = def.albedo;
                    }

                    given = given + def.glow;
                }

                const int cell = medium_.Index(i, j);

                medium_.sigma[cell]    = stopped;
                medium_.albedo[cell]   = reflects;
                medium_.emission[cell] = given;
            }
        });
    }

    // Whatever is shining without being made of anything: a lantern, a torch in the
    // hand, a thrown light.
    for (const Spark &spark : sparks_) {
        // Spread over a disc, and a wide one. The solver aliases sources smaller than
        // about eight cells across -- the paper's one real limitation, from ray
        // segments sitting at fixed positions -- and a light narrower than that also
        // blinks as it is walked past, for the same reason. Eight cells is the floor
        // rather than the figure, so a caller asking for a wider glow still gets one.
        const float radius = std::max(spark.radius, step * 8.0f);
        const int reach    = static_cast<int>(std::ceil(radius / step));

        const int ci = static_cast<int>(std::round((spark.at.x - medium_.origin.x) / step));
        const int cj = static_cast<int>(std::round((spark.at.y - medium_.origin.y) / step));

        for (int di = -reach; di <= reach; di++) {
            for (int dj = -reach; dj <= reach; dj++) {
                const int i = ci + di;
                const int j = cj + dj;

                if (!medium_.InBounds(i, j)) continue;

                const Vector2 vertex = {(i0 + i) * step, (j0 + j) * step};
                const float dx       = vertex.x - spark.at.x;
                const float dy       = vertex.y - spark.at.y;

                const float distance = std::sqrt(dx * dx + dy * dy);
                if (distance >= radius) continue;

                const float fall = 1.0f - distance / radius;
                const int cell   = medium_.Index(i, j);

                medium_.emission[cell] = medium_.emission[cell] + spark.radiance * (fall * fall);
            }
        }
    }

    sparks_.clear();

    // The cloud, laid into the medium as matter.
    //
    // This is the whole of the cloud's shadow and there is no other term for it. A
    // cloud is something in the air that light has to get through, so it is written
    // where it actually is and the shadow falls out of the transport: with the cloud's
    // own outline, softening with its height above the ground, and gone the moment the
    // cloud has passed. Under a broken sky that gives the thing you see from an
    // aeroplane -- separate dark patches shaped like the clouds over them, on ground
    // that is otherwise in full sun.
    //
    // It replaces a share per column. That share was the thickest cloud anywhere above
    // a column applied to the whole of it, which is not a shadow at all: it dimmed the
    // sky above the cloud as much as the ground below, it had no edge to it, and one
    // small cloud drifting past took the light out of everything beneath it from the
    // ground to the top of the world.
    //
    // Two things make it read right, and both are the transport rather than tuning:
    //
    //   - It is never fully opaque. What gets through is exp(-sigma * path), so a wisp
    //     takes a little and a tower takes nearly all, and everything between is
    //     between. A shadow you can still see the ground through is what a real one
    //     looks like, and it is what a crop under it should be able to live on.
    //
    //   - It is lit from above. The cloud has an albedo, so daylight arriving on top of
    //     it scatters, which makes it bright on the sunward side and its underside the
    //     dim grey that a cloud's underside is.
    if (skyCover_) {
        PROFILE_ZONE("cloud");

        // How much a cell of the thickest cloud stops. The deck is about seventy cells
        // deep, so a ray straight down through solid cloud crosses seventy of these:
        // at 0.02 that leaves about a quarter of the light, which is a firm shadow that
        // is still plainly not black.
        constexpr float kCloudSigma = 0.1f;

        // Bright, but well short of the diffuser an albedo near one makes of a volume
        // -- multiple scattering amplifies by 1/(1 - albedo), and at 0.9 that is ten
        // times over, which washes the shadow out again from the inside.
        constexpr light::Radiance kCloudAlbedo = {0.62f, 0.64f, 0.70f};

        const int top    = static_cast<int>(std::ceil((sky_.DeckTop() - medium_.origin.y) / step));
        const int bottom = static_cast<int>(std::floor((sky_.DeckBottom() - medium_.origin.y) / step));

        const int from = std::max(top, 0);
        const int to   = std::min(bottom, rows - 1);

        // One line, once, saying what the pass actually wrote. Whether the deck even
        // falls inside the region, and whether what landed is patchy or a flat sheet,
        // are the two things that decide whether a cloud shadow can have a shape at
        // all -- and neither can be told from the picture.
        static bool reported = false;

        if (!reported) {
            reported = true;

            float most  = 0.0f;
            double sum  = 0.0;
            int filled  = 0;

            for (int i = 0; i < cols; i += 4) {
                const float worldX           = medium_.origin.x + i * step;
                const weather::Column column = sky_.ColumnAt(worldX);

                for (int j = std::max(top, 0); j <= std::min(bottom, rows - 1); j++) {
                    const float density =
                        sky_.DensityAt({worldX, medium_.origin.y + j * step}, column);

                    most = std::max(most, density);
                    sum += density;
                    if (density > 0.01f) filled++;
                }
            }

            TraceLog(LOG_INFO,
                     "CLOUD: deck rows %d..%d of 0..%d (region y %.0f..%.0f, deck y %.0f..%.0f), "
                     "thickest %.3f, filled %d",
                     from, to, rows - 1, medium_.origin.y, medium_.origin.y + rows * step,
                     sky_.DeckTop(), sky_.DeckBottom(), most, filled);
        }

        if (to >= from) {
            pool::For(cols, [&](int i) {
                const float worldX = medium_.origin.x + i * step;

                // Once per column, which is what the column is for.
                const weather::Column column = sky_.ColumnAt(worldX);

                for (int j = from; j <= to; j++) {
                    const float density = sky_.DensityAt({worldX, medium_.origin.y + j * step}, column);
                    if (density <= 0.0f) continue;

                    const int cell = medium_.Index(i, j);

                    medium_.sigma[cell] += density * kCloudSigma;
                    medium_.albedo[cell] = kCloudAlbedo;
                }
            });
        }
    }

    // The canopy used to hold the sky back here, as a share per column, and it is
    // gone. See World::AddLight's neighbour in world.h for what it was and why it
    // could not stay.
    lightSettings_.sky.radiance = skyLight_;

    {
        PROFILE_ZONE("solve");

        lightField_.Solve(medium_, lightSettings_);
    }
}

bool World::BlocksLightAt(Vector2 world) const {
    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].light.opacity < 1.0f) continue;
        if (ValueAt(static_cast<Element>(e), world) > kElements[e].threshold) return true;
    }

    return false;
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

const World::Chunk::Silhouette &World::Occupancy(const Chunk &chunk, int minPrecedence, bool groundOnly) const {
    // A linear scan, over a dozen entries at most: there is one per rank the draw
    // asks about and one for the liquid clamp.
    for (const Chunk::Silhouette &kept : chunk.silhouettes) {
        if (kept.minPrecedence == minPrecedence && kept.groundOnly == groundOnly) return kept;
    }

    // Reserved up front so that a later entry cannot move an earlier one, since
    // the caller is holding a reference to it.
    if (chunk.silhouettes.capacity() == 0) chunk.silhouettes.reserve(kElementCount + 2);

    const Grid &any = chunk.fields[0];

    Grid margin(any.Origin(), any.Cols(), any.Rows(), spacing_);

    // The vertices that came out filled, gathered as the field is built.
    int firstVi = margin.Cols();
    int lastVi  = -1;
    int firstVj = margin.Rows();
    int lastVj  = -1;

    for (int i = 0; i < margin.Cols(); i++) {
        for (int j = 0; j < margin.Rows(); j++) {
            // Below anything a material could reach, so a chunk with none of
            // the asked-for materials reads as open space rather than as filled.
            float filled = -kUnboundedDepth;

            for (std::size_t e = 0; e < kElementCount; e++) {
                if (!kElements[e].rules.occupies || kElements[e].rules.precedence < minPrecedence) continue;
                if (groundOnly && !kElements[e].rules.blocksBodies) continue;

                filled = std::max(filled, chunk.fields[e].ValueAt(i, j) - kElements[e].threshold);
            }

            margin.SetValue(i, j, filled);

            if (filled > 0.0f) {
                firstVi = std::min(firstVi, i);
                lastVi  = std::max(lastVi, i);
                firstVj = std::min(firstVj, j);
                lastVj  = std::max(lastVj, j);
            }
        }
    }

    Chunk::Silhouette kept{.minPrecedence = minPrecedence, .groundOnly = groundOnly, .field = std::move(margin)};

    // A cell is drawn when any of its four corners is filled, so a filled vertex
    // puts the cell it starts and the one before it in play.
    if (lastVi >= 0) {
        kept.firstCol = std::max(firstVi - 1, 0);
        kept.lastCol  = std::min(lastVi, kept.field.Cols() - 2);
        kept.firstRow = std::max(firstVj - 1, 0);
        kept.lastRow  = std::min(lastVj, kept.field.Rows() - 2);
    }

    chunk.silhouettes.push_back(std::move(kept));

    return chunk.silhouettes.back();
}

const Grid &World::OccupancyField(const Chunk &chunk, int minPrecedence, bool groundOnly) const {
    return Occupancy(chunk, minPrecedence, groundOnly).field;
}

void World::ReadSod(Rectangle view) {
    PROFILE_ZONE("ReadSod");

    // Kept on the plant grid rather than on the lattice.
    //
    // The lattice is six pixels and a tuft is ten, so a profile at that spacing
    // gave every blade in a clump the height of whichever lattice column the
    // clump's own middle fell in — and where the ground steps, which it does by a
    // terrace riser at a time, that put half the blades of a tuft a riser above
    // the ground they were supposed to be standing on. At the plant grid each
    // blade reads its own column, so the worst it can be out by is the texel it
    // is drawn on.
    const float step = config::kFloraPixel;

    // A margin either side, because the tufts standing on this reach past the
    // column they grew in and the band is drawn a whole chunk at a time.
    constexpr float kMargin = 256.0f;

    // And because the grass spreading across turned earth measures how far it is
    // from grass that is already there, over this same band. A reach wider than
    // the margin would have a column on screen find a source only once the view
    // had scrolled far enough to include it, so how long it took the grass to
    // arrive would depend on where the player was standing.
    static_assert(sod::kCreepReach <= kMargin, "the creep must be measurable inside the band it is measured over");

    sodFirstColumn_ = static_cast<int>(std::floor((view.x - kMargin) / step));

    const int columns = static_cast<int>(std::ceil((view.width + 2.0f * kMargin) / step)) + 2;

    // Nothing at all when the ground is nowhere near the view, and this is the
    // difference between sixty frames a second and eight.
    //
    // Grass is only ever drawn on the surface, but the work of finding the
    // surface was done wherever the player happened to be looking. Each column
    // walks SurfaceOf, which steps the lattice through some seven hundred pixels
    // asking GroundValueAt — every occupying material — at each step; over the
    // seven hundred and sixty columns of a view that is a hundred thousand
    // lattice reads a frame. Near the ground they land in resident chunks and
    // cost nothing much. Fly a screen away and they land outside every resident
    // chunk, where a read is answered by generating that vertex from the noise,
    // contest and all — and the same pass costs a hundred milliseconds.
    //
    // The band is taken from terrain::Height rather than from the built surface,
    // because it has to be cheap and because it does not need to be exact: it is
    // only deciding whether the ground is within reach of the view at all, and
    // the margins below are far wider than anything digging can move it by.
    {
        constexpr float kProbeStep = 64.0f;

        // What SurfaceOf itself would search, either side of the ground.
        constexpr float kAbove = 160.0f;
        constexpr float kBelow = 704.0f;

        float highest = kUnboundedDepth;
        float lowest  = -kUnboundedDepth;

        for (float x = view.x - kMargin; x <= view.x + view.width + kMargin; x += kProbeStep) {
            const float top = terrain::Height(x, settings_);

            highest = std::min(highest, top);
            lowest  = std::max(lowest, top);
        }

        if (highest - kAbove > view.y + view.height || lowest + kBelow < view.y) {
            sodLook_.clear();
            sodCover_.clear();
            sodTop_.clear();
            sodPush_.clear();
            sodStanding_.clear();
            sodSown_.clear();
            sodSpread_.clear();

            return;
        }
    }

    sodLook_.resize(static_cast<std::size_t>(columns));
    sodCover_.assign(static_cast<std::size_t>(columns), 1.0f);
    sodTop_.assign(static_cast<std::size_t>(columns), 0.0f);
    sodPush_.assign(static_cast<std::size_t>(columns), 0.0f);

    // Negative for ground nobody has touched, which is every column until a brush
    // says otherwise.
    sodSown_.assign(static_cast<std::size_t>(columns), -1.0f);

    const weather::Sky::Season turn = sky_.Turn();
    const auto season               = static_cast<flora::Season>(turn.index % flora::kSeasonCount);

    // What the last band worked out, carried onto this one. See sodColumns_.
    {
        PROFILE_ZONE("sod shift");

        sodShifted_.assign(static_cast<std::size_t>(columns), SodColumn{});

        const int shift = sodFirstColumn_ - sodColumnsFirst_;

        for (int i = 0; i < columns; i++) {
            const int was = i + shift;

            if (was < 0 || was >= static_cast<int>(sodColumns_.size())) continue;

            sodShifted_[static_cast<std::size_t>(i)] = sodColumns_[static_cast<std::size_t>(was)];
        }

        sodColumns_.swap(sodShifted_);
        sodColumnsFirst_ = sodFirstColumn_;
    }

    // Every column the walk below will ask the skyline about, put into the record
    // before it starts — see World::skylineWritable_.
    {
        PROFILE_ZONE("sod skyline");

        const float lattice = static_cast<float>(spacing_);

        const int first = static_cast<int>(std::lround(static_cast<float>(sodFirstColumn_) * step / lattice)) - 1;
        const int last =
            static_cast<int>(std::lround(static_cast<float>(sodFirstColumn_ + columns) * step / lattice)) + 1;

        WarmSkyline(first, last);
    }

    skylineWritable_ = false;

    {
    PROFILE_ZONE("sod columns");

    pool::For(columns, [&](int i) {
        const float x = static_cast<float>(sodFirstColumn_ + i) * step;

        SodColumn &column = sodColumns_[static_cast<std::size_t>(i)];

        if (!column.known) {
            float top   = 0.0f;
            float cover = 0.0f;

            if (SurfaceOf(x, top)) {
                // Which grounds hold a ground cover at all.
                //
                // Soil and sand, and nothing else: dig past either and the floor
                // is rock and stays bare, and a snowfield grows nothing because
                // what is on top of it is snow.
                //
                // Sand is the new one and it is not "grass in the desert". This
                // number is how *established* a column is, which is what digging
                // and regrowth move; how thick the vegetation is at all is the
                // biome's, and in a desert sod::Cover says a quarter of the cells
                // and a good share of stones. So what a full cover on sand buys is
                // that the desert is dressed rather than swept — dry stalks and
                // grit standing between the scrub — and every column of it is
                // settled ground rather than ground that is waiting to recover.
                //
                // The band of green the terrain paints is unaffected and stays
                // soil's: it is the soil's own field read a second way, and under a
                // desert the soil is thirty pixels down with sand on top of it.
                const Vector2 under = {x, top + static_cast<float>(spacing_) * 0.5f};

                const std::optional<Element> ground = OccupantAt(under);

                if (ground == Element::Soil || ground == Element::Sand) cover = 1.0f;
            }

            // The climate with them, since it is a function of the column alone
            // and the noise behind it costs more than the rest of this loop.
            column = {.top = top, .cover = cover, .climate = terrain::ClimateAt(x, settings_), .known = true};
        }

        // Not cached with the rest: the season turns and the air dries out, so
        // the same column ramps differently from one day to the next.
        sodLook_[static_cast<std::size_t>(i)] = sod::LookAt(column.climate, season, turn.blend, sky_.HumidityAt(x));

        // As a share of the hardest this world can blow, so a field is at rest on a
        // still day and near its limit in a storm. Per column rather than per view,
        // because a gust is a wave crossing the world and the whole point of it is
        // that it arrives somewhere before it arrives everywhere.
        sodPush_[static_cast<std::size_t>(i)] = sky_.PushAt(x);

        sodTop_[static_cast<std::size_t>(i)]   = column.top;
        sodCover_[static_cast<std::size_t>(i)] = column.cover;
    },
    // Small blocks, because the cost of this band is wildly uneven: nearly every
    // column is already known and free, and the handful that have just scrolled
    // in cost a lattice walk each — and those are all together at one end.
    64, 4);
    }

    skylineWritable_ = true;

    ReadSown();

    // Then the tufts' own grid, which is a different one — see Blades::standing.
    sodFirstCell_ = static_cast<std::int64_t>(std::floor(static_cast<float>(sodFirstColumn_) * step / sod::kTuftSpan));

    const int cells = static_cast<int>(std::ceil(static_cast<float>(columns) * step / sod::kTuftSpan)) + 2;

    sodStanding_.assign(static_cast<std::size_t>(cells), 1.0f);

    // Cut grass is gone, and stays gone.
    //
    // It used to grow back on a timer, which made a field of grass an endless
    // supply of fibre to anybody willing to stand in it and keep swinging — and
    // the timer was not even the thing that let them: a cut tuft was left as
    // stubble, and stubble is still a tuft, so the same patch could be harvested
    // again on the very next swing without waiting for anything.
    //
    // Both are the same mistake, which is treating a cut as damage to a plant that
    // is still there. It is not: the plant has been taken. What puts grass back is
    // something the player does — a bonemeal, when there is one — and until then
    // an empty patch is a record of what was cleared, kept for good on the same
    // terms as a felled tree and a hand-laid block.
    for (const auto &[cell, at] : mown_) {
        const std::int64_t index = cell - sodFirstCell_;

        if (index >= 0 && index < cells) sodStanding_[static_cast<std::size_t>(index)] = 0.0f;
    }
}

void World::ForgetSod(float fromX, float toX) {
    if (sodColumns_.empty()) return;

    const float step = config::kFloraPixel;

    const int from = static_cast<int>(std::floor(fromX / step)) - sodColumnsFirst_;
    const int to   = static_cast<int>(std::ceil(toX / step)) - sodColumnsFirst_;

    const int first = std::max(from, 0);
    const int last  = std::min(to, static_cast<int>(sodColumns_.size()) - 1);

    for (int i = first; i <= last; i++) sodColumns_[static_cast<std::size_t>(i)].known = false;
}

void World::ReadSown() {
    PROFILE_ZONE("ReadSown");

    const auto span = sodSown_.size();
    if (span == 0) return;

    const int columns = static_cast<int>(span);
    const float step  = config::kFloraPixel;

    const float now = sky_.Time();

    const float take = std::max(sod::kTakeMinutes * 60.0f, 1e-3f);
    const float half = static_cast<float>(spacing_) / 2.0f;

    // Which columns of the band are standing on earth that was turned over, and
    // when it was.
    //
    // Walked from the record rather than looked up per column, the way the mowing
    // is: the record holds what the player has changed lately and nothing else, so
    // this costs what has been dug rather than what is on screen. A world nobody
    // has touched pays for an empty loop.
    for (auto it = sown_.begin(); it != sown_.end();) {
        // Long enough ago that the front has reached anywhere it was ever going
        // to. Dropping it is what keeps the record the size of the outstanding
        // work instead of the size of everything ever dug.
        if (now - it->second >= sod::kSettleSeconds) {
            it = sown_.erase(it);
            continue;
        }

        int i = 0;
        int j = 0;
        FromKey(it->first, i, j);

        const float vx = static_cast<float>(i) * static_cast<float>(spacing_);
        const float vy = static_cast<float>(j) * static_cast<float>(spacing_);

        // The plant-grid columns this lattice vertex stands under. A vertex owns
        // half a lattice step either side of itself, which at six pixels against
        // two is three of them.
        const int from = static_cast<int>(std::ceil((vx - half) / step)) - sodFirstColumn_;
        const int to   = static_cast<int>(std::floor((vx + half) / step)) - sodFirstColumn_;

        for (int c = std::max(from, 0); c <= std::min(to, columns - 1); c++) {
            const float top = sodTop_[static_cast<std::size_t>(c)];

            // Only where this vertex is the ground the grass would be growing on.
            //
            // Without the test a tunnel dug a long way under a meadow would strip
            // the meadow of its grass, because the column it was dug in is the
            // column the meadow stands in. The band runs from a lattice step above
            // the surface — a scoop taken out of a hillside leaves the vertex it
            // removed just over the floor it exposed — down to the depth a sod
            // reaches, which is as far as any of this can matter.
            if (vy < top - static_cast<float>(spacing_) || vy > top + sod::kSodDepth) continue;

            // The latest disturbance wins, so ground turned over twice is as new as
            // the second time.
            sodSown_[static_cast<std::size_t>(c)] = std::max(sodSown_[static_cast<std::size_t>(c)], it->second);
        }

        ++it;
    }

    // How far each column is from grass that is already established, in world
    // pixels, as two sweeps across the band.
    //
    // A column that is waiting for nothing and does carry grass is a source and
    // sits at zero; everything else takes its distance from the nearer of its two
    // sides. Bare rock and open sky are neither sources nor walls — the front
    // crosses them and arrives on the far side, which is the answer that stays
    // sane when a player bridges a gap and then fills it in.
    sodSpread_.assign(span, sod::kCreepReach);

    const auto established = [this](std::size_t c) { return sodSown_[c] < 0.0f && sodCover_[c] > 0.0f; };

    float reach = sod::kCreepReach;

    for (std::size_t c = 0; c < span; c++) {
        reach         = established(c) ? 0.0f : std::min(reach + step, sod::kCreepReach);
        sodSpread_[c] = reach;
    }

    reach = sod::kCreepReach;

    for (std::size_t c = span; c-- > 0;) {
        reach         = established(c) ? 0.0f : std::min(reach + step, sod::kCreepReach);
        sodSpread_[c] = std::min(sodSpread_[c], reach);
    }

    // And what that comes to on the ground: the front arrives after the time it
    // takes to travel there, and the grass then takes hold over kTakeMinutes.
    //
    // Held against the cover rather than replacing it, so nothing here can grow
    // grass where there was none to grow — a column of bare rock the player turned
    // over is bare rock still, and this only ever takes away.
    const float creep = std::max(sod::kCreepPerMinute, 1e-3f);

    for (std::size_t c = 0; c < span; c++) {
        if (sodSown_[c] < 0.0f) continue;

        const float arrives = sodSpread_[c] / creep * 60.0f;
        const float taken   = std::clamp((now - sodSown_[c] - arrives) / take, 0.0f, 1.0f);

        sodCover_[c] = std::min(sodCover_[c], taken);
    }
}

bool World::FootingUnder(Vector2 world, float reach, float &outTop) const {
    const float step = static_cast<float>(spacing_);

    // Started on the lattice, for the reason SurfaceOf gives at length: a walk
    // stepping the lattice from an off-lattice start snaps two consecutive steps
    // onto the same vertex and then skips one, and lands anywhere within half a
    // step of where the ground actually is.
    const float from = std::floor(world.y / step) * step;

    // The crossing, turned into the row the ground is actually *drawn* at — the
    // same correction ReadGround makes for the trees. A square is filled when its
    // centre is inside, so the drawn surface sits up to most of a texel below the
    // contour, and anything seated on the contour instead floats by that much.
    // Onto *that material's* grid, since materials are no longer all drawn at one
    // size — see ElementPaint::texel. Read half a step inside the surface, which is
    // where the material whose top row of squares this is actually sits; on the
    // crossing itself the answer is whatever the contour rounded to.
    //
    // Without this a plank floor, drawn five times finer than the hillside beside
    // it, would still be stood on as though it were hillside, and everything put
    // down on it would sit up to a terrain texel above or below the boards.
    const auto seated = [&](float crossing) {
        const std::optional<Element> under = OccupantAt({world.x, crossing + step * 0.5f});

        return marching_squares::DrawnTop(crossing, under.has_value() ? Def(*under).paint.texel : config::kPixelSize);
    };

    // Inside something already. The answer is the top of whatever the cursor is
    // in, found by walking up: a cursor in the middle of a ramp means the ramp,
    // and a walk downwards from there would pass out through its underside and
    // land on whatever the ramp was built over.
    if (GroundValueAt({world.x, from}) > 0.0f) {
        for (float y = from; y >= from - reach; y -= step) {
            const float above = GroundValueAt({world.x, y - step});
            if (above > 0.0f) continue;

            const float here = GroundValueAt({world.x, y});
            const float t    = -above / std::max(here - above, 1e-6f);

            outTop = seated(y - step + t * step);
            return true;
        }

        return false;
    }

    // Otherwise the first solid met falling from the cursor.
    float previous = GroundValueAt({world.x, from});

    for (float y = from + step; y <= from + reach; y += step) {
        const float here = GroundValueAt({world.x, y});

        if (previous <= 0.0f && here > 0.0f) {
            const float t = -previous / std::max(here - previous, 1e-6f);

            outTop = seated(y - step + t * step);
            return true;
        }

        previous = here;
    }

    return false;
}

bool World::SurfaceOf(float worldX, float &outTop) const {
    const float step = static_cast<float>(spacing_);

    // Started above the surface the noise describes rather than at a fixed
    // height, and carried well past it: what is wanted is the first solid a
    // falling body would meet, and the player can have built above the skyline or
    // dug a long way below it.
    constexpr float kAbove = 96.0f;
    constexpr float kBelow = 640.0f;

    const int column = static_cast<int>(std::lround(worldX / step));

    // Started on the lattice, and this is not tidiness.
    //
    // Skyline is a scan that began at an arbitrary height, so its answer is *not*
    // on the lattice — its own declaration says so and warns what happens next.
    // GroundValueAt reads the vertex nearest to what it is asked about, so a walk
    // stepping the lattice from an off-lattice start snaps two consecutive steps
    // onto the same vertex and then skips one, and the crossing it reports lands
    // anywhere within half a step of where the ground actually is. Half a step at
    // the edge of the ground is the difference between grass on the soil and grass
    // hanging over it.
    const float from = std::floor((Skyline(column) - kAbove) / step) * step;

    float y          = from;
    const float stop = from + kAbove + kBelow;

    float previous = GroundValueAt({worldX, y});

    for (y += step; y <= stop; y += step) {
        const float here = GroundValueAt({worldX, y});

        if (previous <= 0.0f && here > 0.0f) {
            // Interpolated onto the contour, so the tufts stand on the line the
            // ground was drawn along instead of on the lattice it was sampled on.
            const float t = -previous / std::max(here - previous, 1e-6f);

            outTop = y - step + t * step;
            return true;
        }

        previous = here;
    }

    return false;
}

sod::Blades World::Grass() const {
    return {.top         = sodTop_.data(),
            .cover       = sodCover_.data(),
            .push        = sodPush_.data(),
            .look        = sodLook_.data(),
            .count       = static_cast<int>(sodLook_.size()),
            .firstColumn = sodFirstColumn_,
            .spacing     = config::kFloraPixel,
            .standing    = sodStanding_.data(),
            .cells       = static_cast<int>(sodStanding_.size()),
            .firstCell   = sodFirstCell_};
}

int World::MowGrass(Rectangle hitbox, float now) {
    // Bounded by what one swing can reach. A hitbox is a body's arm, not a
    // region, so a handful of cells is the whole of it.
    constexpr int kRoom = 16;

    std::int64_t cut[kRoom];
    bool ripe[kRoom];

    const int taken = sod::Cut(Grass(), hitbox, settings_.seed, cut, ripe, kRoom);

    int paid = 0;

    for (int i = 0; i < taken; i++) {
        mown_[cut[i]] = now;

        // Cleared either way; paid for only where the grass had finished growing.
        if (ripe[i]) paid++;
    }

    return paid;
}

float World::CoverDepth(float worldX, float surfaceY) const {
    // Bounded by the deepest a cover may ever be, so a column of bare rock costs
    // one lookup and stops rather than walking to the bottom of the world.
    const int steps = static_cast<int>(std::ceil(kCoverCeiling / static_cast<float>(spacing_))) + 1;

    const float step = static_cast<float>(spacing_);

    float depth = 0.0f;

    for (int k = 0; k < steps; k++) {
        const std::optional<Element> here = OccupantAt({worldX, surfaceY + static_cast<float>(k) * step});
        if (!here.has_value()) continue;

        // A cover and nothing else. Rock, ore and anything the player stood there
        // are all "not the layer over the rock", which is the whole of the test.
        if (Def(*here).spawn.generator != Generator::Cover) break;

        depth = static_cast<float>(k + 1) * step;
    }

    return depth;
}

float World::GroundValueAt(Vector2 world) const {
    int cx = 0;
    int cy = 0;
    ToChunk(world, cx, cy);

    const Chunk *chunk = Find(cx, cy);

    float filled = -kUnboundedDepth;

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (!def.rules.occupies || !def.rules.blocksBodies) continue;

        // Interpolated between the four samples around the position, which is the
        // same reading the rasteriser takes.
        //
        // Not the nearest vertex, and the difference is the whole point. Anything
        // that has to agree with where the ground was *drawn* has to interpolate
        // the field the way the drawing did; snapping to a vertex instead answers
        // with the height of somewhere up to half a lattice step away, and the
        // surface is terraced into risers four times that tall. What that put on
        // screen was tufts of grass hanging in the air beside every step.
        const float value = (chunk != nullptr) ? marching_squares::SampleAt(chunk->fields[e], world)
                                               : ValueAt(static_cast<Element>(e), world);

        filled = std::max(filled, value - def.threshold);
    }

    return filled;
}

Grid World::SodField(const Chunk &chunk) const {
    const Grid &soil = chunk.fields[ElementIndex(Element::Soil)];

    // The silhouette of the whole ground, which is what says where the sky stops.
    // Read from the union rather than from the soil alone because what buries a
    // sod is anything at all standing on it — snow the world laid down, or rock
    // the player did.
    const Grid &filled = OccupancyField(chunk);

    Grid sod(soil.Origin(), soil.Cols(), soil.Rows(), spacing_);

    const float threshold = Def(Element::Soil).threshold;
    const float step      = static_cast<float>(spacing_);

    for (int i = 0; i < sod.Cols(); i++) {
        // The column this vertex belongs to, in the numbering ReadSod filled —
        // which is the plant grid and not this one.
        const int column = static_cast<int>(std::floor(sod.PointAt(i, 0).x / config::kFloraPixel)) - sodFirstColumn_;

        const float cover =
            sodCover_.empty()
                ? 1.0f
                : sodCover_[static_cast<std::size_t>(std::clamp(column, 0, static_cast<int>(sodCover_.size()) - 1))];

        // How far under the sky this column already is where the chunk begins.
        //
        // Without it every chunk would start its own reckoning at its top row, and
        // a chunk that begins underground would read its first row as the surface
        // and lay a stripe of turf along its own ceiling. Only a few vertices are
        // needed: what is being measured is a depth that stops mattering past the
        // thickness of a sod.
        const Vector2 top = sod.PointAt(i, 0);

        float prevValue = -1.0f;
        float prevDepth = -step;

        for (int seed = kSodSeedRows; seed >= 1; seed--) {
            const float y     = top.y - static_cast<float>(seed) * step;
            const float value = GroundValueAt({top.x, y});

            // The topmost seed row is where the walk has to start from something,
            // and buried is the safe answer: a chunk deep in the rock is deep in
            // the rock, and reading it as open sky is the fault this exists to
            // prevent.
            const float depth = (value > 0.0f) ? ((seed == kSodSeedRows) ? kSodBuried : prevDepth + step) : -step;

            prevValue = value;
            prevDepth = depth;
        }

        for (int j = 0; j < sod.Rows(); j++) {
            const float here = filled.ValueAt(i, j);

            // Depth below the topmost solid in this column, in pixels, walked
            // rather than read off the field.
            //
            // The field cannot answer it. A material's value is its distance to
            // its own edges, so where the soil meets the rock under it the union
            // dips — the soil is near its own floor and the rock has been cut away
            // to make room — and read as a depth that dip says "surface" thirty
            // pixels underground. What it drew was a stripe of turf buried in the
            // rock, level, following the bottom of the soil.
            //
            // Walking the column has no such reading to make. It also gives the
            // sky-facing test away for nothing: a cave roof and the belly of an
            // overhang are deep under the topmost solid of their own column by
            // construction, so neither can be turfed and there is no normal to
            // test.
            float depth;

            if (here <= 0.0f) {
                depth = -step;
            } else if (prevValue <= 0.0f) {
                // Just entered the ground. The crossing is interpolated so the
                // reckoning starts on the contour rather than on the lattice,
                // which is what keeps the band an even thickness on a slope.
                const float t = -prevValue / std::max(here - prevValue, 1e-6f);

                depth = (1.0f - t) * step;
            } else {
                depth = prevDepth + step;
            }

            prevValue = here;
            prevDepth = depth;

            // Inside the soil, and the soil alone. The exclusion pass has already
            // cut sand and snow out of this field, so neither of them can grow
            // grass and neither has to be asked about.
            float value = soil.ValueAt(i, j) - threshold;

            // And within a sod's depth of the sky.
            value = std::min(value, (sod::kSodDepth - depth) / terrain::kDensitySpan);

            // And as much of it as has grown back. Subtracted rather than
            // multiplied: the field is a distance, and a smaller distance is a
            // shallower sod, while a smaller field is a sod in the wrong place.
            value -= (1.0f - cover) * sod::kSodDepth / terrain::kDensitySpan;

            sod.SetValue(i, j, value);
        }
    }

    return sod;
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

const World::Painted *World::PaintedFor(int cx, int cy) const {
    const auto found = paintedOf_.find(Key(cx, cy));
    if (found == paintedOf_.end()) return nullptr;

    const Painted &slot = painted_[static_cast<std::size_t>(found->second)];

    return (slot.holds && slot.cx == cx && slot.cy == cy) ? &slot : nullptr;
}

void World::DropPainted(int cx, int cy) {
    const auto found = paintedOf_.find(Key(cx, cy));
    if (found == paintedOf_.end()) return;

    // The slot is kept and only disowned, since a texture is a GPU resource and
    // making another costs far more than drawing into this one again.
    painted_[static_cast<std::size_t>(found->second)].holds = false;

    paintedOf_.erase(found);
}

void World::UnloadPainted() {
    for (Painted &slot : painted_) {
        if (slot.texture.id != 0) UnloadRenderTexture(slot.texture);
    }

    painted_.clear();
    paintedOf_.clear();
}

void World::PaintBackdrop(Rectangle own) const {
    const float pixel = config::kPixelSize;

    // Squares anchored to the world rather than to the chunk, exactly as
    // marching_squares::DrawPainted anchors them: the backdrop and the ground in
    // front of it have to fall on one grid, or the grain behind a cut is half a
    // texel out from the grain either side of it and the join is what the eye
    // finds first.
    const int m0 = static_cast<int>(std::floor(own.x / pixel));
    const int m1 = static_cast<int>(std::ceil((own.x + own.width) / pixel));
    const int n0 = static_cast<int>(std::floor(own.y / pixel));
    const int n1 = static_cast<int>(std::ceil((own.y + own.height) / pixel));

    // No face, and so no form — which is the whole of what makes this read as the
    // inside of something. A depth past soil::kRimReach leaves the rim term at
    // nothing and the texture alone, and the normal is then never consulted. See
    // soil::Shading.
    constexpr float kDeep = 1e9f;

    for (int m = m0; m <= m1; m++) {
        // The middle of the square, since that is where DrawPainted samples the
        // ground's own fields and the two have to be answering about one place.
        const float x = (static_cast<float>(m) + 0.5f) * pixel;

        // And the middle is also what says whose square this is — see the note on
        // the margin beside this function's declaration. The square itself may hang
        // over the border by nearly its own width, which is what the margin is for.
        if (x < own.x || x >= own.x + own.width) continue;

        // Once for the column, which is what a column is for: the surface costs
        // eight octaves and the climate two fields more, and every square below
        // this one wants the same two answers.
        const float ground             = terrain::Height(x, settings_);
        const terrain::Climate climate = terrain::ClimateAt(x, settings_);

        // What each cover is worth here, in pixels of slab. Asked of the very
        // function the generator asks — a second rule for how deep the soil goes
        // would be a backdrop that stops agreeing with the ground the first time
        // either is touched.
        std::array<float, kElementCount> slab{};

        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementSpawn &spawn = kElements[e].spawn;
            if (spawn.generator != Generator::Cover) continue;

            slab[e] = CoverThickness(spawn, x, ground, climate.temperature, climate.humidity);
        }

        const float top = ground + kBehindSink;

        for (int n = n0; n <= n1; n++) {
            const float y = (static_cast<float>(n) + 0.5f) * pixel;

            if (y < own.y || y >= own.y + own.height) continue;

            // Above the land there is sky, and the sky is drawn behind this whole
            // layer already. Nothing here may cover it.
            if (y <= top) continue;

            // Which material, by the rule the generator uses: the cover with the
            // strongest claim that still reaches this deep, and the rock under all
            // of them. The depth is measured from the true surface rather than from
            // where the painting starts, so a slab written as four pixels of soil is
            // four pixels of soil here too.
            const float depth = y - ground;

            std::size_t what = ElementIndex(Element::Rock);
            int strongest    = -1;

            for (std::size_t e = 0; e < kElementCount; e++) {
                if (slab[e] <= depth) continue;

                const int precedence = kElements[e].rules.precedence;
                if (precedence <= strongest) continue;

                strongest = precedence;
                what      = e;
            }

            const Vector2 at = {static_cast<float>(m) * pixel, static_cast<float>(n) * pixel};

            DrawRectangleV(at, {pixel, pixel}, behind_[what]({at, kDeep, {0.0f, -1.0f}}));
        }
    }
}

void World::PaintChunk(Painted &slot, int cx, int cy) {
    const float span = ChunkSpan();

    // Drawn through a camera that puts the chunk's own corner, less the margin,
    // at the texture's origin. One texel per world unit, so nothing is scaled and
    // the squares land on whole texels exactly as they land on whole pixels.
    Camera2D frame{};
    frame.offset = {0.0f, 0.0f};
    frame.target = {static_cast<float>(cx) * span - kPaintedMargin, static_cast<float>(cy) * span - kPaintedMargin};
    frame.zoom   = 1.0f;

    // The whole chunk and its margin, so nothing is clipped away that a
    // neighbouring view would have wanted.
    const Rectangle covered = {frame.target.x, frame.target.y, span + 2.0f * kPaintedMargin,
                               span + 2.0f * kPaintedMargin};

    BeginTextureMode(slot.texture);

    // Clear rather than blank: what is not ground has to let the sky through when
    // this is composited, and every colour the ground is drawn in is opaque, so
    // the alpha in here is exactly "there is ground at this texel".
    ClearBackground(BLANK);

    BeginMode2D(frame);

    // The country behind the ground, under everything — including under a wall,
    // which is a thing the player put up in front of it. Drawn whether or not this
    // chunk has been generated: what stands behind the world is a property of the
    // world's own noise, and a chunk that has not arrived yet has no say in it.
    //
    // The chunk's own span and not `covered`: the margin belongs to the neighbour
    // that owns the squares in it.
    PaintBackdrop({static_cast<float>(cx) * span, static_cast<float>(cy) * span, span, span});

    const Chunk *chunk = Find(cx, cy);

    if (chunk != nullptr) {
        // The walls first, so everything else covers them.
        //
        // Into the same texture rather than a layer of its own, and that is worth
        // saying: a wall is behind the ground and behind the character, and the
        // whole of the terrain is already drawn before the character is. So being
        // first in here is being behind both, for no second texture, no second
        // blit, and no change to the order of the frame.
        //
        // They take no part in the exclusion order below — a wall contests no
        // vertex, so it is in nobody's silhouette and has none of its own.
        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementDef &def = kElements[e];
            if (!def.rules.background) continue;

            marching_squares::DrawPainted(chunk->fields[e], def.threshold, paint_[e], Outline(def.contour),
                                          def.paint.texel, covered);
        }

        // Painted from the back, each material drawn as itself together with
        // everything that outranks it — the order DrawTerrain used to walk across
        // every chunk at once. Chunks tile without overlapping, so drawing one
        // whole at a time puts down the same picture.
        for (const Element element : exclusionOrder_) {
            const ElementDef &def = Def(element);

            // From its own field, for one of the two reasons DrawnUnioned gives:
            // it is not part of the ground, or it is a vein and the union would
            // scatter it through every material that outranks it.
            if (!DrawnUnioned(def)) {
                const std::size_t index = ElementIndex(element);

                marching_squares::DrawPainted(chunk->fields[index], def.threshold, paint_[index],
                                              Outline(def.contour), def.paint.texel, covered);
                continue;
            }

            const Chunk::Silhouette &silhouette = Occupancy(*chunk, def.rules.precedence, true);
            if (silhouette.Empty()) continue;

            marching_squares::DrawPainted(silhouette.field, 0.0f, paint_[ElementIndex(element)],
                                          Outline(def.contour), def.paint.texel, covered,
                                          {.firstCol = silhouette.firstCol,
                                           .lastCol  = silhouette.lastCol,
                                           .firstRow = silhouette.firstRow,
                                           .lastRow  = silhouette.lastRow});
        }
    }

    EndMode2D();
    EndTextureMode();

    slot.cx    = cx;
    slot.cy    = cy;
    slot.holds = true;
}

void World::PaintChunks(Rectangle view) {
    PROFILE_ZONE("PaintChunks");

    if (!config::kPixelArt) return;

    int minCx = 0;
    int minCy = 0;
    int maxCx = 0;
    int maxCy = 0;
    ChunkRange(view, minCx, minCy, maxCx, maxCy);

    paintedAge_++;

    // Anything already painted is claimed first, so that the eviction below never
    // takes a slot this same view is about to draw from.
    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            const auto found = paintedOf_.find(Key(cx, cy));
            if (found == paintedOf_.end()) continue;

            painted_[static_cast<std::size_t>(found->second)].age = paintedAge_;
        }
    }

    const int side = static_cast<int>(ChunkSpan()) + 2 * kPaintedMargin;

    for (int cx = minCx; cx <= maxCx; cx++) {
        for (int cy = minCy; cy <= maxCy; cy++) {
            if (PaintedFor(cx, cy) != nullptr) continue;
            if (Find(cx, cy) == nullptr) continue;

            int chosen = -1;

            // A slot nothing is using, or a new one while there is room for it.
            for (std::size_t s = 0; s < painted_.size() && chosen < 0; s++) {
                if (!painted_[s].holds) chosen = static_cast<int>(s);
            }

            if (chosen < 0 && static_cast<int>(painted_.size()) < kPaintedSlots) {
                Painted slot;
                slot.texture = LoadRenderTexture(side, side);

                // Point sampling, because the whole equivalence rests on a screen
                // pixel taking one texel rather than a blend of four.
                SetTextureFilter(slot.texture.texture, TEXTURE_FILTER_POINT);

                painted_.push_back(slot);
                chosen = static_cast<int>(painted_.size()) - 1;
            }

            // Otherwise the one least recently drawn from, which the claim above
            // has kept out of the current view.
            if (chosen < 0) {
                long long oldest = paintedAge_;

                for (std::size_t s = 0; s < painted_.size(); s++) {
                    if (painted_[s].age >= oldest) continue;

                    oldest = painted_[s].age;
                    chosen = static_cast<int>(s);
                }
            }

            // Every slot is in this view. Nothing is evicted; the chunks that did
            // not fit are drawn straight to the screen by DrawTerrain instead.
            if (chosen < 0) continue;

            Painted &slot = painted_[static_cast<std::size_t>(chosen)];

            if (slot.holds) paintedOf_.erase(Key(slot.cx, slot.cy));

            PaintChunk(slot, cx, cy);

            slot.age = paintedAge_;

            paintedOf_[Key(cx, cy)] = chosen;
        }
    }
}

void World::DrawUnderground(Rectangle view) const {
    // The deep rock as the backdrop paints it, with the texture taken off.
    //
    // The middle of the ramp, which is where a texel with no face and no drift
    // lands — see soil::kBase. So this is the average of the wall the chunk
    // textures are about to draw over it, and the seam between the two is a change
    // of grain rather than a change of colour.
    const Color deep = behind_[ElementIndex(Element::Rock)].ramp.tone[kElementRamp / 2];

    const float step = static_cast<float>(spacing_);

    const int first = static_cast<int>(std::floor(view.x / step)) - 1;
    const int last  = static_cast<int>(std::ceil((view.x + view.width) / step)) + 1;

    const float floorY = view.y + view.height;

    // Opaque, and that is the whole of what it has to be: the alpha in this layer
    // is how much of the sky a pixel covers, and below the land the answer is all
    // of it. See LitLayer::Capture.
    //
    // Carried along the row, since the right-hand end of one column is the
    // left-hand end of the next and the surface is dear enough to be worth asking
    // about once.
    float left = terrain::Height(first * step, settings_);

    for (int column = first; column <= last; column++) {
        const float x0    = column * step;
        const float right = terrain::Height(x0 + step, settings_);

        // The lower of the two ends, since the contour between them can stand as
        // high as either and this must never be seen above the ground it is behind.
        const float surface = std::max(left, right) + kBehindSink;

        left = right;

        const float from = std::max(surface, view.y);
        if (from >= floorY) continue;

        DrawRectangleRec({x0, from, step, floorY - from}, deep);
    }
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
    // The ground itself, from the picture PaintChunks made of each chunk.
    //
    // A blit rather than a rasterisation, and the same one: the texture holds a
    // texel per world unit, so a screen pixel takes the texel whose square it
    // falls in — which is the square the rectangle would have covered it with.
    // See World::Painted.
    {
        PROFILE_ZONE("blit ground");

        const float span = ChunkSpan();
        const float side = span + 2.0f * kPaintedMargin;

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                const Painted *slot = PaintedFor(cx, cy);
                if (slot == nullptr) continue;

                const Vector2 at = {static_cast<float>(cx) * span - kPaintedMargin,
                                    static_cast<float>(cy) * span - kPaintedMargin};

                // A render target comes out upside down, hence the negative
                // height on the source.
                DrawTextureRec(slot->texture.texture, {0.0f, 0.0f, side, -side}, at, WHITE);
            }
        }
    }

    // Anything that neither flows nor claims its space: a wall, which contests no
    // vertex and stands behind whatever does.
    //
    // **First of the fallbacks, and only where there is no picture.** Both halves of
    // that were wrong and together they were one bug with a plain symptom: a wall put
    // up against a hillside appeared *in front* of it, as though it had replaced the
    // blocks it was meant to stand behind.
    //
    // This loop used to be the last thing DrawTerrain did — after the ground, after
    // the grass — and it ran over every chunk whether or not the chunk had a picture.
    // So a painted chunk, which already holds its walls laid down behind everything
    // inside PaintChunk, had a second copy of them painted over the finished ground.
    // The copy underneath was correct and invisible; the one on top was what the
    // player saw.
    //
    // A wall may never cover what is in front of it. The most it may do is show
    // through the gaps, which is what being behind means.
    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        if (def.rules.flows || def.rules.occupies) continue;

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                if (config::kPixelArt && PaintedFor(cx, cy) != nullptr) continue;

                const Chunk *chunk = Find(cx, cy);
                if (chunk == nullptr) continue;

                if (config::kPixelArt) {
                    marching_squares::DrawPainted(chunk->fields[e], def.threshold, paint_[e], Outline(def.contour),
                                                  def.paint.texel, view);
                } else {
                    marching_squares::DrawFilled(chunk->fields[e], def.threshold, Body(def));
                    if (config::kDrawContours) {
                        marching_squares::DrawContour(chunk->fields[e], def.threshold, def.contour);
                    }
                }
            }
        }
    }

    // And whatever had no picture made of it — a chunk that arrived after
    // PaintChunks ran, or one that found no slot free — drawn the long way, so
    // that the cache is a saving and never a condition for the ground being
    // there at all.
    for (const Element element : exclusionOrder_) {
        const ElementDef &def = Def(element);

        // Anything that is not part of the ground, and any vein, is drawn from its
        // own field alone rather than unioned into the silhouette of what is under
        // it. DrawnUnioned holds both reasons; the first of them is that
        //
        // the union is stored as the strongest of several fields, and
        // interpolating that is not the same as interpolating each and taking
        // the strongest. Between two smooth fields the difference is nothing,
        // because exclusion already makes them cross their thresholds on the
        // same line. Against a hand-placed material, which is one inside its
        // brush and zero outside, the union's crossing lands a fraction of a
        // cell further out than the material's own, and what shows in the gap
        // is a ring of whatever was beneath.
        if (!DrawnUnioned(def)) {
            const std::size_t index = ElementIndex(element);

            for (int cx = minCx; cx <= maxCx; cx++) {
                for (int cy = minCy; cy <= maxCy; cy++) {
                    if (config::kPixelArt && PaintedFor(cx, cy) != nullptr) continue;

                    const Chunk *chunk = Find(cx, cy);
                    if (chunk == nullptr) continue;

                    if (config::kPixelArt) {
                        marching_squares::DrawPainted(chunk->fields[index], def.threshold, paint_[index],
                                                      Outline(def.contour), def.paint.texel, view);
                    } else {
                        marching_squares::DrawFilled(chunk->fields[index], def.threshold, Body(def));
                        if (config::kDrawContours) {
                            marching_squares::DrawContour(chunk->fields[index], def.threshold, def.contour);
                        }
                    }
                }
            }

            continue;
        }

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                if (config::kPixelArt && PaintedFor(cx, cy) != nullptr) continue;

                const Chunk *chunk = Find(cx, cy);
                if (chunk == nullptr) continue;

                const Chunk::Silhouette &silhouette = [&]() -> const Chunk::Silhouette & {
                    PROFILE_ZONE("occupancy");

                    return Occupancy(*chunk, def.rules.precedence, true);
                }();

                // Nothing of this rank or above stands in this chunk, which is
                // the usual answer: a hillside holds no ore and a seam of ore
                // holds no snow. Walking the chunk to find that out was ten
                // thousand samples for a material that draws nothing.
                if (silhouette.Empty()) continue;

                const Grid &field = silhouette.field;

                if (config::kPixelArt) {
                    PROFILE_ZONE("paint ground");

                    marching_squares::DrawPainted(field, 0.0f, paint_[ElementIndex(element)], Outline(def.contour),
                                                  def.paint.texel, view,
                                                  {.firstCol = silhouette.firstCol,
                                                   .lastCol  = silhouette.lastCol,
                                                   .firstRow = silhouette.firstRow,
                                                   .lastRow  = silhouette.lastRow});
                } else {
                    marching_squares::DrawFilled(field, 0.0f, Body(def));
                    if (config::kDrawContours) marching_squares::DrawContour(field, 0.0f, def.contour);
                }
            }
        }
    }

    // Then the grass, over the finished ground.
    //
    // Last of the solid draws and not a material in the list above, because it is
    // not a material: it is the soil's own field read a second way. Rasterised by
    // the same routine at the same texel size, so it lands on the ground's own
    // staircase by construction rather than by being lined up with it.
    if (config::kPixelArt && !sodLook_.empty()) {
        const SodPainter grass{.looks       = sodLook_.data(),
                               .count       = static_cast<int>(sodLook_.size()),
                               .firstColumn = sodFirstColumn_,
                               .spacing     = config::kFloraPixel,
                               .seed        = settings_.seed + kSodSeed};

        // Built across the cores first and drawn after, because the field a chunk
        // of grass is drawn from is derived — the soil read against the ground's
        // whole silhouette — and deriving it costs more than drawing it. The draw
        // itself cannot be shared: it is raylib, and raylib is one thread.
        {
            PROFILE_ZONE("sod field");

            sodFields_.clear();

            for (int cx = minCx; cx <= maxCx; cx++) {
                for (int cy = minCy; cy <= maxCy; cy++) {
                    const Chunk *chunk = Find(cx, cy);
                    if (chunk == nullptr) continue;

                    sodFields_.push_back(chunk);
                }
            }

            sodGrids_.clear();
            sodGrids_.reserve(sodFields_.size());

            for (std::size_t k = 0; k < sodFields_.size(); k++) {
                sodGrids_.push_back(Grid({0.0f, 0.0f}, 1, 1, spacing_));
            }

            pool::For(
                static_cast<int>(sodFields_.size()),
                [&](int k) { sodGrids_[static_cast<std::size_t>(k)] = SodField(*sodFields_[static_cast<std::size_t>(k)]); },
                2, 1);
        }

        {
            PROFILE_ZONE("paint sod");

            for (const Grid &field : sodGrids_) {
                marching_squares::DrawPainted(field, 0.0f, grass, BLANK, config::kPixelSize, view);
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
        const Color body    = Body(style);
        const Color surface = {style.contour.r, style.contour.g, style.contour.b, 255};

        for (int cx = minCx; cx <= maxCx; cx++) {
            for (int cy = minCy; cy <= maxCy; cy++) {
                if (Find(cx, cy) == nullptr) continue;

                const Grid field = LiquidRenderField(cx, cy, element);

                if (config::kPixelArt) {
                    marching_squares::DrawPainted(field, style.threshold, paint_[e], Outline(surface),
                                                  style.paint.texel);
                } else {
                    marching_squares::DrawFilled(field, style.threshold, body);
                    if (config::kDrawContours) marching_squares::DrawContour(field, style.threshold, surface);
                }
            }
        }
    }
}

weather::Ground World::GroundUnder(Rectangle view, float margin) const {
    const float step = static_cast<float>(spacing_);

    const int firstColumn = static_cast<int>(std::floor((view.x - margin) / step));
    const int columns     = static_cast<int>(std::ceil((view.width + 2.0f * margin) / step)) + 2;

    // Built here, in the draw, rather than beside the light solve, because it has to
    // see this frame's edits: the brush runs before the frame is drawn, and a block
    // placed on this frame should stop this frame's rain.
    SurfaceProfile(firstColumn, columns, surface_);

    return {.top     = surface_.data(),
            .count   = static_cast<int>(surface_.size()),
            .originX = static_cast<float>(firstColumn) * step,
            .spacing = step};
}

void World::DrawRain(Rectangle view) const {
    // The same stretch the sky will draw over, asked for rather than guessed at: the
    // rain leans, so drops landing inside the view start well outside it.
    sky_.DrawRain(view, GroundUnder(view, sky_.RainReach()));
}

void World::DrawStars(Rectangle view) const {
    // No lean to allow for — a star is drawn where it is — so the view itself is the
    // whole of what has to be covered.
    weather::Ground ground = GroundUnder(view, 0.0f);

    // With the ranges folded into it, which is the one thing being drawn after the
    // light costs. A star is no longer hidden by something simply because that
    // something was drawn later, so everything that should hide one has to be asked
    // — the sky already asks the land and the cloud, and the horizon is the third.
    //
    // Written into the profile rather than tested separately because the profile is
    // exactly the question "how high is the world over this column", and a mountain
    // on the skyline is the world over that column. Nothing in weather.h has to
    // learn that ranges exist.
    if (vista_.Visible(view)) {
        for (int column = 0; column < ground.count; column++) {
            const float x = ground.originX + static_cast<float>(column) * ground.spacing;

            surface_[static_cast<std::size_t>(column)] = std::min(surface_[static_cast<std::size_t>(column)],
                                                                  vista_.CrestAt(x, view));
        }
    }

    sky_.DrawStars(view, ground);
}

void World::DrawMist(Rectangle view) const {
    // A cell either side, since the bank is drawn on its own world-anchored grid and
    // the cell straddling the edge of the view has to be prepared like any other.
    sky_.DrawMist(view, GroundUnder(view, 32.0f));
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
