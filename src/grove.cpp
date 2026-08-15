#include "grove.h"

#include "config.h"
#include "marching_squares.h"

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

// The sway's two amplitudes are declared in grove.h, because sod.h mirrors them and
// a probe measures against them. The argument for them belongs here.
//
// `kSwayHold` is small: a tree that leans a tenth of its height is a tree in a
// hurricane, and the sway is meant to read as a wood breathing rather than as an
// emergency.
//
// There are two terms and not one because they answer to the wind differently. What
// holds a tree over is the mean pressure on it and goes to nothing as the air goes
// still; what shakes it is the turbulence in that air, and a tree is a resonant
// thing that rings from very little of it. A single term scaled by the wind alone
// left a crown moving a fraction of a texel on a clear afternoon — and the bend is
// drawn in whole plant pixels, so a fraction of one is nothing at all and the whole
// wood stood frozen.
//
// The quiver is therefore floored: `kSwayIdle` of it runs in dead air, and the rest
// comes up with the root of the wind rather than with the wind, so a light breeze
// already shakes a tree noticeably while a gale only shakes it somewhat more. Under
// a root the growth is steepest exactly where the numbers are smallest, which is
// where it was needed.

// Seconds one full sway takes in still air, and seconds it takes when the wind is
// hard on. A branch has its own period and the wind sets how hard it is pushed, not
// how fast it swings — but a stiff wind does shorten it, so there are two.
//
// Two periods rather than one period and a multiplier, because the wind has to
// choose between fixed rates rather than set a rate. See Lean for what setting one
// costs; in short, a rate multiplied by the absolute clock is not a frequency.
//
// `kSwayUrgency` is now how much of the wind it takes to hear the quick one alone:
// at this figure a gale is entirely the quick sway and a light breeze is almost
// entirely the slow one.
constexpr float kSwayPeriod  = 3.1f;
constexpr float kSwayHurry   = 1.6f;
constexpr float kSwayUrgency = 1.30f;

// Share of the sky a crown holds back from the ground under it.
//
// Read as cover rather than as extinction — see World::AddCover. A share, so it
// is directly how much daylight the wood floor loses, and half is a wood: dim
// enough to be its own place, bright enough to walk through.
constexpr float kCanopyShade = 0.5f;

// Seconds a tree takes to go over, and how long the trunk takes to go once it
// has landed.
//
// Just over a second for the fall. Faster reads as the tree being deleted;
// slower and the player is waiting on an animation to finish before the wood
// arrives.
//
// And then it is gone, in a tenth of a second, at the moment of impact. This was
// a long pause with the trunk lying on the ground and a slow fade after it, on
// the reasoning that vanishing at the instant of impact reads as deletion — and
// that reasoning was wrong about which moment carries the fall. The impact is the
// event; a trunk lying still for two and a half seconds afterwards is the tree
// having already happened, and it puts the reward that far behind the swing that
// earned it. What sells the landing is what comes off it — see the burst — not
// how long the corpse stays on screen.
constexpr float kFallTime   = 1.15f;
constexpr float kVanishTime = 0.10f;

// The blow: how long the trunk rings for, how fast, and how far the crown moves
// at the top of it.
//
// The reach was 0.035, which on an oak is four and a half pixels at the top of
// the crown and under two thirds of one at the height a person swings at. A blow
// that moves the thing struck by half a pixel is a blow nobody can see landing,
// and the wobble was over in a third of a second on top of that. At these figures
// an oak's crown swings ten pixels and rings for half a second, which is a tree
// taking a hit.
constexpr float kShakeTime  = 0.50f;
constexpr float kShakeDecay = 5.5f;
constexpr float kShakeRate  = 34.0f;
constexpr float kShakeReach = 0.080f;

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

// How close together two trunks may be planted, in world pixels.
//
// One block, which is to say very nearly touching. What this replaces was a
// spacing of a hundred and ten — the width of the scatter's cell — and that is a
// rule about how a wood arranges itself when it grows on its own, not about what
// a player may do with a sapling in their hand. Minecraft lets saplings sit in
// neighbouring blocks and so does this.
//
// Not zero, because two trunks in the same place are one trunk drawn twice.
constexpr float kPlantApart = 16.0f;

// Where the ids of planted trees begin.
//
// Records are keyed by cell index for everything the world grew itself, so a
// planted tree needs a key no cell can ever be. A cell index is a world position
// over the cell span, and a world position is a float — which stops counting in
// whole pixels above two to the twenty-fourth, about sixteen million. Over a
// span of a hundred and ten that is an index under a hundred and fifty thousand,
// and this base clears the largest one the world can produce by seven million
// times.
constexpr std::int64_t kPlantedBase = 1LL << 40;

// And where the ids of the ghosts begin, one per species.
//
// A ghost is a real sprite of a real species and so needs a slot in the sheet
// like anything else, but it is not a plant and has no cell. Its own id per
// species means the sheet bakes each one once and hands it back for nothing every
// frame the cursor hovers — where one shared id would re-bake on every change of
// species, since the sheet keys on the cell and not on what grows in it.
//
// Above the planted base by a margin no hand can reach: a player would have to
// plant a trillion saplings to collide with it.
constexpr std::int64_t kGhostBase = 1LL << 52;

// How solid a ghost is drawn.
//
// Enough to read as the tree it is promising and not enough to be mistaken for
// one standing there.
constexpr float kGhostFade = 0.45f;

// Share of the world's own plants that are not yet grown.
//
// A wood is a grown wood — that is what makes "no record means a mature tree" the
// right default, and it stays right. But *every* tree being full grown was its own
// kind of wrong, and it is the one a player meets with an axe in their hand: the
// scatter varies a plant's size by a third either way, so a wood holds trees at
// two thirds the height of their neighbours, and every one of them took a full
// tree's five blows and paid a full tree's timber. What looked like a sapling was
// an oak wearing a smaller coat.
//
// An eighth, which is about what a managed wood carries as young growth. They are
// real saplings and young trees — one blow and a seed back, or a short tree and
// less wood — and they come up over the session at their own paces.
constexpr float kYoungShare = 0.12f;

// How far along the youngest of them start.
//
// Off the floor, so the eighth above is mostly *young* trees rather than mostly
// seedlings: a wood with one plant in ten ankle-high reads as a nursery. At a
// quarter, half of the young growth is past the sapling stage on the first frame
// and the rest arrives there within a few minutes.
constexpr float kYoungFloor = 0.25f;

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

// What is left standing where a tree came down, in world pixels.
//
// A share of the tree's own height, bounded at both ends. Height and not trunk
// width, which is what it was: a pine's trunk is the narrowest in the table and
// its tree the tallest, so the biggest thing in the wood left the smallest mark
// in it — eight pixels, which beside a character of twenty-six is a scuff on the
// ground rather than a stump.
//
// The bounds are what keep it a stump. Below the floor it stops reading as
// something to walk up to and swing at; above the ceiling it is as tall as the
// player, and the thing left behind starts to compete with the thing that was
// felled.
constexpr float kStumpHeight  = 0.14f;
constexpr float kStumpFloor   = 14.0f;
constexpr float kStumpCeiling = 20.0f;

// How far the cut flares past the trunk that stood on it, as a share of the
// trunk's own width. A stump is the root collar and is wider than the wood above
// it.
constexpr float kStumpWiden = 1.5f;

// The stump taking a blow: how long it jolts for, how fast, and how far.
//
// Sharper and shorter than the crown's ring above, because they are two different
// things being hit. A crown is a mass on a springy trunk and it swings; a stump is
// a block of wood in the ground, and what an axe does to one is knock it a couple
// of pixels sideways and stop.
//
// Four pixels is two of the texels a stump is drawn on, which is what makes the
// jolt read at all — this is a thing twelve texels wide, and moving it by less
// than one is moving it by nothing.
constexpr float kJoltTime  = 0.24f;
constexpr float kJoltDecay = 13.0f;
constexpr float kJoltRate  = 58.0f;
constexpr float kJolt      = 4.0f;

// How long the cut face stays pale after a blow.
//
// Two frames at sixty. The eye reads it as the axe biting rather than as the
// stump changing colour, which is the whole of what a flash is for — and any
// longer and it becomes the second of those.
constexpr float kJoltFlash = 0.035f;

// How far into a stump the axe has eaten by the time it is about to break, in
// texels off the top.
//
// The part that answers "it does not feel like I am getting anywhere". A jolt says
// the blow landed and says nothing about the one before it; this is the record of
// every blow so far, standing in the world where the player can see it. Bounded
// well short of the whole stump: it has to still read as a stump at the moment it
// gives way, or the last blow lands on something that already looks cleared.
constexpr float kStumpChew = 3.0f;

