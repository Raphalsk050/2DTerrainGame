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

// Horizontal bands a swaying plant is drawn in.
//
// Eight, and the number is bounded from both sides. Fewer and the bend is a
// staircase of visible steps; more and neighbouring bands round to the same whole
// pixel anyway, so the extra quads draw the same picture. At the sway amplitude
// below a mature tree spans about five distinct offsets, so eight bands is
// already past the point of diminishing return and the cost is a quad each.
constexpr int kSwayBands = 8;

// How far the top of a plant leans, as a share of its height, at the strongest
// gust the weather can produce.
//
// Small. A tree that leans a tenth of its height is a tree in a hurricane, and
// the sway is meant to read as a wood breathing rather than as an emergency.
constexpr float kSwayReach = 0.055f;

// Seconds one full sway takes at rest, and how much faster a hard wind drives it.
// A branch has its own period and the wind sets how hard it is pushed, not how
// fast it swings — but a stiff wind does shorten it, so the two are related and
// neither is a constant.
constexpr float kSwayPeriod = 3.1f;
constexpr float kSwayUrgency = 0.45f;

// Share of the sky a crown holds back from the ground under it.
//
// Read as cover rather than as extinction — see World::AddCover. A share, so it
// is directly how much daylight the wood floor loses, and half is a wood: dim
// enough to be its own place, bright enough to walk through.
constexpr float kCanopyShade = 0.5f;

// Seconds a tree takes to go over, and how long it lies there before it fades.
//
// Just over a second. Faster reads as the tree being deleted; slower and the
// player is waiting on an animation to finish before the wood arrives.
constexpr float kFallTime = 1.15f;
constexpr float kLieTime  = 1.4f;
constexpr float kFadeTime = 0.7f;

// The blow: how long the trunk rings for, how fast, and how far the crown moves
// at the top of it.
constexpr float kShakeTime  = 0.32f;
constexpr float kShakeDecay = 8.0f;
constexpr float kShakeRate  = 40.0f;
constexpr float kShakeReach = 0.035f;

// Weather minutes are what everything else in the world is timed in, and the
// tree clock is the same one, so a fall runs at the same speed under F7 as the
// rain it is falling in.
constexpr float kMinute = 60.0f;

// How far a trunk is from its own centre, as a share of the canopy width, for
// the purpose of being hit. Generous against the drawn trunk: a swing that
// visibly connects has to land.
constexpr float kStrikeSlack = 8.0f;

// Lattice steps of empty ground under a trunk before it is treated as standing
// over a hole. See Grove::Undermine for why one is not enough.
constexpr int kFootingProbe = 3;

// Cells either side the planting looks in when the one asked for is taken.
//
// One. Two put the sapling far enough away that the player could not see where
// it had gone, which reads as the seed having been eaten.
constexpr int kPlantReach = 1;

// Weather minutes a tree takes to close over a blow it survived.
//
// It has to heal, and not only because a scarred wood is odd: the overlay lets a
// record go once there is nothing left to say about a plant, and a wound that
// never closes is something to say for ever. Slower than felling one, so
// interrupting a chop still costs the player the progress.
constexpr float kHealMinutes = 3.0f;

// How far a tree turns as it goes over. Not quite flat, so the crown ends up
// resting on the ground rather than driven through it.
constexpr float kFallAngle = 84.0f;

// Deterministic value in [0,1) from a cell, a salt and the world seed.
//
// The same mix flora uses to place a plant, kept here rather than shared because
// what it answers is a different question — flora asks what grows, this asks what
// a felling gives up — and a shared one would tie a change in either to the other.
float Chance(std::int64_t cell, int salt, int seed) {
    auto bits = static_cast<std::uint64_t>(cell) * 0x9E3779B97F4A7C15ull;

    bits ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(salt)) * 0xBF58476D1CE4E5B9ull;
    bits ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)) * 0x94D049BB133111EBull;

    bits ^= bits >> 30;
    bits *= 0xBF58476D1CE4E5B9ull;
    bits ^= bits >> 27;
    bits *= 0x94D049BB133111EBull;
    bits ^= bits >> 31;

    return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24);
}

// Which stage a plant wears at a share of the way to maturity.
//
// Not evenly spaced, and should not be: a seedling stays a seedling for a long
// while and then goes quickly, which is what makes watching one worth doing.
flora::Stage StageOf(float growth) {
    if (growth >= 1.0f) return flora::Stage::Mature;
    if (growth >= 0.62f) return flora::Stage::Young;

    return flora::Stage::Sapling;
}

