#include "core/registry.h"
#include "entity/mob/brain.h"
#include "entity/mob/mob_def.h"
#include "item/item_def.h"

#include <cstring>
#include <string>

// Everything a creature's row claims, checked at startup.
//
// A creature is the one kind of content in this project that cannot be checked by
// looking: a boar that never spawns looks exactly like a boar that spawns somewhere
// you have not walked, and a creature whose behaviour did not resolve stands
// perfectly still, which is a thing calm animals also do. So the rows are checked
// against themselves before the window opens, and `--mobs` checks them against the
// world afterwards.
namespace {

std::string TempersExist() {
    std::string wrong;

    for (const mob::Def *def : mob::kinds::Table().All()) {
        if (mob::brain::Find(def->temper) != nullptr) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("mob '") + def->name + "' has the temper '"
                 + (def->temper != nullptr ? def->temper : "(none)") + "', and there is no such behaviour";
    }

    return wrong;
}

std::string SpoilsAreReal() {
    std::string wrong;

    for (const mob::Def *def : mob::kinds::Table().All()) {
        for (const mob::Spoil &spoil : def->spoils.each) {
            if (spoil.item == nullptr) continue;

            if (!item::Find(spoil.item).has_value()) {
                if (!wrong.empty()) wrong += "; ";

                wrong += std::string("mob '") + def->name + "' drops '" + spoil.item
                         + "', and there is no such item";

                continue;
            }

            // `least` over `most` is the silent one: the roll comes out empty, the
            // creature drops nothing, and the row looks perfectly reasonable.
            if (spoil.least < 0 || spoil.most < spoil.least) {
                if (!wrong.empty()) wrong += "; ";

                wrong += std::string("mob '") + def->name + "' drops '" + spoil.item
                         + "' in a range that cannot happen";
            }
        }
    }

    return wrong;
}

// A creature that can spawn has somewhere to spawn.
//
// A band with its ends the wrong way round is the trap, and it is exactly the
// `ClimateRamp` fault CLAUDE.md §8 warns about in another table: nothing errors,
// nothing warns, and the creature simply never appears anywhere in the world with
// no line of code to point at.
std::string HauntsAreReachable() {
    std::string wrong;

    for (const mob::Def *def : mob::kinds::Table().All()) {
        const mob::Haunt &haunt = def->haunt;

        // A row that never spawns on its own is a legitimate thing — something put
        // down by hand, or by a command — so it is not checked and not reported.
        if (haunt.chance <= 0.0f) continue;

        const char *fault = nullptr;

        if (haunt.toDepth < haunt.fromDepth) {
            fault = "its depth band runs backwards";
        } else if (haunt.darkerThan < haunt.brighterThan) {
            fault = "its light band runs backwards";
        } else if (haunt.least < 1 || haunt.most < haunt.least) {
            fault = "its group size cannot happen";
        } else if (haunt.climate.goneAt > 1.0f) {
            fault = "its climate is never suitable enough to appear";
        }

        if (fault == nullptr) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("mob '") + def->name + "' can never spawn: " + fault;
    }

    return wrong;
}

// Every figure is the size its row claims.
//
// See `figure::IsWellFormed` for why this is checked rather than trusted: a short
// row is not a syntax error, it is a creature drawn with a hole down one side, and
// a picture with a hole in it still looks like a picture.
std::string FiguresAreWellFormed() {
    std::string wrong;

    for (const mob::Def *def : mob::kinds::Table().All()) {
        if (figure::IsWellFormed(def->look)) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("mob '") + def->name + "' is not drawn at the size its row declares";
    }

    return wrong;
}

// A body has to be a body.
//
// Zero width or height is a creature nothing can ever collide with and nothing can
// ever hit, standing invisibly in the world for ever.
std::string BodiesAreSolid() {
    std::string wrong;

    for (const mob::Def *def : mob::kinds::Table().All()) {
        if (def->build.width > 0.0f && def->build.height > 0.0f && def->hardy > 0) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("mob '") + def->name + "' has no body to speak of";
    }

    return wrong;
}

std::string NamesAreUnique() {
    const auto &rows = mob::kinds::Table().All();

    std::string wrong;

    for (std::size_t i = 1; i < rows.size(); i++) {
        if (std::strcmp(rows[i - 1]->name, rows[i]->name) != 0) continue;

        if (!wrong.empty()) wrong += "; ";

        wrong += std::string("two mobs are both called '") + rows[i]->name + "'";
    }

    return wrong;
}

const registry::Checker tempers{TempersExist};
const registry::Checker spoils{SpoilsAreReal};
const registry::Checker haunts{HauntsAreReachable};
const registry::Checker figures{FiguresAreWellFormed};
const registry::Checker bodies{BodiesAreSolid};
const registry::Checker unique{NamesAreUnique};

} // namespace