// The chips an axe throws out of wood: how many, how long they last, and how they
// travel.
//
// Faster and much heavier than a leaf, and they do not flutter. A chip is a
// splinter of the trunk — it leaves fast, it drops fast, and it is gone before it
// can be looked at, which is exactly the difference between debris and a leaf.
constexpr int kChipCount  = 7;
constexpr float kChipLife = 0.45f;
constexpr float kChipOut  = 86.0f;
constexpr float kChipUp   = 104.0f;
constexpr float kChipFall = 620.0f;

// Share of a standing tree's toughness that the stump left behind still has.
//
// Under half. Clearing a stump is a chore rather than a second tree: the player
// has already paid for this trunk once, and what the second round of swings buys
// is the ground back, plus the last of the wood that was in the root.
constexpr float kStumpShare = 0.4f;

// And the share of the tree's wood that comes out of the stump when it is
// cleared, never less than one piece.
constexpr float kStumpYield = 0.35f;

// The burst of leaves a blow knocks out of a crown: how many, how long they last,
// and how they travel.
//
// A real particle rather than a cell of the drifting field below, and this is the
// one thing that field cannot say — *this* leaf came off *that* tree just now. It
// still keeps no state: a burst is a pure function of the plant's own id, the
// clock the blow was struck on, and the time since, all three of which the record
// already holds. Nothing is spawned and nothing has to be reaped.
//
// The impact of a falling trunk uses the same burst several times over, which is
// what covers the moment the tree goes: the crown that was there is replaced by
// what came off it rather than by nothing.
constexpr int kBurstLeaves = 10;
constexpr float kBurstLife = 0.85f;

// Pixels per second outwards and upwards at the moment of the blow, and the
// gravity that takes them back down. Leaves are light: they barely arc, and what
// they mostly do is come off and flutter.
constexpr float kBurstOut  = 34.0f;
constexpr float kBurstUp   = 26.0f;
constexpr float kBurstFall = 130.0f;

// How far a leaf swings across its own path as it falls, and how fast. The same
// swing the drifting field uses, which is what keeps a knocked-off leaf and a
// shed one the same kind of thing.
constexpr float kBurstSwing = 5.0f;
constexpr float kBurstRate  = 9.0f;

// How much bigger the burst is when the whole tree lands. A blow takes leaves off
// one branch; the ground takes them off all of it.
constexpr int kImpactBurst = 5;

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

// How fast this particular plant goes, against the middle of its species.
//
// The spread Minecraft gets for nothing out of rolling a chance against a random
// tick, and the thing a table of fixed maturities cannot say at all: two saplings
// planted together must not come up together, and a row of them along a fence has
// to arrive as a row of different trees over several minutes rather than as a
// wall appearing at once.
//
// Hashed out of the plant's own cell, so it is the same tree's own pace in every
// session and no two neighbours share one. See flora::kVigourLeast.
float Vigour(std::int64_t cell, int seed) {
    return flora::kVigourLeast + (flora::kVigourMost - flora::kVigourLeast) * Chance(cell, 149, seed);
}

// Seconds this plant takes to come up, under average light and rain.
float Maturing(const flora::SpeciesDef &def, std::int64_t cell, int seed) {
    return std::max(def.growth.maturityMinutes * kMinute / Vigour(cell, seed), 1.0f);
}

// How much of a full-grown tree of its species this plant is, by the size it is
// actually drawn at.
//
// One number standing for "how big is this thing really", and it carries both
// halves of that: which stage it is at, and the plant's own size against its
// species. Both were being ignored by everything that mattered — a sapling and a
// hundred-and-thirty-pixel oak took the same five blows and paid the same four to
// seven wood, and so did a stunted tree two thirds the size of the one beside it.
// What a player sees is the size; what they should get is the size.
//
// Never zero, so a plant is always worth at least a blow and at least a piece.
float Stature(const flora::Plant &plant, flora::Stage stage) {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const float full = def.height[flora::StageIndex(flora::Stage::Mature)];
    const float here = def.height[flora::StageIndex(stage)] * plant.scale;

    return std::clamp(here / std::max(full, 1e-3f), 0.12f, 1.5f);
}

// How fast a plant grows, as a share of the way to maturity per second.
//
// The species names how long it takes under average light and rain, the plant's
// own vigour moves that either way, and the two factors below are written so that
// at the averages they multiply to one — so `maturityMinutes` means what it says,
// and better than average is faster rather than the number being a floor nothing
// reaches.
float Rate(const flora::SpeciesDef &def, std::int64_t cell, int seed, const terrain::Climate &climate, float light,
           float water, float meanLight, float meanWater) {
    const flora::SpeciesGrowth &growth = def.growth;

    const float pace = 1.0f / Maturing(def, cell, seed);

    const auto against = [](float need, float have, float mean) {
        // Below the mean this bites and above it this helps, both in proportion
        // to how much the species cares.
        return 1.0f - need + need * (have / std::max(mean, 1e-3f));
    };

    // And how well the place suits it at all. A tree at the edge of its range is
    // already stunted by the scatter; this is why it also took longer to get
    // there.
    const float suited = std::exp(-std::pow(
        (climate.temperature - def.climate.temperature) / std::max(def.climate.temperatureWidth, 1e-3f), 2.0f));

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
// How far apart the columns are that a leaf can fall down, in world pixels. The
// one lever over how thick the fall is before the season and the weather thin it —
// see kLeafFall in grove.h for the fall itself, which is the tree's own height.
//
// Fine, and it has to be: this is a whole leaf per column at most, so the column
// spacing is the ceiling on how hard a wood can be seen to shed. At four times this
// a storm stripping a birch wood put a dozen leaves over the width of the screen,
// which reads as a few specks of dirt rather than as weather.
constexpr float kLeafCell  = 2.2f;
constexpr float kLeafSwing = 13.0f;
constexpr float kLeafSide  = 0.9f;

// Share of the cells that hold a leaf at all, before the season and the weather
// thin them.
constexpr float kLeafDensity = 0.9f;

// Share of the season's shedding that happens on a dead still day.
//
// The rest of it is the wind's to release. A wood in autumn drops leaves whether
// or not anything is blowing, so this cannot be zero; but what a gust does is
// strip a crown, and at the old arrangement — season alone, wind nowhere in it —
// a gale and a calm afternoon shed exactly the same number of leaves, which is
// the one thing anybody watching a storm would notice was wrong.
//
// A fifth, so the crest of a gale sheds five times what dead air does — and it is
// worth being exact about which two things that ratio is between, now that both of
// them are real. Against the *mean* of a storm rather than its crest it is nearer
// three, and against a clear afternoon rather than dead air nearer two and a half.
// Anything closer to a half here and all of those collapse into the same picture
// with more leaves in it.
constexpr float kCalmShed = 0.10f;

// The tallest crown any species grows, in world pixels.
//
// Walked from the table once rather than written down, and it is the longest fall a
// leaf can have — which is what the sweep upwind of the view has to be sized
// against. Static because the table is compile-time constant and this is asked
// every frame.
float TallestCrown() {
    static const float tallest = [] {
        float found = 1.0f;

        for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
            const flora::SpeciesDef &def = flora::Def(static_cast<flora::Species>(s));

            if (!def.deciduous) continue;

            found = std::max(found, def.height[flora::StageIndex(flora::Stage::Mature)]);
        }

        return found;
    }();

    return tallest;
}

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

    // A share of the hardest this world can blow, so the whole wood is at rest on a
    // still day and near its limit in a storm. A share and not a speed, because how
    // far a tree bends is a fraction of its own height — see Sky::PushAt.
    const float push = sky.PushAt(plant.base.x);

    // Each tree keeps its own phase, so a stand does not beat as one object.
    // Taken from the cell, so a tree sways the same way every time it is met.
    const float phase = static_cast<float>((plant.id * 2654435761LL) & 0xffff) / 65536.0f;

    // Two oscillators at fixed rates, crossfaded by the wind — never one oscillator
    // whose rate the wind sets.
    //
    // This is the difference between modulating a frequency and multiplying by one,
    // and getting it wrong is spectacular. Written `sin(now * rate)` with `rate`
    // varying, the angle's true speed is `rate + now * d(rate)/dt`: the second term
    // carries the *absolute clock* as a factor, so as a session runs on, an ordinary
    // gust rising and falling whips the phase round faster and faster. At an hour of
    // weather time a wood swaying twice a second was vibrating at eighty. It showed
    // up first after F7, which reaches that hour in a minute and a half, and read as
    // the fast-weather animation carrying on after the fast weather stopped — but it
    // was never the toggle. It was the clock.
    //
    // Both rates here are constants, so the angle only ever advances at the rate it
    // says, and the wind changes which of the two is heard rather than how fast
    // either runs.
    const float turn = (now + phase * kSwayPeriod) * 2.0f * 3.14159265f;

    const float easy  = std::sin(turn / kSwayPeriod);
    const float hurry = std::sin(turn / kSwayHurry + phase);

    const float urgency = std::clamp(std::fabs(push) * kSwayUrgency, 0.0f, 1.0f);

    const float swing = easy * (1.0f - urgency) + hurry * urgency;

    // Held over by the wind, and ringing about wherever it is being held. In a gale
    // the hold is several times the quiver, so the tree never crosses upright —
    // which is the one thing a bent tree never does. In still air the hold is
    // nothing and only the quiver is left, so it crosses upright freely, which is
    // the one thing a tree in still air always does.
    const float hold   = kSwayHold * push;
    const float quiver = kSwaySwing * (kSwayIdle + (1.0f - kSwayIdle) * std::sqrt(std::fabs(push))) * swing;

    return height * (hold + quiver);
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