// How fast a plant grows, as a share of the way to maturity per second.
//
// The species names how long it takes under average light and rain, and the two
// factors below are written so that at the averages they multiply to one — so
// `maturityMinutes` means what it says, and better than average is faster rather
// than the number being a floor nothing reaches.
float Rate(const flora::SpeciesDef &def, const terrain::Climate &climate, float light, float water, float meanLight,
           float meanWater) {
    const flora::SpeciesGrowth &growth = def.growth;

    const float pace = 1.0f / std::max(growth.maturityMinutes * kMinute, 1.0f);

    const auto against = [](float need, float have, float mean) {
        // Below the mean this bites and above it this helps, both in proportion
        // to how much the species cares.
        return 1.0f - need + need * (have / std::max(mean, 1e-3f));
    };

    // And how well the place suits it at all. A tree at the edge of its range is
    // already stunted by the scatter; this is why it also took longer to get
    // there.
    const float suited = std::exp(-std::pow((climate.temperature - def.climate.temperature) /
                                                std::max(def.climate.temperatureWidth, 1e-3f),
                                            2.0f));

    return pace * against(growth.lightNeed, light, meanLight) * against(growth.waterNeed, water, meanWater) *
           (0.45f + 0.55f * suited);
}

// The falling-leaf field: how far apart the leaves are, how fast they drop and
// how far they swing as they go.
//
// Not a particle system, and deliberately. A leaf is one cell of a lattice, its
// whole life is a function of the cell's index and the clock, and nothing about
// it is stored between frames — the same arrangement the rain uses, for the same
// reason: there is no moment at which a leaf has to be told anything. What it
// cannot express is "this leaf came off *that* branch just now", and that is the
// one thing worth a real particle, which is why the burst at a chop is separate.
// The numbers below were first set by eye and came out invisible, which is worth
// writing down because the arithmetic says so plainly. Over a thousand-pixel view
// the old figures gave twenty-eight cells; a fifth of them held a leaf; a quarter
// of that survived the spring's shedding; half of those stood under a deciduous
// crown; and half again were inside the stretch of the fall that is on screen.
// **Four tenths of one leaf.** A field of particles nobody can ever see is
// indistinguishable from one that is switched off.
constexpr float kLeafCell  = 18.0f;
constexpr float kLeafFall  = 26.0f;
// The fall wraps over this, and only the stretch between a crown's top and its
// foot is on screen — so a span far longer than a tree spends most of every
// leaf's life culled. Near the height of a mature crown keeps three quarters of
// them visible instead of a half.
constexpr float kLeafSpan  = 170.0f;
constexpr float kLeafSwing = 13.0f;
constexpr float kLeafSide  = 0.9f;

// Share of the cells that hold a leaf at all, before the season thins them.
constexpr float kLeafDensity = 0.85f;

// How far a plant is through its fall, eased the way a rod hinged at its foot
// goes over: barely at first, then all at once.
//
// The exact curve is the pendulum's and it has no closed form worth the trouble;
// this is one minus a cosine, which has the same two properties that read —
// zero rate at the start and its fastest at the moment of impact.
float Toppling(float t) {
    const float u = std::clamp(t / kFallTime, 0.0f, 1.0f);
    return 1.0f - std::cos(u * 1.5707963f);
}

// How far the top of a plant is leaning now, in world pixels.
//
// The wind sets where the tree is pushed to and the swing is about that, rather
// than about upright — which is what makes a gust read as a shove followed by a
// wobble rather than as a shake that happens to coincide with it.
float Lean(const weather::Sky &sky, const flora::Plant &plant, float now) {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

    // As a share of what the strongest gust could do, so the whole wood is at
    // rest on a still day and near its limit in a storm.
    const float reach = std::max(sky.WindReach(), 1e-3f);
    const float push  = std::clamp(sky.WindAt(plant.base.x) / reach, -1.0f, 1.0f);

    // Each tree keeps its own phase, so a stand does not beat as one object.
    // Taken from the cell, so a tree sways the same way every time it is met.
    const float phase = static_cast<float>((plant.id * 2654435761LL) & 0xffff) / 65536.0f;

    const float rate = (1.0f + std::fabs(push) * kSwayUrgency) / kSwayPeriod;

    const float swing = std::sin((now * rate + phase) * 2.0f * 3.14159265f);

    // Two thirds of the lean is the wind holding the tree over and one third is
    // it swinging about that. A tree that only oscillated would cross upright in
    // a gale, which is the one thing a bent tree never does.
    return height * kSwayReach * push * (0.66f + 0.34f * swing);
}

} // namespace

void Grove::Configure(const flora::Settings &settings, const terrain::Settings &terrain, const weather::Sky &sky) {
    settings_ = settings;
    terrain_  = terrain;

    flora::Calibrate(settings_);

    // What a place gets when nobody is watching. Measured from the sky rather
    // than written down, so changing the length of the day or the odds of a storm
    // moves the rate an unwatched wood grows at, instead of quietly putting the
    // watched and the unwatched world on two different clocks.
    meanLight_ = std::max(sky.MeanDaylight(), 1e-3f);
    meanWater_ = std::max(sky.MeanRain(), 1e-3f);

    sheet_.Create();
}

void Grove::Unload() { sheet_.Unload(); }

