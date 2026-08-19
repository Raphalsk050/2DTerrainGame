#include "core/registry.h"
#include "flora/flora.h"
#include "item/item_def.h"

#include <string>

// The joins between the species table and the item table, checked at startup.
//
// What used to be `static_assert(SaplingsArePlantable())`. See
// `entity/fixture_checks.cpp` for the general case and CLAUDE.md §16.2b for the
// day that produced both.
namespace {

std::string SaplingsArePlantable() {
    std::string wrong;

    for (std::size_t e = 0; e < flora::kSpeciesCount; e++) {
        const flora::SpeciesDef &def = flora::kSpecies[e];

        // No sapling at all is a legitimate answer — a species that does not sow —
        // and must not be reported. This is the one distinction the old
        // `std::optional` was introduced to make, and it survives the move to
        // names because `nullptr` says the same thing.
        if (def.sapling == nullptr) continue;

        const std::optional<Item> sapling = item::Find(def.sapling);

        if (!sapling.has_value()) {
            if (!wrong.empty()) wrong += "; ";

            wrong += std::string("species '") + def.name + "' sows '" + def.sapling
                     + "', and there is no such item";

            continue;
        }

        if (Def(*sapling).placement != Placement::Plant) {
            if (!wrong.empty()) wrong += "; ";

            wrong += std::string("species '") + def.name + "' sows item '" + def.sapling
                     + "', whose placement is not Plant — the hand will never put it in the ground";
        }
    }

    return wrong;
}

// Every drop names an item that exists, in a quantity that can happen.
//
// `least` over `most` is the silent one: the roll comes out empty, the tree drops
// nothing, and the row looks perfectly reasonable.
std::string DropsAreSound() {
    std::string wrong;

    for (std::size_t e = 0; e < flora::kSpeciesCount; e++) {
        const flora::SpeciesDef &def = flora::kSpecies[e];

        for (const flora::DropRule &rule : def.drops) {
            if (rule.item == nullptr) continue;

            if (!item::Find(rule.item).has_value()) {
                if (!wrong.empty()) wrong += "; ";

                wrong += std::string("species '") + def.name + "' drops '" + rule.item
                         + "', and there is no such item";

                continue;
            }

            if (rule.least < 0 || rule.most < rule.least) {
                if (!wrong.empty()) wrong += "; ";

                wrong += std::string("species '") + def.name + "' drops '" + rule.item
                         + "' in a range that cannot happen";
            }
        }
    }

    return wrong;
}

const registry::Checker plantable{SaplingsArePlantable};
const registry::Checker sound{DropsAreSound};

} // namespace