void Grove::Unload() {
    sheet_.Unload();
}

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
    sunk_.assign(static_cast<std::size_t>(count), 0.0f);

    // The skyline and not the surface as built. It is memoised per column, so
    // after one pass over a stretch of world this is a lookup each — and it is
    // the answer that does not move when somebody digs, which is what keeps a
    // wood from rearranging itself around a hole.
    //
    // And, beside it, how far that answer has fallen below the land's own
    // surface. The skyline follows the sky *down* wherever an entrance has opened
    // the ground — which is exactly right for lighting and exactly wrong for
    // planting, since what it reports inside a wide cave mouth is a ledge halfway
    // down a shaft. The two heights agree over ordinary ground and disagree by a
    // long way over a hole, so their difference is the whole of the test.
    for (int i = 0; i < count; i++) {
        const float x = static_cast<float>(first + i) * spacing;

        // The land's own surface and the skyline's scan for it, which are the same
        // place over ordinary ground and a long way apart down a shaft.
        const float land    = terrain::Height(x, world.Settings());
        const float scanned = world.Skyline(first + i);

        // Rounded to the row the ground is actually *drawn* at. A square is filled
        // when its centre is inside, so the drawn surface is the first such row at
        // or below the crossing — up to most of a texel from it, by a different
        // amount in every column. A trunk based on the crossing stands that far off
        // the ground it grew in, and beside a character standing on the drawn
        // surface it is the character that reads as floating.
        //
        // Taken from the *land* and not from the skyline, and that is a second
        // fault on top of the first. The skyline is a scan: it starts at
        // `Height - kSkylineHeadroom` and steps down by the lattice until it is
        // inside the ground, so what it returns is the first sample that was
        // already underground rather than the surface — its own declaration in
        // world.cpp says so and warns that the answer is not on the lattice. With a
        // headroom of sixty-four and a step of six the scan grid sits two pixels
        // below the surface, and rounding *that* to the texel grid carries a good
        // third of all columns down a whole texel further. The result was a wood
        // planted five pixels inside the hill, uniformly, everywhere.
        //
        // Over ordinary ground the crossing is the land's surface exactly — the
        // density field is built as the signed distance from it — so this is not an
        // approximation of the skyline, it is the number the skyline was
        // approximating.
        //
        // The scan is still what answers where it has genuinely fallen: a cave
        // entrance breaking the surface drops it by far more than its own
        // quantisation, and there the ledge down the shaft is the real ground and
        // the land overhead is a roof. One lattice step is the line between the
        // two, because a step is the whole of the error the scan can have.
        const bool shaft = (scanned - land) > spacing;

        surface_[static_cast<std::size_t>(i)] =
            marching_squares::DrawnTop(shaft ? scanned : land, config::kPixelSize);

        // Measured from the scan rather than from the row above, so it keeps saying
        // exactly what it always said — how far the sky reached past the land — and
        // does not start reporting the rounding as a hole.
        sunk_[static_cast<std::size_t>(i)] = scanned - land;
    }

    ground_ = {.top     = surface_.data(),
               .sunk    = sunk_.data(),
               .count   = count,
               .originX = static_cast<float>(first) * spacing,
               .spacing = spacing};
}

void Grove::Update(const World &world, Rectangle view, Vector2 player, float now, float dt, Inventory &into) {
    ReadGround(world, view);

    // Grown a little wider than the view, so a tree coming over the edge has a
    // frame or two to be drawn into the sheet before anybody could see that it
    // was not there yet.
    flora::Scatter(flora::Layer::Canopy, view.x - kLead, view.x + view.width + kLead, settings_, terrain_, ground_,
                   plants_);

    // The hand-planted ones appended before anything walks the list, and that
    // placement is load-bearing rather than tidy. `Scatter` fills `plants_` with
    // what the world grows, which a planted tree is by definition not; everything
    // below — the yield of a felled trunk, the thinning of the undergrowth beneath
    // one — reads `plants_` and would simply not see them. It ran last, so a planted
    // tree that was chopped down was never in the list on the frame its wood was due
    // and gave up nothing, however big it had grown.
    Planted(view);

    // Trees that have just finished going over give up what they were carrying.
    //
    // Done here rather than at the moment of the killing blow, because what the
    // player is owed arrives when the trunk hits the ground — the whole point of
    // drawing the fall is that the reward is at the end of it.
    //
    // And after the trunk has gone rather than at the instant it lands, which is
    // the order the eye can follow: the tree comes down, the tree is not there any
    // more, and the wood is lying where it was. Thrown a tenth of a second earlier
    // the pieces appear underneath a trunk that is still drawn over them, and what
    // reads is wood arriving out of the ground.
    for (const flora::Plant &plant : plants_) {
        const auto found = remembered_.find(plant.id);
        if (found == remembered_.end()) continue;

        TreeState &state = found->second;

        if (state.dropped || state.felledAt < 0.0f || now - state.felledAt < kFallTime + kVanishTime) continue;

        state.dropped = true;

        Yield(plant, state, now);
    }

    // And the floor under them. Grown after the trees, because it asks where they
    // are: nothing takes root inside a trunk, and what grows in the open is not
    // what grows in the shade.
    flora::Scatter(flora::Layer::Undergrowth, view.x - kLead, view.x + view.width + kLead, settings_, terrain_, ground_,
                   undergrowth_);

    Thin();

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

        // The species' own light need, read backwards, which is exactly what it
        // means: needing little light is the same fact as tolerating shade, so a
        // shade plant thickens under a crown where a sun plant thins out.
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

        // Broken and taken away. A planted plant exists only because its record
        // says so, so a cleared one is a plant that is simply not there.
        if (state.cleared) continue;

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

        if (away > 0.0f) {
            state.growth +=
                away * Rate(def, plant.id, settings_.seed, climate, meanLight_, meanWater_, meanLight_, meanWater_);
        }

        if (here > 0.0f) {
            state.growth += here * Rate(def, plant.id, settings_.seed, climate, world.LightLevelAt(plant.base),
                                        world.HumidityAt(plant.base), meanLight_, meanWater_);
        }

        state.growth    = std::min(state.growth, 1.0f);
        state.updatedAt = now;
    }
}

void Grove::Yield(const flora::Plant &plant, const TreeState &state, float now, float share, bool woodOnly) {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    // What comes off a tree is what was in it, and what was in it is how big it
    // was. The table's counts are a full-grown one of its species; everything
    // smaller — a young tree, or a mature one the scatter stunted — pays what its
    // own size is worth. Rounded up further down, so the smallest thing worth
    // felling is still worth a piece.
    share *= Stature(plant, StageOf(state.growth));

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

        // Timber alone, where the caller asked for it. Everything else on the
        // table hung in the crown and came down with it.
        if (woodOnly && rule.item != Item::Wood) continue;

        // The tree's own cell decides, so the same tree gives the same wood in
        // any session. Nothing about a drop is left to the moment it happened in.
        const float roll = Chance(plant.id, static_cast<int>(rule.item) * 71 + 3, settings_.seed);

        if (roll >= rule.chance) continue;

        const float amount = Chance(plant.id, static_cast<int>(rule.item) * 71 + 11, settings_.seed);

        const int whole = rule.least + static_cast<int>(amount * static_cast<float>(rule.most - rule.least + 1));

        // Rounded up, so a share of a small drop is a piece rather than nothing:
        // a stump that gives no wood at all is a stump nobody has a reason to cut.
        const int count = static_cast<int>(std::ceil(static_cast<float>(std::clamp(whole, rule.least, rule.most)) *
                                                     std::clamp(share, 0.0f, 1.0f)));

        if (count <= 0) continue;

        drops_.Scatter(ItemsOf(rule.item, count), from, away, now);
    }
}

