#include "entity/mob/spawner.h"

#include "entity/mob/brains/whim.h"
#include "entity/mob/herd.h"
#include "world/element.h"
#include "world/terrain.h"
#include "world/world.h"

#include <cmath>

namespace {

// How many spots one attempt tries before giving up.
//
// A handful and not one: most of the world suits nothing, so a single spot would
// mean a creature arriving every several seconds even in country full of them. A
// handful and not a hundred, because the cost of an attempt is what keeps the
// spawner off the frame.
constexpr int kTries = 6;

// How much room a creature needs above the ground it is put on, as a multiple of
// its own height.
//
// A creature spawned flush against a ceiling is a creature spawned inside one: the
// body is unstuck on its first frame, which throws it somewhere it was never meant
// to be, and what a player sees is an animal appearing out of a wall.
constexpr float kHeadroom = 1.25f;

} // namespace

bool mob::spawn::Suits(const World &world, const Def &def, Vector2 at) {
    const Haunt &haunt = def.haunt;

    // The generated surface, not the dug one. A player who sinks a shaft has not
    // moved the country, and a cave creature welling up out of a staircase because
    // the ground above it was carried away is a creature following the shovel. Same
    // argument CLAUDE.md §17.3 makes about the horizon.
    const float surface = terrain::Height(at.x, world.Settings());

    const float depth = at.y - surface;

    if (depth < haunt.fromDepth || depth > haunt.toDepth) return false;

    // Room to stand. Both tests are needed and they are different: the first says
    // the creature is not inside anything, the second that there is something under
    // it — a spot in mid-air over a ravine passes the first on its own.
    const float half = def.build.width / 2.0f;

    const Rectangle box = {at.x - half, at.y - def.build.height * kHeadroom, def.build.width,
                           def.build.height * kHeadroom};

    if (world.OverlapsSolid(box)) return false;

    // A floating creature needs no floor — that is what floating is — and asking
    // one for a floor is how a cave full of headroom comes out with no bats in it.
    if (!def.build.floats) {
        if (!world.OverlapsSolid({at.x - half, at.y + 1.0f, def.build.width, 2.0f})) return false;
    }

    // Then the two that read fields, last because they are the dear ones.
    const float lit = world.LightLevelAt({at.x, at.y - def.build.height * 0.5f});

    if (lit > haunt.darkerThan || lit < haunt.brighterThan) return false;

    const terrain::Climate climate = terrain::ClimateAt(at.x, world.Settings());

    // The same bell every cover, wood, drought and snowfall is placed by. A
    // creature's country and the grass's country are one fact read twice, not two
    // facts kept in step by hand — see CLAUDE.md §8.
    const float suits = ClimateBell(haunt.climate, climate.temperature, climate.humidity);

    return suits >= haunt.climate.goneAt;
}

std::optional<mob::spawn::Wish> mob::spawn::Try(const World &world, Rectangle active, Vector2 quarry,
                                                std::uint32_t &seed, const Herd &herd) {
    const int rows = kinds::Count();

    if (rows <= 0) return std::nullopt;

    for (int t = 0; t < kTries; t++) {
        // A row first, then a place for it. The other way round — a place, then
        // whichever row suits it — reads better and is much worse: it makes a
        // creature's frequency depend on how many *other* rows happen to suit the
        // same ground, so adding a cave creature would quietly halve the bats.
        const Kind kind = Kind{Count(seed, 0, rows - 1)};

        const Def &def = kinds::Of(kind);

        if (def.haunt.chance <= 0.0f) continue;
        if (Chance(seed) >= def.haunt.chance) continue;

        // Already as many of these as the row allows. Checked per kind rather than
        // over the pool, so a cave full of bats cannot crowd every boar in the
        // county out of the world.
        if (herd.LiveOf(kind) >= def.haunt.crowd) continue;

        const float x = Between(seed, active.x, active.x + active.width);

        // Nothing may appear inside the view it is appearing into. Minecraft's rule
        // is a radius around the player; this is the same rule in the one dimension
        // this world has. Tested first because it is one subtraction and it throws
        // away half the candidates.
        if (std::fabs(x - quarry.x) < def.haunt.keepAway) continue;

        // The height is taken *from the row's own band*, measured off the surface at
        // this column — not from anywhere in the view.
        //
        // This is the whole difference between a spawner that works and one that
        // does not, and it shipped broken. Picking a uniform point in the simulated
        // region and rejecting it against the band means a boar, whose band is 56
        // pixels of a region thirteen hundred deep, is found by about one candidate
        // in twenty-five; with six tries a second attempt in a hundred succeeded, and
        // what that looked like was an empty world with no error anywhere in it.
        //
        // Sampling the band directly makes every candidate land in it by
        // construction, and leaves the refusals to be about things that are actually
        // worth refusing — rock, light, climate.
        const float surface = terrain::Height(x, world.Settings());

        const Vector2 at = {x, surface + Between(seed, def.haunt.fromDepth, def.haunt.toDepth)};

        // And it still has to be somewhere the simulation covers. A creature placed
        // outside the active region is one that is let go on the very next frame.
        if (at.y < active.y || at.y > active.y + active.height) continue;

        if (!Suits(world, def, at)) continue;

        return Wish{.kind = kind, .at = at, .many = Count(seed, def.haunt.least, def.haunt.most)};
    }

    return std::nullopt;
}