void Grove::ReadGround(const World &world, Rectangle view) {
    const auto spacing = static_cast<float>(world.Spacing());
    const auto &rules  = settings_.layer[flora::LayerIndex(flora::Layer::Canopy)];

    // Everything the scatter will ask about, and the lead it is run over.
    //
    // The lead is the part that was missing, and it was the whole of why trees
    // fell over as the player walked. Update scatters from `view.x - kLead`, but
    // this prepared only as far as the canopy margin — so every plant in that
    // strip got GroundAt clamped to the first column of the buffer, which on a
    // cliff can be a hundred pixels from the truth. The plant was placed hanging
    // in the air, and Undermine, doing its job, cut it down on the frame it
    // appeared.
    //
    // Asking past the end is answered by the edge rather than by anything worse,
    // so the fault was silent: no crash, no warning, just a wood that fell down
    // as you approached it.
    const float margin =
        kLead + flora::Margin(flora::Layer::Canopy, settings_) + rules.cellSpan + rules.slopeSpan + spacing;

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

void Grove::Update(const World &world, Rectangle view, Vector2 player, float now, float dt, Harvest &into) {
    ReadGround(world, view);

    // Grown a little wider than the view, so a tree coming over the edge has a
    // frame or two to be drawn into the sheet before anybody could see that it
    // was not there yet.
    flora::Scatter(flora::Layer::Canopy, view.x - kLead, view.x + view.width + kLead, settings_, terrain_, ground_,
                   plants_);

    // Trees that have just finished going over give up what they were carrying.
    //
    // Done here rather than at the moment of the killing blow, because what the
    // player is owed arrives when the trunk hits the ground — the whole point of
    // drawing the fall is that the reward is at the end of it.
    for (const flora::Plant &plant : plants_) {
        const auto found = remembered_.find(plant.id);
        if (found == remembered_.end()) continue;

        TreeState &state = found->second;

        if (state.dropped || state.felledAt < 0.0f || now - state.felledAt < kFallTime) continue;

        state.dropped = true;

        Yield(plant, state, now);
    }

    // And the floor under them. Grown after the trees, because it asks where they
    // are: nothing takes root inside a trunk, and what grows in the open is not
    // what grows in the shade.
    flora::Scatter(flora::Layer::Undergrowth, view.x - kLead, view.x + view.width + kLead, settings_, terrain_,
                   ground_, undergrowth_);

    Thin();

    Planted(view);
    Undermine(world, now);
    Ripen(world, now, dt);

    drops_.Update(world, player, dt, now, into);

    Forget(now);
}

void Grove::Thin() {
    // Cleared out from under the trunks, and thinned by what stands over them.
    //
    // The only place the two passes touch, and it runs one way: the undergrowth
    // asks about the trees and the trees never ask about it, so neither pass has
    // to be told when the other changes and both stay pure functions of position.
    std::size_t kept = 0;

    for (std::size_t i = 0; i < undergrowth_.size(); i++) {
        const flora::Plant &plant = undergrowth_[i];

        bool blocked = false;
        float shade  = 0.0f;

        for (const flora::Plant &tree : plants_) {
            const flora::SpeciesDef &def = flora::Def(tree.species);

            const float width = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * tree.scale;

            const float apart = std::fabs(tree.base.x - plant.base.x);

            // Inside the trunk's own footing, nothing grows.
            if (apart < std::max(width * def.shape.trunkWidth, config::kFloraPixel * 2.0f)) {
                blocked = true;
                break;
            }

            // Under the crown, some do and some do not.
            if (apart < width * 0.5f) shade = std::max(shade, 1.0f - apart / (width * 0.5f));
        }

        if (blocked) continue;

        // A fern wants the shade a bush does not. The species' own light need is
        // read backwards here, which is exactly what it means: needing little
        // light is the same fact as tolerating shade.
        const flora::SpeciesDef &def = flora::Def(plant.species);

        const float wants = 1.0f - def.growth.lightNeed;
        const float suits = 1.0f - shade + shade * (0.35f + 1.3f * wants);

        if (Chance(plant.id, 57, settings_.seed) >= std::clamp(suits, 0.0f, 1.0f)) continue;

        undergrowth_[kept++] = plant;
    }

    undergrowth_.resize(kept);
}

void Grove::Planted(Rectangle view) {
    for (const auto &[cell, state] : remembered_) {
        if (!state.planted) continue;
        if (state.at.x < view.x - kLead || state.at.x > view.x + view.width + kLead) continue;

        flora::Plant plant;

        plant.id      = cell;
        plant.species = static_cast<flora::Species>(state.species);
        plant.base    = state.at;
        plant.scale   = 1.0f;

        plants_.push_back(plant);
    }
}

void Grove::Undermine(const World &world, float now) {
    const auto step = static_cast<float>(world.Spacing());

    for (const flora::Plant &plant : plants_) {
        // Standing on nothing. Placement reads the skyline, which is the shape of
        // the land and takes no notice of digging — that is deliberate, so a wood
        // does not rearrange itself around a hole — but a tree left hanging over
        // one somebody dug has to answer for it, and the honest answer is that it
        // comes down.
        //
        // The one place the built surface is consulted rather than the noise.
        //
        // Probed several steps down rather than one pixel, and the reason is a
        // trap worth writing down. World::Skyline starts its scan at
        // `Height(x) - kSkylineHeadroom` and walks down by the lattice step, so
        // what it returns is that height plus a whole number of steps — which is
        // **not** a multiple of the step, and therefore not on the lattice. A
        // plant's base is that value, while IsSolidAt snaps its query to the
        // nearest lattice vertex. The two live on grids up to half a step apart,
        // and half a step at the edge of the ground is exactly the difference
        // between rock and sky: a single shallow probe read air under perfectly
        // ordinary trees and felled them the instant they came into view.
        //
        // A hole three steps deep is somebody digging. Anything shallower is the
        // two grids disagreeing.
        bool footing = false;

        for (int probe = 1; probe <= kFootingProbe && !footing; probe++) {
            footing = world.IsSolidAt({plant.base.x, plant.base.y + static_cast<float>(probe) * step});
        }

        if (footing) continue;

        const auto found = remembered_.find(plant.id);
        if (found != remembered_.end() && found->second.felledAt >= 0.0f) continue;

        TreeState &state = Remember(plant, now);

        state.felledAt = now;
        state.health   = 0.0f;

        // Into the hole rather than away from a player who may be nowhere near.
        state.fallLeft = Chance(plant.id, 91, settings_.seed) < 0.5f;
    }
}

void Grove::Ripen(const World &world, float now, float dt) {
    for (const flora::Plant &plant : plants_) {
        auto found = remembered_.find(plant.id);
        if (found == remembered_.end()) continue;

        TreeState &state = found->second;

        // A tree that was struck and left alone closes over it. Without this the
        // wound is permanent and so is the record: `Forget` only lets go of a
        // plant at full health, so every tree the player ever hit and walked away
        // from stayed in the overlay for good.
        if (state.health < 1.0f && state.felledAt < 0.0f) {
            const float gap = std::max(now - state.updatedAt, 0.0f);

            state.health = std::min(state.health + gap / (kHealMinutes * kMinute), 1.0f);
        }

        if (state.growth >= 1.0f || state.felledAt >= 0.0f) {
            state.updatedAt = now;
            continue;
        }

        const flora::SpeciesDef &def   = flora::Def(plant.species);
        const terrain::Climate climate = terrain::ClimateAt(plant.base.x, terrain_);

        const float gap = std::max(now - state.updatedAt, 0.0f);

        // Whatever of the gap is longer than one frame is time nobody watched.
        const float away = std::max(gap - dt, 0.0f);
        const float here = std::min(gap, dt);

        if (away > 0.0f) state.growth += away * Rate(def, climate, meanLight_, meanWater_, meanLight_, meanWater_);

        if (here > 0.0f) {
            state.growth += here * Rate(def, climate, world.LightLevelAt(plant.base), world.HumidityAt(plant.base),
                                        meanLight_, meanWater_);
        }

        state.growth    = std::min(state.growth, 1.0f);
        state.updatedAt = now;
    }
}

void Grove::Yield(const flora::Plant &plant, const TreeState &state, float now) {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

    // Out of the stump, and thrown along the line the tree fell.
    //
    // It was thrown from the middle of the fallen trunk, which reads better and
    // is wrong: a tree that goes over into a hillside has its middle inside the
    // hill, and wood that starts inside rock has no direction it can move in. The
    // stump is the one place that is certainly open, because a tree was standing
    // in it a second ago.
    const Vector2 from = {plant.base.x, plant.base.y - height * 0.10f};

    const float away = state.fallLeft ? -1.0f : 1.0f;

    for (const flora::DropRule &rule : def.drops) {
        if (rule.chance <= 0.0f || rule.most <= 0) continue;

        // The tree's own cell decides, so felling the same tree twice — after a
        // regrowth, or in another session — gives the same wood. Nothing about a
        // drop is left to the moment it happened in.
        const float roll = Chance(plant.id, static_cast<int>(rule.item) * 71 + 3, settings_.seed);

        if (roll >= rule.chance) continue;

        const float amount = Chance(plant.id, static_cast<int>(rule.item) * 71 + 11, settings_.seed);

        const int count = rule.least + static_cast<int>(amount * static_cast<float>(rule.most - rule.least + 1));

        drops_.Spawn(rule.item, std::clamp(count, rule.least, rule.most), from, away, now);
    }
}

void Grove::Draw(const weather::Sky &sky, flora::Season season, float now) const {
    if (!sheet_.Ready()) return;

    const float pixel = config::kFloraPixel;

    sheet_.Begin();

    // The floor first, so a trunk stands in front of the ferns around it.
    for (const flora::Plant &plant : undergrowth_) {
        const canopy::Sprite *sprite = sheet_.Acquire(plant, flora::Stage::Mature, season);
        if (sprite == nullptr) continue;

        const float lean = Lean(sky, plant, now);

        DrawTexturePro(sheet_.Texture(), sprite->source,
                       {Snap(plant.base.x - sprite->anchor.x * pixel + lean),
                        Snap(plant.base.y - sprite->anchor.y * pixel), sprite->source.width * pixel,
                        sprite->source.height * pixel},
                       {0.0f, 0.0f}, 0.0f, WHITE);
    }

    for (const flora::Plant &plant : plants_) {
        // What has happened to it, if anything. Mature and undamaged where there
        // is no record, which is what an untouched wood is.
        const Standing standing = Read(plant, now);

        // Down and gone: the cut trunk is all that is left, and it stays until the
        // species' own regrowth takes it.
        if (standing.stump) {
            DrawStump(plant);
            continue;
        }

        // The stage is part of what a sprite *is*, so it goes into the key: a
        // sapling and the tree it becomes are two drawings, and the sheet keeps
        // whichever is being asked for.
        const canopy::Sprite *sprite = sheet_.Acquire(plant, standing.stage, season);

        // Nothing yet: the frame's drawing budget is spent and this tree will be
        // there on one of the next few. Skipped rather than drawn some other way,
        // because the only thing worse than a tree arriving a frame late is a
        // different tree standing in for it.
        if (sprite == nullptr) continue;

        const float left = Snap(plant.base.x - sprite->anchor.x * pixel);
        const float top  = Snap(plant.base.y - sprite->anchor.y * pixel);

        if (standing.felling >= 0.0f) {
            // Turned about the foot of its own trunk, which is what the anchor is
            // for. One quad and no bands: a tree on its way over is a rigid thing
            // pivoting, and the bands exist to bend a standing one.
            //
            // The texels turn with it. That is not how a pixel artist would draw
            // each frame of it, and it is what every game of this kind does,
            // because the alternative is re-rasterising the tree at every angle it
            // passes through.
            const float angle = kFallAngle * Toppling(standing.felling) * (standing.fallLeft ? -1.0f : 1.0f);

            const Rectangle target = {plant.base.x, plant.base.y, sprite->source.width * pixel,
                                      sprite->source.height * pixel};

            const Vector2 pivot = {sprite->anchor.x * pixel, sprite->anchor.y * pixel};

            DrawTexturePro(sheet_.Texture(), sprite->source, target, pivot, angle,
                           Fade(WHITE, standing.fade));

            // The stump is under it from the moment it starts to go, so there is
            // never a frame with nothing where the tree was.
            DrawStump(plant);
            continue;
        }

        // How far the top of this plant leans, in world pixels, right now: the
        // wind, plus whatever is left of the last blow.
        const float lean = Lean(sky, plant, now) + standing.shake;

        // A tree still enough to be one quad is drawn as one. Most of a calm wood
        // is, and a strip draw costs a quad per band.
        if (std::fabs(lean) < pixel * 0.5f) {
            DrawTexturePro(sheet_.Texture(), sprite->source,
                           {left, top, sprite->source.width * pixel, sprite->source.height * pixel}, {0.0f, 0.0f},
                           0.0f, WHITE);
            continue;
        }

        // Otherwise in horizontal bands, each shifted further than the one below
        // it. A shear rather than a rotation, because a tree does not pivot at
        // its foot in the wind — it bends, and the bend is nearly all in the
        // crown.
        for (int band = 0; band < kSwayBands; band++) {
            const float from = static_cast<float>(band) / static_cast<float>(kSwayBands);
            const float to   = static_cast<float>(band + 1) / static_cast<float>(kSwayBands);

            // Rows are counted from the top of the sprite, and the sway is
            // measured from the foot, so the band nearest the ground is the last.
            const float y0 = std::floor(sprite->source.height * from);
            const float y1 = std::floor(sprite->source.height * to);

            if (y1 <= y0) continue;

            // How high up the plant the middle of this band sits, in [0,1].
            const float height = 1.0f - (y0 + y1) * 0.5f / std::max(sprite->source.height, 1.0f);

            // Squared, so the foot barely moves and the crown carries the bend.
            // A linear ramp reads as the whole tree sliding sideways.
            const float shift = lean * height * height;

            // Quantised to whole plant pixels. The offsets *between* bands are
            // what a viewer sees, and a fractional one tears the sprite along the
            // seam between two of them — every row of a band is drawn from one
            // offset, so the offset has to land on the grid the texels are on.
            const float offset = std::round(shift / pixel) * pixel;

            const Rectangle source = {sprite->source.x, sprite->source.y + y0, sprite->source.width, y1 - y0};

            const Rectangle target = {left + offset, top + y0 * pixel, sprite->source.width * pixel,
                                      (y1 - y0) * pixel};

            DrawTexturePro(sheet_.Texture(), source, target, {0.0f, 0.0f}, 0.0f, WHITE);
        }
    }
}

Grove::Standing Grove::Read(const flora::Plant &plant, float now) const {
    Standing standing;

    const auto found = remembered_.find(plant.id);
    if (found == remembered_.end()) return standing;

    const TreeState &state = found->second;

    standing.stage = StageOf(state.growth);

    // Struck lately: the trunk still rings. A decaying oscillation, and the two
    // numbers it needs are already in the record, so this costs no state of its
    // own.
    if (state.struckAt >= 0.0f) {
        const float since = now - state.struckAt;

        if (since < kShakeTime) {
            const float height =
                flora::Def(plant.species).height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

            standing.shake = height * kShakeReach * std::exp(-kShakeDecay * since) * std::sin(kShakeRate * since);
        }
    }

    if (state.felledAt >= 0.0f) {
        const float since = now - state.felledAt;

        standing.felling  = since;
        standing.fallLeft = state.fallLeft;
        standing.stump    = since > kFallTime + kLieTime + kFadeTime;

        // Lies where it landed for a moment and then goes. The pause matters:
        // without it the tree vanishes at the instant of impact and the fall
        // reads as the tree being deleted rather than as it coming down.
        const float lying = since - kFallTime - kLieTime;

        standing.fade = (lying <= 0.0f) ? 1.0f : std::clamp(1.0f - lying / kFadeTime, 0.0f, 1.0f);
    }

    return standing;
}

Grove::TreeState &Grove::Remember(const flora::Plant &plant, float now) {
    auto found = remembered_.find(plant.id);
    if (found != remembered_.end()) return found->second;

    TreeState fresh;
    fresh.updatedAt = now;
    fresh.species   = static_cast<std::uint8_t>(flora::SpeciesIndex(plant.species));

    return remembered_.emplace(plant.id, fresh).first->second;
}

void Grove::Forget(float now) {
    for (auto it = remembered_.begin(); it != remembered_.end();) {
        const TreeState &state = it->second;

        // A planted one is the overlay's own assertion and can never be dropped:
        // the procedural pass does not know it is there.
        if (state.planted) {
            ++it;
            continue;
        }

        if (state.felledAt >= 0.0f) {
            // Regrown. The species table decides how long a stump stays, and
            // kNever leaves it for good.
            const float regrow =
                flora::kSpecies[state.species].growth.regrowMinutes * kMinute;

            it = (now - state.felledAt > regrow) ? remembered_.erase(it) : std::next(it);
            continue;
        }

        // Undamaged, unfelled, unpicked and fully grown is exactly what a plant
        // with no record is, so the record is worth nothing and goes.
        const bool quiet = state.health >= 1.0f && state.growth >= 1.0f && state.fruitAt < 0.0f;

        it = quiet ? remembered_.erase(it) : std::next(it);
    }
}

void Grove::Strike(Rectangle hitbox, float damage, Vector2 from, float now) {
    for (const flora::Plant &plant : plants_) {
        const flora::SpeciesDef &def = flora::Def(plant.species);

        const Standing standing = Read(plant, now);
        if (standing.felling >= 0.0f) continue;

        const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;
        const float width  = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

        // The trunk alone. Swinging at the crown of a tree twenty pixels over
        // your head should not fell it.
        const float half = std::max(width * def.shape.trunkWidth * 0.5f, 1.0f) + kStrikeSlack;

        const Rectangle trunk = {plant.base.x - half, plant.base.y - height * def.shape.clearance, half * 2.0f,
                                 height * def.shape.clearance};

        if (!CheckCollisionRecs(hitbox, trunk)) continue;

        TreeState &state = Remember(plant, now);

        state.struckAt = now;
        state.health -= damage / std::max(def.growth.toughness, 0.5f);

        if (state.health > 0.0f) return;

        state.felledAt = now;

        // Away from whoever swung. A tree that fell towards the axe would be the
        // one thing in the world actively trying to land on the player.
        state.fallLeft = from.x > plant.base.x;

        return;
    }
}

flora::Species Grove::Suited(float worldX) const {
    const terrain::Climate climate = terrain::ClimateAt(worldX, terrain_);

    flora::Species best = flora::Species::Oak;
    float highest       = -1.0f;

    for (std::size_t e = 0; e < flora::kSpeciesCount; e++) {
        const flora::SpeciesDef &def = flora::kSpecies[e];
        if (def.layer != flora::Layer::Canopy) continue;

        const auto bell = [](float value, float centre, float width) {
            const float t = (value - centre) / std::max(width, 1e-3f);
            return std::exp(-t * t);
        };

        const float fit = bell(climate.temperature, def.climate.temperature, def.climate.temperatureWidth) *
                          bell(climate.humidity, def.climate.humidity, def.climate.humidityWidth);

        if (fit <= highest) continue;

        highest = fit;
        best    = static_cast<flora::Species>(e);
    }

    return best;
}

bool Grove::Plant(flora::Species species, Vector2 world, float now) {
    // One per cell, the same rule the scatter follows — otherwise a planted wood
    // could be packed tighter than any wood the world grows. But a cell holding
    // nothing but a stump is free: cutting a tree and putting one back where it
    // stood is the most natural thing a player does, and refusing it silently was
    // the worst of both answers.
    const auto vacant = [this](std::int64_t cell) {
        if (const auto found = remembered_.find(cell); found != remembered_.end()) {
            const TreeState &state = found->second;

            if (state.planted || state.felledAt < 0.0f) return false;
        }

        for (const flora::Plant &standing : plants_) {
            if (standing.id != cell) continue;

            // Standing timber, unless it is the felled one whose record said so.
            const auto found = remembered_.find(cell);
            if (found == remembered_.end() || found->second.felledAt < 0.0f) return false;
        }

        return true;
    };

    std::int64_t cell = flora::CellAt(flora::Layer::Canopy, settings_, world.x);

    // And if the spot really is taken, the next one over will do. Walking a
    // player two paces to find a gap is a worse game than putting the sapling
    // where a sapling can go.
    if (!vacant(cell)) {
        const std::int64_t wanted = cell;

        cell = wanted;

        for (int step = 1; step <= kPlantReach && !vacant(cell); step++) {
            cell = vacant(wanted + step) ? wanted + step : wanted - step;
        }

        if (!vacant(cell)) return false;
    }

    TreeState fresh;

    fresh.plantedAt = now;
    fresh.updatedAt = now;
    fresh.growth    = 0.0f;
    fresh.planted   = true;
    fresh.species   = static_cast<std::uint8_t>(flora::SpeciesIndex(species));

    // Where the player asked, held inside whichever cell took it.
    //
    // The centre of the cell is what this was, and it made planting look broken:
    // when the spot asked for was taken, the search moved a cell or two over and
    // then put the sapling in the *middle* of that one — up to a couple of
    // hundred pixels away, off the edge of what the player was looking at. The
    // sapling went in, the seed was spent, and nothing appeared to have happened.
    //
    // Clamped rather than centred, so it lands at the player's feet when the cell
    // is theirs and against the near edge when it is not.
    const float span   = settings_.layer[flora::LayerIndex(flora::Layer::Canopy)].cellSpan;
    const float inset  = span * 0.15f;

    const float where = std::clamp(world.x, static_cast<float>(cell) * span + inset,
                                   static_cast<float>(cell + 1) * span - inset);

    fresh.at = {where, flora::GroundAt(ground_, where) + ground_.spacing * 0.5f};

    remembered_.emplace(cell, fresh);

    return true;
}

void Grove::DrawStump(const flora::Plant &plant) const {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const float width = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

    // A little wider than the trunk, the way a cut one flares, and just tall
    // enough to read as something left behind rather than as a mark on the ground.
    const float half = std::max(width * def.shape.trunkWidth * 0.75f, config::kFloraPixel);
    const float tall = std::max(half * 1.1f, config::kFloraPixel * 2.0f);

    const flora::SpeciesPalette &palette = def.palette[flora::SeasonIndex(flora::Season::Summer)];

    DrawRectangleV({Snap(plant.base.x - half), Snap(plant.base.y - tall)}, {half * 2.0f, tall}, palette.bark);

    // The cut face, which is the only part of a stump anybody looks at.
    DrawRectangleV({Snap(plant.base.x - half), Snap(plant.base.y - tall)}, {half * 2.0f, config::kFloraPixel},
                   palette.barkLight);
}

void Grove::Shade(World &world, float now) const {
    if (!sheet_.Ready()) return;

    for (const flora::Plant &plant : plants_) {
        const flora::SpeciesDef &def = flora::Def(plant.species);

        // A tree on its way down or gone stops shading the ground. Nothing has to
        // be told: the shade is re-offered every frame and this one simply is not
        // offered.
        const Standing standing = Read(plant, now);
        if (standing.felling >= 0.0f || standing.stump) continue;

        // And a tree that has not been drawn casts nothing. The two used to
        // disagree — this walked every plant while the draw skipped whatever the
        // frame's budget had not reached — and what reached the screen was a row
        // of grey blobs hanging in an empty sky, one for every tree that was not
        // there yet. Asking the sheet is what keeps the shadow and the thing
        // casting it the same set.
        //
        // Asked and not requested: this must not spend the frame's drawing budget
        // or pick a season, both of which are the draw's business.
        if (!sheet_.Holds(plant.id)) continue;

        // The crown alone, not the whole sprite: the bare trunk below it stops
        // nothing worth solving for, and including it would put a column of dusk
        // down to the ground under every tree.
        const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;
        const float width  = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

        const float foot = height * def.shape.clearance;

        (void)height;
        (void)foot;

        world.AddCover(plant.base.x - width * 0.5f, plant.base.x + width * 0.5f, kCanopyShade);
    }
}

void Grove::DrawFruit(flora::Season season, float now) const {
    // Fruit sets in spring and is worth picking through the summer. Outside that
    // window a tree that bears is just a tree.
    const float ripe = (season == flora::Season::Summer)   ? 1.0f
                       : (season == flora::Season::Spring) ? 0.35f
                                                           : 0.0f;

    if (ripe <= 0.0f) return;

    const float pixel = config::kFloraPixel;

    for (const flora::Plant &plant : plants_) {
        const flora::SpeciesDef &def = flora::Def(plant.species);

        // A species bears if its table says it drops fruit. Nothing else has to
        // be declared: the drop table already knows.
        const flora::DropRule *bears = nullptr;

        for (const flora::DropRule &rule : def.drops) {
            if (rule.item == Item::Apple && rule.chance > 0.0f) bears = &rule;
        }

        if (bears == nullptr) continue;

        const Standing standing = Read(plant, now);
        if (standing.felling >= 0.0f || standing.stump || standing.stage != flora::Stage::Mature) continue;

        // Picked lately, and not back yet.
        const auto found = remembered_.find(plant.id);
        if (found != remembered_.end() && found->second.fruitAt >= 0.0f) {
            if (now - found->second.fruitAt < def.growth.maturityMinutes * kMinute * 0.5f) continue;
        }

        const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;
        const float width  = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

        const float foot = height * def.shape.clearance;

        // Hung at points fixed to the tree, so the fruit on one does not move
        // about between frames.
        const int count = 3 + static_cast<int>(Chance(plant.id, 61, settings_.seed) * 4.0f * ripe);

        for (int i = 0; i < count; i++) {
            const float ax = (Chance(plant.id, 67 + i * 3, settings_.seed) - 0.5f) * width * 0.72f;
            const float ay = foot + Chance(plant.id, 71 + i * 3, settings_.seed) * (height - foot) * 0.8f;

            DrawRectangleV({Snap(plant.base.x + ax), Snap(plant.base.y - ay)}, {pixel, pixel},
                           Def(Item::Apple).colour);
        }
    }
}

void Grove::DrawLeaves(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const {
    // Autumn sheds and the rest of the year barely does, which is the whole of
    // what a season means to a leaf. Winter has nothing left to drop.
    // Autumn sheds and the rest of the year barely does — but "barely" still has
    // to be visible, or the whole field may as well not run.
    const float shedding = (season == flora::Season::Autumn)   ? 1.0f
                           : (season == flora::Season::Spring) ? 0.7f
                           : (season == flora::Season::Winter) ? 0.0f
                                                              : 0.5f;

    if (shedding <= 0.0f || plants_.empty()) return;

    const float pixel = config::kFloraPixel;

    // Blown along with everything else, so the leaves and the crowns they came
    // off lean the same way.
    const float wind = sky.WindAt(view.x + view.width * 0.5f);

    const int first = static_cast<int>(std::floor(view.x / kLeafCell)) - 1;
    const int last  = static_cast<int>(std::ceil((view.x + view.width) / kLeafCell)) + 1;

    for (int cell = first; cell <= last; cell++) {
        if (Chance(cell, 13, settings_.seed) > kLeafDensity * shedding) continue;

        const float column = (static_cast<float>(cell) + Chance(cell, 17, settings_.seed)) * kLeafCell;

        // Only where something is standing to have shed it. A leaf falling in an
        // open field is the one thing that would give the trick away.
        const flora::Plant *under = nullptr;

        for (const flora::Plant &plant : plants_) {
            const flora::SpeciesDef &def = flora::Def(plant.species);
            if (!def.deciduous) continue;

            const float width = def.canopyWidth[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

            if (std::fabs(plant.base.x - column) > width * 0.5f) continue;

            under = &plant;
            break;
        }

        if (under == nullptr) continue;

        const flora::SpeciesDef &def = flora::Def(under->species);

        const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * under->scale;

        // Leaves the crown and wraps back to it, so the fall runs for ever
        // without anything having to be respawned.
        const float top = under->base.y - height;

        const float pace  = kLeafFall * (0.7f + 0.6f * Chance(cell, 23, settings_.seed));
        const float drift = std::fmod(Chance(cell, 29, settings_.seed) * kLeafSpan + now * pace, kLeafSpan);

        const float y = top + drift;
        if (y > under->base.y) continue;

        // A leaf does not fall straight: it swings from side to side as it goes,
        // and the swing is most of what says leaf rather than raindrop.
        const float swing = std::sin((drift / kLeafSwing) + Chance(cell, 31, settings_.seed) * 6.28318f);

        const float x = column + swing * kLeafSwing * kLeafSide + wind * (drift / kLeafSpan) * 0.5f;

        // The species' own tones, so what falls is the colour of what it fell
        // from.
        const flora::SpeciesPalette &palette = def.palette[flora::SeasonIndex(season)];

        // Two pixels rather than one, turned with the swing.
        //
        // A single square reads as a speck of dirt on the screen at any distance;
        // a pair reads as a leaf, and turning the pair as it swings is most of
        // what says it is falling rather than being carried straight down.
        const float lead = Snap(x);
        const float over = Snap(y);

        DrawRectangleV({lead, over}, {pixel, pixel}, palette.leaf[3]);
        DrawRectangleV({lead + (swing >= 0.0f ? pixel : -pixel), over + pixel}, {pixel, pixel}, palette.leaf[1]);
    }
}

void Grove::DrawSheet() const {
    if (!sheet_.Ready()) return;

    const Texture2D texture = sheet_.Texture();

    DrawRectangle(0, 0, texture.width, texture.height, {18, 20, 26, 235});
    DrawTexture(texture, 0, 0, WHITE);
    DrawRectangleLines(0, 0, texture.width, texture.height, {120, 200, 140, 255});
}