void Grove::Draw(const weather::Sky &sky, flora::Season season, float now) const {
    if (!sheet_.Ready()) return;

    const float pixel = config::kFloraPixel;

    sheet_.Begin();

    // The floor first, so a trunk stands in front of the ferns around it.
    for (const flora::Plant &plant : undergrowth_) {
        const canopy::Sprite *sprite = sheet_.Acquire(plant, flora::Stage::Mature, season, Snowy(plant));
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

        // Cut out root and all. Bare ground, and it stays bare: nothing grows here
        // again unless somebody plants it.
        if (standing.cleared) continue;

        // Down: the cut trunk is all that is left, and it stays until the player
        // takes an axe to that too.
        if (standing.stump) {
            DrawStump(plant, standing);
            continue;
        }

        // The stage is part of what a sprite *is*, so it goes into the key: a
        // sapling and the tree it becomes are two drawings, and the sheet keeps
        // whichever is being asked for.
        const canopy::Sprite *sprite = sheet_.Acquire(plant, standing.stage, season, Snowy(plant));

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

            DrawTexturePro(sheet_.Texture(), sprite->source, target, pivot, angle, Fade(WHITE, standing.fade));

            // The stump is under it from the moment it starts to go, so there is
            // never a frame with nothing where the tree was.
            DrawStump(plant, standing);
            continue;
        }

        // How far the top of this plant leans, in world pixels, right now: the
        // wind, plus whatever is left of the last blow.
        const float lean = Lean(sky, plant, now) + standing.shake;

        // A tree still enough to be one quad is drawn as one. Most of a calm wood
        // is, and a strip draw costs a quad per band.
        if (std::fabs(lean) < pixel * 0.5f) {
            DrawTexturePro(sheet_.Texture(), sprite->source,
                           {left, top, sprite->source.width * pixel, sprite->source.height * pixel}, {0.0f, 0.0f}, 0.0f,
                           WHITE);
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

            const Rectangle target = {left + offset, top + y0 * pixel, sprite->source.width * pixel, (y1 - y0) * pixel};

            DrawTexturePro(sheet_.Texture(), source, target, {0.0f, 0.0f}, 0.0f, WHITE);
        }
    }
}

bool Grove::Snowy(const flora::Plant &plant) const {
    return SurfaceCoverAt(plant.base.x, terrain_) == Element::Snow;
}

float Grove::Aged(const flora::Plant &plant, float now) const {
    const float roll = Chance(plant.id, 137, settings_.seed);

    // Most of the wood is grown, and answering so costs one hash.
    if (roll >= kYoungShare) return 1.0f;

    // The rest is somewhere on its way. Where it started is its own — spread
    // evenly over the run above the floor, so a young wood holds every age at
    // once rather than a cohort — and how fast it closes that is its own vigour.
    //
    // A pure function of the cell and the clock, which is the whole reason this
    // can exist at all: an untouched wood of mixed ages keeps no records, exactly
    // as an untouched wood of mature trees keeps none. The moment the player
    // touches one, Remember seeds its record from this and Ripen takes over
    // against the light and the rain actually falling on it.
    const float from = kYoungFloor + (1.0f - kYoungFloor) * (roll / kYoungShare);

    const flora::SpeciesDef &def = flora::Def(plant.species);

    return std::min(from + std::max(now, 0.0f) / Maturing(def, plant.id, settings_.seed), 1.0f);
}

Grove::Standing Grove::Read(const flora::Plant &plant, float now) const {
    Standing standing;

    const auto found = remembered_.find(plant.id);

    // Nothing has happened to it, so the world answers: undamaged, unfelled, and
    // as far along as its own cell and the clock say it is.
    if (found == remembered_.end()) {
        standing.stage = StageOf(Aged(plant, now));

        return standing;
    }

    const TreeState &state = found->second;

    standing.stage = StageOf(state.growth);
    standing.wear  = std::clamp(1.0f - state.stumpHealth, 0.0f, 1.0f);

    if (state.struckAt >= 0.0f) standing.struck = now - state.struckAt;

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
        standing.stump    = since > kFallTime + kVanishTime;
        standing.cleared  = state.cleared;

        // Gone on impact, over the few frames kVanishTime allows. The trunk is
        // whole for the whole of the turn and then it is not there, which is the
        // moment the burst of leaves is thrown into.
        const float down = since - kFallTime;

        standing.fade = (down <= 0.0f) ? 1.0f : std::clamp(1.0f - down / kVanishTime, 0.0f, 1.0f);
    }

    return standing;
}

void Grove::Blow(TreeState &state, float now) const {
    state.struckAt = now;

    // Into the next slot of the ring rather than over the last one, so the leaves
    // this blow knocks loose join whatever the one before it left in the air.
    state.blowSlot                = (state.blowSlot + 1) % kBlows;
    state.blowAt[state.blowSlot] = now;
}

Grove::TreeState &Grove::Remember(const flora::Plant &plant, float now) {
    auto found = remembered_.find(plant.id);
    if (found != remembered_.end()) return found->second;

    TreeState fresh;
    fresh.updatedAt = now;
    fresh.species   = static_cast<std::uint8_t>(flora::SpeciesIndex(plant.species));

    // Seeded from what the world was already saying about it, or a young tree
    // would jump to full size on the frame somebody first swung at it. From here
    // on Ripen carries it, against the light and the rain that are actually
    // falling rather than against the averages Aged assumes — which is the whole
    // difference between a plant nobody is watching and one somebody is.
    fresh.growth = Aged(plant, now);

    return remembered_.emplace(plant.id, fresh).first->second;
}

void Grove::Forget(float now) {
    for (auto it = remembered_.begin(); it != remembered_.end();) {
        const TreeState &state = it->second;

        // A planted one is the overlay's own assertion and can never be dropped
        // while it is standing: the procedural pass does not know it is there.
        //
        // Once it has been cleared, though, the assertion is that nothing is
        // there — and nothing is what an absent record already says. So this is
        // the one felled plant whose record does go, and it has to: a player who
        // plants and breaks the same sapling a hundred times would otherwise leave
        // a hundred entries behind saying nothing.
        if (state.planted) {
            it = state.cleared ? remembered_.erase(it) : std::next(it);
            continue;
        }

        // Felled, and kept for good — whether the stump is still standing or has
        // been cut out after it.
        //
        // This used to expire, and the tree grew back in the same cell. It was the
        // one place the overlay could shed a record without lying, and it was
        // lying anyway: a tree that comes back where it was cut is not something
        // that happens, and a wood that repairs itself behind the player is a wood
        // that cannot be cleared. Stardew Valley is the reference and it is
        // unambiguous — what you cut stays cut, and what grows again grows from a
        // seed somebody planted.
        //
        // So this is the one record that never goes, and it is the same bargain
        // World::edits_ makes for the same reason: a plant with no record is a
        // mature tree, so the fact that a cell is empty can only be held by
        // keeping something in it. The count is on screen beside the edits.
        if (state.felledAt >= 0.0f) {
            ++it;
            continue;
        }

        // Undamaged, unfelled, unpicked and fully grown is exactly what a plant
        // with no record is, so the record is worth nothing and goes.
        const bool quiet = state.health >= 1.0f && state.growth >= 1.0f && state.fruitAt < 0.0f;

        it = quiet ? remembered_.erase(it) : std::next(it);
    }
}

std::optional<Rectangle> Grove::StrikeRect(const flora::Plant &plant, float now) const {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const Standing standing = Read(plant, now);

    // Nothing there at all.
    if (standing.cleared) return std::nullopt;

    // The stump left where a tree came down, which is the second half of the job
    // and the reason the wood is not simply gone when the tree is. Taken before the
    // guard below, because to that guard a stump is a felled tree and a felled tree
    // is not something to swing at.
    if (standing.stump) return StumpRect(plant, standing.stage);

    // Going over. Nothing to swing at while it does.
    if (standing.felling >= 0.0f) return std::nullopt;

    // The size it actually is, rather than the size it will be.
    //
    // This read the mature row whatever stage the plant was at, so a sapling
    // ankle-high on the ground carried an oak's hitbox: a swing anywhere in the
    // fifty pixels of empty air above it connected, and connected with a trunk that
    // was not there.
    const std::size_t grown = flora::StageIndex(standing.stage);

    const float height = def.height[grown] * plant.scale;
    const float width  = def.canopyWidth[grown] * plant.scale;

    // A sapling is the whole of itself, being too small to have a trunk apart from
    // its leaves.
    if (standing.stage == flora::Stage::Sapling) {
        const float reach = std::max(width * 0.5f, kStrikeSlack);

        return Rectangle{plant.base.x - reach, plant.base.y - height, reach * 2.0f, height};
    }

    // The trunk alone. Swinging at the crown of a tree twenty pixels over your head
    // should not fell it.
    const float half = std::max(width * def.shape.trunkWidth * 0.5f, 1.0f) + kStrikeSlack;

    return Rectangle{plant.base.x - half, plant.base.y - height * def.shape.clearance, half * 2.0f,
                     height * def.shape.clearance};
}

