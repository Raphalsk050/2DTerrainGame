#include "entity/mob/suits.h"

#include "world/element.h"
#include "world/terrain.h"
#include "world/world.h"

bool mob::Suits(const World &world, const Def &def, Vector2 at) {
    const Haunt &haunt = def.haunt;

    // The generated surface, not the dug one. A player who sinks a shaft has not
    // moved the country, and a cave creature welling up out of a staircase because
    // the ground above it was carried away is a creature following the shovel. Same
    // argument CLAUDE.md §17.3 makes about the horizon.
    const float depth = at.y - terrain::Height(at.x, world.Settings());

    if (depth < haunt.fromDepth || depth > haunt.toDepth) return false;

    // Room to stand. Both tests are needed and they are different: the first says the
    // creature is not inside anything, the second that there is something under it —
    // a spot in mid-air over a ravine passes the first on its own.
    const float half = def.build.width / 2.0f;

    // Stopped a pixel short of the feet, because a body does not occupy the ground it
    // is standing on.
    //
    // Without the pixel the room test overlaps the top row of squares of the very floor
    // the creature was placed on, so every spot found by walking down to a floor was
    // then refused for being inside it — and the world came out with no animals in it
    // at all.
    const Rectangle box = {at.x - half, at.y - def.build.height * kHeadroom, def.build.width,
                           def.build.height * kHeadroom - 1.0f};

    if (world.OverlapsSolid(box)) return false;

    // A floating creature needs no floor — that is what floating is — and asking one
    // for a floor is how a cave full of headroom comes out with no bats in it.
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
    return ClimateBell(haunt.climate, climate.temperature, climate.humidity) >= haunt.climate.goneAt;
}