void Grove::DrawCollision(flora::Season season, float now) const {
    const float pixel = config::kFloraPixel;

    // The undergrowth first, under the trees, in the order Draw uses — an overlay
    // laid out differently from the thing it is about is its own puzzle.
    for (const flora::Plant &plant : undergrowth_) {
        const canopy::Sprite *sprite = sheet_.Acquire(plant, flora::Stage::Mature, season, Snowy(plant));
        if (sprite == nullptr) continue;

        DrawRectangleLinesEx({plant.base.x - sprite->anchor.x * pixel, plant.base.y - sprite->anchor.y * pixel,
                              sprite->source.width * pixel, sprite->source.height * pixel},
                             1.0f, Fade(SKYBLUE, 0.35f));
    }

    for (const flora::Plant &plant : plants_) {
        const Standing standing = Read(plant, now);
        if (standing.cleared) continue;

        // Where the picture goes. Taken from the sprite that is actually in the
        // sheet rather than from the table, since the whole point is to show what
        // was drawn and not what was meant to be.
        const canopy::Sprite *sprite = sheet_.Acquire(plant, standing.stage, season, Snowy(plant));

        if (sprite != nullptr) {
            DrawRectangleLinesEx({plant.base.x - sprite->anchor.x * pixel, plant.base.y - sprite->anchor.y * pixel,
                                  sprite->source.width * pixel, sprite->source.height * pixel},
                                 1.0f, Fade(SKYBLUE, 0.7f));
        }

        // Where the axe goes.
        const std::optional<Rectangle> strike = StrikeRect(plant, now);
        if (strike.has_value()) DrawRectangleLinesEx(*strike, 1.0f, Fade(RED, 0.9f));

        // And the seat itself, as a cross rather than a dot: what matters about it
        // is the two lines, since a trunk that is off by a pixel is off along one
        // of them and a dot hides which.
        DrawLineV({plant.base.x - 6.0f, plant.base.y}, {plant.base.x + 6.0f, plant.base.y}, MAGENTA);
        DrawLineV({plant.base.x, plant.base.y - 6.0f}, {plant.base.x, plant.base.y + 6.0f}, MAGENTA);
    }
}

bool Grove::TimberAt(Rectangle probe, float now) const {
    for (const flora::Plant &plant : plants_) {
        const std::optional<Rectangle> box = StrikeRect(plant, now);

        if (box.has_value() && CheckCollisionRecs(probe, *box)) return true;
    }

    return false;
}

void Grove::Strike(Rectangle hitbox, float damage, Vector2 from, float now) {
    for (const flora::Plant &plant : plants_) {
        const std::optional<Rectangle> box = StrikeRect(plant, now);

        // The one place the geometry of a swing lives. What may be hit and what a
        // hit does are two questions, and the cursor has to ask the first without
        // answering the second — a second copy of these rectangles would be a
        // cursor that lights up on trees this cannot fell.
        if (!box.has_value() || !CheckCollisionRecs(hitbox, *box)) continue;

        const flora::SpeciesDef &def = flora::Def(plant.species);

        const Standing standing = Read(plant, now);

        // How much tree there is to cut through. See Stature: a plant that is
        // drawn small has to *be* small, and this is the half of that the axe
        // feels.
        const float stature = Stature(plant, standing.stage);

        if (standing.stump) {
            TreeState &state = Remember(plant, now);

            Blow(state, now);
            state.stumpHealth -= damage / std::max(def.growth.toughness * stature * kStumpShare, 0.5f);

            if (state.stumpHealth > 0.0f) return;

            state.cleared = true;

            // Thrown away from this swing rather than along the line the tree
            // went down: the player has walked round the stump since, and what
            // decides which way a chip flies is where the axe came from now.
            state.fallLeft = from.x > plant.base.x;

            // And the last of the wood, out of the root.
            Yield(plant, state, now, kStumpYield, true);

            return;
        }

        // On its way over. A tree in the air is not something an axe can reach,
        // and a second blow on one would only start its fall again.
        if (standing.felling >= 0.0f) continue;

        // Only for where the sapling's seed is thrown from; the geometry of the
        // blow itself was settled by StrikeRect above.
        const std::size_t grown = flora::StageIndex(standing.stage);

        const float height = def.height[grown] * plant.scale;

        // A sapling is not a tree and does not come down like one.
        //
        // One blow, no fall, no stump, and the seed back in the hand that put it
        // there — which is what Minecraft does and the only thing that makes sense
        // of planting one by mistake. Felling it instead spent a second and a half
        // on an animation of a twig going over and then paid out a mature tree's
        // worth of timber for it.
        if (standing.stage == flora::Stage::Sapling) {
            TreeState &state = Remember(plant, now);

            // Straight to gone: nothing was left standing, so there is no stump to
            // clear afterwards and nothing for the fall to drop.
            state.felledAt = now;
            state.cleared  = true;
            state.dropped  = true;
            state.fallLeft = from.x > plant.base.x;

            drops_.Scatter(ItemsOf(def.sapling, 1), {plant.base.x, plant.base.y - height * 0.5f},
                           state.fallLeft ? -1.0f : 1.0f, now);

            return;
        }

        TreeState &state = Remember(plant, now);

        Blow(state, now);
        state.health -= damage / std::max(def.growth.toughness * stature, 0.5f);

        if (state.health > 0.0f) return;

        state.felledAt = now;

        // Away from whoever swung. A tree that fell towards the axe would be the
        // one thing in the world actively trying to land on the player.
        state.fallLeft = from.x > plant.base.x;

        return;
    }
}


bool Grove::Plant(flora::Species species, Vector2 foot, float now) {
    // A planted tree is not part of the scatter, and so is not bound by the
    // scatter's one-per-cell rule.
    //
    // That rule is what makes a wood cost nothing — every procedural plant is a
    // pure function of its cell index, and a cell grows one. But a planted tree
    // is the single plant the procedural pass knows nothing about: it carries its
    // own position rather than deriving one, and its record is the only evidence
    // it exists at all. Keeping it inside the cell space bought nothing and cost
    // the player a hundred and ten pixels between saplings, which is the width of
    // an oak's canopy and not a rule anybody asked for.
    //
    // Nor could that be tuned away. The span has to be at least the widest canopy
    // in the layer or the scatter's bound stops holding, and at a hundred and ten
    // against an oak's hundred and five it is already as low as it goes.
    //
    // So the spacing here is its own, it is small, and it is only about trunks
    // standing on top of one another.
    for (const flora::Plant &standing : plants_) {
        if (std::fabs(standing.base.x - foot.x) >= kPlantApart) continue;

        // Read before believing it. `plants_` is what the world *would* grow
        // rather than what is standing, so a felled tree is still in it — and the
        // stump of one is the commonest place a player puts a sapling.
        const Standing state = Read(standing, now);
        if (state.felling >= 0.0f || state.stump) continue;

        return false;
    }

    TreeState fresh;

    fresh.plantedAt = now;
    fresh.updatedAt = now;
    fresh.growth    = 0.0f;
    fresh.planted   = true;
    fresh.species   = static_cast<std::uint8_t>(flora::SpeciesIndex(species));

    // Exactly where the player asked, seated the way every other plant in the
    // world is seated — see the scatter, and the half step it no longer adds.
    fresh.at = {foot.x, foot.y};

    remembered_.emplace(kPlantedBase + nextPlanted_++, fresh);

    return true;
}

void Grove::DrawGhost(flora::Species species, Vector2 foot, flora::Season season, Color tint) const {
    if (!sheet_.Ready()) return;

    flora::Plant ghost;

    ghost.id      = kGhostBase + static_cast<std::int64_t>(flora::SpeciesIndex(species));
    ghost.species = species;
    ghost.scale   = 1.0f;

    // Seated the way every other plant in the world is — see the scatter, and
    // Plant, which seats a sapling the same way. The ghost has to stand exactly
    // where the sapling will, or it is a promise about somewhere else.
    ghost.base = {foot.x, foot.y};

    const canopy::Sprite *sprite = sheet_.Acquire(ghost, flora::Stage::Sapling, season, false);

    // The frame's drawing budget is spent and this is the first hover. It will be
    // there next frame; a marker drawn in its place would only be a second answer
    // to the question the ghost is already answering.
    if (sprite == nullptr) return;

    const float pixel = config::kFloraPixel;

    DrawTexturePro(sheet_.Texture(), sprite->source,
                   {Snap(ghost.base.x - sprite->anchor.x * pixel), Snap(ghost.base.y - sprite->anchor.y * pixel),
                    sprite->source.width * pixel, sprite->source.height * pixel},
                   {0.0f, 0.0f}, 0.0f, Fade(tint, kGhostFade));
}

Rectangle Grove::StumpRect(const flora::Plant &plant, flora::Stage stage) const {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const std::size_t grown = flora::StageIndex(stage);

    const float width  = def.canopyWidth[grown] * plant.scale;
    const float height = def.height[grown] * plant.scale;

    const float half = std::max(width * def.shape.trunkWidth * 0.5f * kStumpWiden, config::kFloraPixel);
    const float tall = std::clamp(height * kStumpHeight, kStumpFloor, kStumpCeiling);

    // Snapped, because it is what a rectangle is drawn from and what an axe is
    // tested against, and the two have to be the same rectangle to the pixel.
    const float left = Snap(plant.base.x - half);
    const float top  = Snap(plant.base.y - tall);

    return {left, top, Snap(plant.base.x + half) - left, Snap(plant.base.y) - top};
}

void Grove::DrawStump(const flora::Plant &plant, const Standing &standing) const {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const flora::SpeciesPalette &palette = def.palette[flora::SeasonIndex(flora::Season::Summer)];

    const Rectangle stump = StumpRect(plant, standing.stage);

    const float pixel = config::kFloraPixel;

    const int columns = std::max(static_cast<int>(std::round(stump.width / pixel)), 1);
    const int rows    = std::max(static_cast<int>(std::round(stump.height / pixel)), 1);

    // What the last blow is still doing to it: a jolt sideways, dying away, and
    // for the first two frames a cut face gone pale where the axe went in.
    //
    // Snapped to a whole texel, because the whole stump moves together and a
    // fraction of one reads as the thing blurring rather than as it being hit.
    float jolt   = 0.0f;
    bool struck  = false;

    if (standing.struck >= 0.0f && standing.struck < kJoltTime) {
        jolt = std::round(kJolt * std::exp(-kJoltDecay * standing.struck) * std::sin(kJoltRate * standing.struck) /
                          pixel) *
               pixel;

        struck = standing.struck < kJoltFlash;
    }

    // And what every blow so far adds up to: the axe eats into the top of it,
    // column by column and unevenly, so a player halfway through a stump can see
    // that they are halfway through it.
    const float bite = std::clamp(standing.wear, 0.0f, 1.0f);

    // How many rows the cut face takes. Two, so the ring inside it has somewhere
    // to sit; on a stump too short for that, one.
    const int face = (rows >= 5) ? 2 : 1;

    // Drawn texel by texel rather than as two rectangles.
    //
    // It was two flat rectangles, and at the size a stump used to be that was
    // nearly defensible. At this size it is a brown brick sitting on the grass:
    // the thing left where a tree stood is now something the player walks up to
    // and swings at, so it has to carry what everything else in the wood does —
    // a lit side, a shaded one, the grain between them, and an edge where the cut
    // is. Twelve by nine texels, which is a hundred squares.
    for (int i = 0; i < columns; i++) {
        // Across the stump, in [0,1], and then to the same axis the trunk's own
        // bark is shaded on so the light lands the same way on both.
        const float across = (static_cast<float>(i) + 0.5f) / static_cast<float>(columns) * 2.0f - 1.0f;

        // How many texels the axe has taken off this column. Its own share of the
        // bite, so the top goes ragged rather than sinking level.
        const int gone = static_cast<int>(bite * kStumpChew *
                                          (0.35f + 0.65f * Chance(plant.id, 600 + i * 23, settings_.seed)));

        for (int j = gone; j < rows; j++) {
            Color colour = (across < -0.15f) ? palette.barkLight : (across < 0.45f ? palette.bark : palette.barkDark);

            if (j < face + gone) {
                // The cut face: raw wood, which is the palest thing on a stump and
                // the only part of it anybody looks at. Ringed rather than flat —
                // the heartwood is darker than the sapwood around it, and that ring
                // is what says the trunk was cut through rather than worn down.
                const bool heart = std::fabs(across) < 0.42f;

                colour = heart ? palette.bark : palette.barkLight;

                // And a nick out of the rim here and there, so the cut is a saw
                // line and not a ruled one.
                if (Chance(plant.id, 400 + i * 7 + j * 3, settings_.seed) > 0.82f) colour = palette.barkDark;

                // Raw where the axe just went in. Two frames of the palest wood
                // the species has, across the whole face — the bite, not a state.
                if (struck) colour = palette.barkLight;
            } else {
                // Bark, with the same two marks the trunk carries: a scatter of
                // darker texels and the occasional notch. Taken from the plant's
                // own id, so a stump looks the same every time it is met.
                if (Chance(plant.id, 200 + i * 13 + j * 5, settings_.seed) > 0.72f) {
                    colour = (across < 0.45f) ? palette.bark : palette.barkDark;
                }

                if (Chance(plant.id, 300 + i * 5 + j * 11, settings_.seed) > 0.88f) colour = palette.barkDark;
            }

            // The foot, where it goes into the ground. Dark all the way across,
            // which is the contact shadow a thing standing on soil has and the
            // difference between a stump in the grass and one drawn over it.
            if (j == rows - 1) colour = palette.barkDark;

            // The jolt fades out towards the foot: a stump is held by its roots,
            // so what a blow moves is the top of it.
            const float held = 1.0f - static_cast<float>(j) / static_cast<float>(rows);

            DrawRectangleV({stump.x + static_cast<float>(i) * pixel + jolt * held,
                            stump.y + static_cast<float>(j) * pixel},
                           {pixel, pixel}, colour);
        }
    }
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
        //
        // And the crown it actually has, not the one it will have. A sapling
        // casting a full oak's shade darkens a strip of ground a hundred pixels
        // wide from under a plant the size of a boot.
        const std::size_t grown = flora::StageIndex(standing.stage);

        const float height = def.height[grown] * plant.scale;
        const float width  = def.canopyWidth[grown] * plant.scale;

        const float foot = height * def.shape.clearance;

        (void)height;
        (void)foot;

        world.AddCover(plant.base.x - width * 0.5f, plant.base.x + width * 0.5f, kCanopyShade);
    }
}

void Grove::DrawFruit(const weather::Sky &sky, flora::Season season, float now) const {
    // Fruit sets in spring and is worth picking through the summer. Outside that
    // window a tree that bears is just a tree.
    const float ripe = (season == flora::Season::Summer) ? 1.0f : (season == flora::Season::Spring) ? 0.35f : 0.0f;

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

        // Fixed to the tree means fixed to the branch, and a branch moves. The same
        // lean the trunk is drawn with, including whatever a blow left ringing in
        // it, or an apple hangs dead still in the air while the whole crown it is
        // attached to bends away underneath it.
        const float lean = Lean(sky, plant, now) + standing.shake;

        for (int i = 0; i < count; i++) {
            const float ax = (Chance(plant.id, 67 + i * 3, settings_.seed) - 0.5f) * width * 0.72f;
            const float ay = foot + Chance(plant.id, 71 + i * 3, settings_.seed) * (height - foot) * 0.8f;

            // Squared in how high it hangs, the same profile the sprite's own bands
            // are sheared by — see Grove::Draw. Anything else and the fruit drifts
            // out of the crown it is supposed to be hanging in.
            const float up    = ay / std::max(height, 1e-3f);
            const float bough = lean * up * up;

            DrawRectangleV({Snap(plant.base.x + ax + bough), Snap(plant.base.y - ay)}, {pixel, pixel},
                           Def(Item::Apple).colour);
        }
    }
}

void Grove::DrawLeaves(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const {
    DrawDrift(sky, season, view, now);
    DrawBurst(sky, season, now);
}

void Grove::Spray(const flora::Plant &plant, flora::Season season, const Burst &burst) const {
    const flora::SpeciesDef &def = flora::Def(plant.species);

    const std::size_t mature = flora::StageIndex(flora::Stage::Mature);

    const float height = def.height[mature] * plant.scale;
    const float width  = def.canopyWidth[mature] * plant.scale;

    // The crown alone, which is what sheds. Nothing comes off the bare trunk
    // under it however hard it is hit.
    const float foot = -height * def.shape.clearance;
    const float top  = -height;

    // Where the crown is now, as the turn the fall left it at. The leaves are
    // hung on the tree and then the tree is put where it went, rather than each
    // leaf being placed in the world twice.
    const float turn = burst.angle * 3.14159265f / 180.0f;

    const float sine   = std::sin(turn);
    const float cosine = std::cos(turn);

    const flora::SpeciesPalette &palette = def.palette[flora::SeasonIndex(season)];

    const float pixel = config::kFloraPixel;

    for (int leaf = 0; leaf < kBurstLeaves * burst.rounds; leaf++) {
        const int salt = burst.salt + leaf * 17;

        // Staggered, so a burst comes off over a few frames rather than as one
        // ring leaving the crown together.
        const float age = burst.since - Chance(plant.id, salt + 1, settings_.seed) * 0.14f;
        if (age <= 0.0f || age >= kBurstLife) continue;

        // Where on the crown it grew, before the tree was turned.
        const float ax = (Chance(plant.id, salt + 2, settings_.seed) - 0.5f) * width * 0.86f;
        const float ay = foot + Chance(plant.id, salt + 3, settings_.seed) * (top - foot);

        const Vector2 from = {plant.base.x + ax * cosine - ay * sine, plant.base.y + ax * sine + ay * cosine};

        // Outwards from the trunk rather than in a random direction: what a blow
        // does is knock leaves off the crown, and each one leaves by its own side
        // of it.
        const float side = (ax >= 0.0f) ? 1.0f : -1.0f;

        const float out = kBurstOut * burst.vigour * (0.35f + 0.65f * Chance(plant.id, salt + 4, settings_.seed));
        const float up  = kBurstUp * burst.vigour * (0.25f + 0.75f * Chance(plant.id, salt + 5, settings_.seed));

        // Thrown, and then gravity. Light enough that the arc is shallow and most
        // of what is seen is the flutter across it.
        const float swing =
            std::sin(age * kBurstRate + Chance(plant.id, salt + 6, settings_.seed) * 6.28318f) * kBurstSwing;

        // And the air it is falling through, by the one rule everything loose in
        // this world is carried by. The air as it stood when the blow landed, which
        // is what `burst.wind` holds — a burst is over in under a second, so what
        // matters is that the whole of it is carried by one wind rather than by a
        // figure sliding under it frame by frame.
        const float carried = weather::Carry(burst.wind, age / kBurstLife, kBurstLife, weather::kLeafDrag);

        const float x = from.x + side * out * age + swing + carried;
        const float y = from.y - up * age + 0.5f * kBurstFall * age * age;

        // Never below the foot of the tree, so a leaf settles on the ground it
        // came off instead of sinking through the hill.
        if (y > plant.base.y) continue;

        // Faded out over the last third, so a burst thins rather than being
        // switched off.
        const float fade = std::clamp((1.0f - age / kBurstLife) * 3.0f, 0.0f, 1.0f);

        // Two texels, turned with the swing — the same leaf the drifting field
        // draws, because it is the same leaf.
        const float lead = Snap(x);
        const float over = Snap(y);

        DrawRectangleV({lead, over}, {pixel, pixel}, Fade(palette.leaf[3], fade));
        DrawRectangleV({lead + (swing >= 0.0f ? pixel : -pixel), over + pixel}, {pixel, pixel},
                       Fade(palette.leaf[1], fade));
    }
}

void Grove::Chips(const flora::Plant &plant, Vector2 at, float since, int salt, float wind) const {
    if (since <= 0.0f || since >= kChipLife) return;

    const flora::SpeciesPalette &palette = flora::Def(plant.species).palette[flora::SeasonIndex(flora::Season::Summer)];

    const float pixel = config::kFloraPixel;

    for (int chip = 0; chip < kChipCount; chip++) {
        const int seed = salt + chip * 29;

        // Out to both sides rather than away from the swing. An axe biting into
        // wood throws splinters off the near face and the far one alike, and a
        // spray that only ever goes one way reads as the stump leaking.
        const float side = (Chance(plant.id, seed + 1, settings_.seed) < 0.5f) ? -1.0f : 1.0f;

        const float out = kChipOut * (0.30f + 0.70f * Chance(plant.id, seed + 2, settings_.seed));
        const float up  = kChipUp * (0.35f + 0.65f * Chance(plant.id, seed + 3, settings_.seed));

        // Along the cut rather than from one point, so the spray has the width of
        // the thing it came out of.
        const float from = (Chance(plant.id, seed + 4, settings_.seed) - 0.5f) * pixel * 4.0f;

        // Blown, like everything else, and barely: a chip of wood is heavy and
        // the gale that carries a leaf across a clearing nudges one of these.
        const float x =
            at.x + from + side * out * since + weather::Carry(wind, since / kChipLife, kChipLife, weather::kChipDrag);
        const float y = at.y - up * since + 0.5f * kChipFall * since * since;

        if (y > at.y + pixel) continue;

        const float fade = std::clamp((1.0f - since / kChipLife) * 2.2f, 0.0f, 1.0f);

        // One texel of pale sapwood, which is what the inside of a trunk is and
        // what makes a chip read as wood rather than as bark falling off.
        DrawRectangleV({Snap(x), Snap(y)}, {pixel, pixel},
                       Fade((chip % 3 == 0) ? palette.bark : palette.barkLight, fade));
    }
}

void Grove::DrawBurst(const weather::Sky &sky, flora::Season season, float now) const {
    // Past this there is nothing left of any burst to draw, and it is worth
    // testing before the work rather than inside it: the record holds the last
    // blow for as long as the wound takes to heal, which is minutes.
    const float spent = kBurstLife + 0.2f;

    for (const flora::Plant &plant : plants_) {
        const auto found = remembered_.find(plant.id);
        if (found == remembered_.end()) continue;

        const TreeState &state = found->second;

        if (state.felledAt < 0.0f) {
            // Every blow still in the air, and not merely the last of them.
            //
            // One timestamp meant one burst, and a second swing landed on top of
            // the first: the leaves already falling were put back in the crown and
            // thrown again. Three slots, three bursts, and a blow adds to what is
            // falling instead of replacing it.
            for (int slot = 0; slot < kBlows; slot++) {
                const float blow = state.blowAt[slot];
                if (blow < 0.0f || now - blow >= spent) continue;

                // Each slot salted apart, or two overlapping bursts would be the
                // same ten leaves drawn twice.
                //
                // The wind as it stood when this blow landed, and each slot gets its
                // own reading. Two bursts a second apart were thrown into different
                // air, and a gust arriving between them should carry the second one
                // further — sampling once for the tree would put both in whatever is
                // blowing now and quietly drag the older one sideways with it.
                Spray(plant, season,
                      {.since = now - blow, .salt = 500 + slot * 131, .wind = sky.WindAt(plant.base.x, blow)});
            }

            // And the chips out of the trunk, halfway up the clear stretch of it,
            // which is where a person swings.
            if (state.struckAt >= 0.0f) {
                const flora::SpeciesDef &def = flora::Def(plant.species);

                const float height = def.height[flora::StageIndex(flora::Stage::Mature)] * plant.scale;

                Chips(plant, {plant.base.x, plant.base.y - height * def.shape.clearance * 0.5f}, now - state.struckAt,
                      700, sky.WindAt(plant.base.x, state.struckAt));
            }

            continue;
        }

        // The stump, taking the second half of the job. No crown left to shed, so
        // what comes off it is wood.
        if (now - state.felledAt > kFallTime + kVanishTime) {
            if (state.struckAt >= 0.0f && !state.cleared) {
                const Rectangle stump = StumpRect(plant, StageOf(state.growth));

                Chips(plant, {stump.x + stump.width * 0.5f, stump.y}, now - state.struckAt, 800,
                      sky.WindAt(plant.base.x, state.struckAt));
            }

            continue;
        }

        // And what the ground knocked out of it, at the moment the crown arrived.
        //
        // This is what carries the tree going. The trunk is drawn for the whole of
        // its turn and is then gone within a tenth of a second, and left on its
        // own that reads as a tree being switched off rather than as one landing.
        // Five bursts over and thrown twice as hard, because what hit the ground
        // was the whole crown and not one branch of it.
        const float landed = now - state.felledAt - kFallTime;

        if (landed <= 0.0f || landed >= spent) continue;

        Spray(plant, season,
              {.since  = landed,
               .rounds = kImpactBurst,
               .salt   = 900,
               .angle  = kFallAngle * (state.fallLeft ? -1.0f : 1.0f),
               .vigour = 2.0f,
               .wind   = sky.WindAt(plant.base.x, state.felledAt + kFallTime)});
    }
}

void Grove::DrawDrift(const weather::Sky &sky, flora::Season season, Rectangle view, float now) const {
    // What the season is worth to a leaf, and it is two figures rather than one
    // because a season answers two different questions.
    //
    // `shedding` is what comes down of its own accord. That is the one autumn owns:
    // a wood in October drops leaves in dead air because the leaves are letting go
    // anyway, and in high summer almost nothing falls on a still day.
    //
    // `stripping` is what the wind can tear off, and it is nearly flat across the
    // year, because what it depends on is whether there is a crown full of leaves to
    // tear at — which there is in spring and summer just as much as in autumn. A
    // little under autumn's, since a leaf that is already letting go needs less
    // pulling than one still firmly on.
    //
    // Scaling both by one seasonal figure was wrong and read as wrong: a summer gale
    // stripped half of what an autumn gale did, so the field looked like something
    // that only happened in autumn. The wind now works whenever there are leaves,
    // which is every season but the bare one.
    const float shedding = (season == flora::Season::Autumn)   ? 1.00f
                           : (season == flora::Season::Spring) ? 0.35f
                           : (season == flora::Season::Winter) ? 0.0f
                                                               : 0.25f;

    const float stripping = (season == flora::Season::Autumn)   ? 1.00f
                            : (season == flora::Season::Spring) ? 0.85f
                            : (season == flora::Season::Winter) ? 0.0f
                                                                : 0.80f;

    // Zeroed before the early return below and not after it, or a bare winter wood
    // reports whatever the last leafy frame counted. Which it did.
    drifting_ = 0;

    // Winter, and nothing else. A deciduous crown stands bare, so there is nothing
    // for even a gale to take off it.
    if (stripping <= 0.0f || plants_.empty()) return;

    // The most this season can ever loosen, whatever the weather does. What the
    // cheap rejection below is measured against.
    const float utmost = kCalmShed * shedding + (1.0f - kCalmShed) * stripping;

    const float pixel = config::kFloraPixel;

    // How far upwind of the view a leaf can be let go of and still blow into it.
    //
    // Without this the field is only ever emptied by the wind: a leaf near the edge
    // is carried out of sight, and the one that should have arrived from the other
    // side was never generated, because the sweep stopped at the view. In a gale
    // that swept the upwind edge of the screen clean — hundreds of pixels of wood
    // with a bare sky beside it — which is one of the ways this looked like the
    // wind doing nothing.
    //
    // Sized from the gale rather than from the wind now blowing, for the reason
    // every buffer in this project is: it is a span to be swept, and a span that
    // tracks the current weather runs out the moment the weather changes.
    // Downwind only. A leaf is carried the way the air is going, so the strip that
    // can feed the view lies on one side of it and sweeping the other costs cells
    // and finds nothing. The mean rather than the gust, because this is a span to be
    // swept and it wants one answer for the whole of it.
    const float slowest = kLeafFall * 0.7f;
    const float blown   = sky.Gale() * (TallestCrown() / slowest) * 0.5f * weather::kLeafDrag;

    const float upwind = (sky.Mean() >= 0.0f) ? blown : 0.0f;
    const float lee    = (sky.Mean() >= 0.0f) ? 0.0f : blown;

    const int first = static_cast<int>(std::floor((view.x - upwind) / kLeafCell)) - 1;
    const int last  = static_cast<int>(std::ceil((view.x + view.width + lee) / kLeafCell)) + 1;

    // The wood, gathered once.
    //
    // Every cell has to find the tree standing over it, and the sweep is now several
    // times the width of the view — so asking the whole plant list, with a record
    // lookup on each, once per cell was most of the cost of the field. What a cell
    // actually needs is four floats, and they are the same four for every cell under
    // the same tree.
    //
    // Standing, and only standing. A felled tree stays in `plants_` — the procedural
    // pass still says a tree grows in that cell and only the record says it came
    // down — so without this the leaves went on falling out of thin air where the
    // tree had been. The third place this has caught me now, after the shade and the
    // fruit. **Anything that walks `plants_` and does something with a tree has to
    // ask `Read` first**; the list is what the world would grow, not what is there.
    shedders_.clear();

    for (const flora::Plant &plant : plants_) {
        const flora::SpeciesDef &def = flora::Def(plant.species);
        if (!def.deciduous) continue;

        const Standing standing = Read(plant, now);
        if (standing.felling >= 0.0f || standing.stump) continue;

        // The crown it actually has. A sapling was shedding a mature tree's worth
        // from a mature tree's height.
        const std::size_t stage = flora::StageIndex(standing.stage);

        shedders_.push_back({plant.base.x, def.canopyWidth[stage] * plant.scale * 0.5f, plant.base.y,
                             def.height[stage] * plant.scale, &def});
    }

    if (shedders_.empty()) return;

    for (int cell = first; cell <= last; cell++) {
        const float column = (static_cast<float>(cell) + Chance(cell, 17, settings_.seed)) * kLeafCell;

        // Cells this season will never shed from at all, rejected before the wood is
        // searched. The full test below needs the wind at the moment of release, and
        // that needs the fall, and the fall is the tree's own height — so the tree
        // has to be found first. What this can settle without it is the cells no
        // weather could ever loosen, since `loosened` is at most this season's utmost.
        if (Chance(cell, 13, settings_.seed) > kLeafDensity * utmost) continue;

        // Only where something is standing to have shed it. A leaf falling in an
        // open field is the one thing that would give the trick away.
        const Shedder *under = nullptr;

        for (const Shedder &tree : shedders_) {
            if (std::fabs(tree.at - column) > tree.reach) continue;

            under = &tree;
            break;
        }

        if (under == nullptr) continue;

        const flora::SpeciesDef &def = *under->def;

        const float height = under->height;

        // Leaves the crown and wraps back to it, so the fall runs for ever without
        // anything having to be respawned.
        //
        // Over the tree's own height, and this is the correction that made the whole
        // field answer the weather. It used to wrap over one distance for the whole
        // world, set near the tallest crown there is — so on anything shorter, most
        // of every leaf's cycle was spent below the ground it had already landed on,
        // culled and drawn as nothing. Half the population was invisible, and worse:
        // `Carry` is quadratic in how far through its flight a leaf is, so the part
        // that *was* on screen only ever reached the flat beginning of that curve
        // and collected a quarter of the sideways travel it should have. The wind
        // was being thrown away exactly where it would have been seen.
        //
        // Wrapped over the height instead, every leaf is visible for the whole of
        // its life, and the whole of the quadratic happens where it can be watched.
        const float span = std::max(height, 1.0f);

        const float top = under->foot - height;

        // How fast this leaf falls, how far down it has got, and — from those two
        // alone — the moment it was let go of. `drift` wraps, so `released` steps
        // back by one whole fall each time the leaf recycles: it is the release
        // time exactly, in closed form, with nothing stored.
        const float pace     = kLeafFall * (0.7f + 0.6f * Chance(cell, 23, settings_.seed));
        const float drift    = std::fmod(Chance(cell, 29, settings_.seed) * span + now * pace, span);
        const float released = now - drift / std::max(pace, 1e-3f);

        // The wind at this cell, not at the middle of the view: a gust is a wave
        // crossing the world, so one end of a wood can be blowing while the other
        // is still. The same reading the grass and the crowns take.
        //
        // And at the moment the leaf left the crown, not at this one. Sampled now,
        // the figure multiplies the whole of a path the leaf has already flown, so
        // every leaf in the air slides sideways as the gust rises and slides *back*
        // as it falls away. Nothing falls upwind. Asked about its own release the
        // leaf keeps whatever air it was let go into, which is also what makes a
        // gust legible: the crest hands a cohort of leaves the same hard push, and
        // they come down together some seconds behind it.
        const float wind = sky.WindAt(column, released);

        // How hard the wood is shedding here, which is the season and the weather
        // together. On a still day a tree lets go of what it was going to let go
        // of anyway; a gale strips it, and the difference between the two is most
        // of what makes weather worth watching.
        //
        // On the release moment too, and that is what makes the count mean anything.
        // Gated on the wind now, a leaf loosened by a gust would wink out of the air
        // the instant the crest moved on; gated on its release it stays up for the
        // whole of its fall, so what is on screen is a record of the gusts that have
        // gone by rather than a headcount of the one blowing.
        const float loosened =
            kCalmShed * shedding + (1.0f - kCalmShed) * stripping * sky.Stir(column, released);

        if (Chance(cell, 13, settings_.seed) > kLeafDensity * loosened) continue;

        const float y = top + drift;
        if (y > under->foot) continue;

        // A leaf does not fall straight: it swings from side to side as it goes,
        // and the swing is most of what says leaf rather than raindrop.
        const float swing = std::sin((drift / kLeafSwing) + Chance(cell, 31, settings_.seed) * 6.28318f);

        // And carried, by the same rule as everything else loose in the air. The
        // flight lasts as long as this fall takes at this leaf's own pace, which is
        // what turns the air's speed into a distance.
        const float carried = weather::Carry(wind, drift / span, span / pace, weather::kLeafDrag);

        const float x = column + swing * kLeafSwing * kLeafSide + carried;

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

        drifting_++;

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
