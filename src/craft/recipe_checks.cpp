#include "core/registry.h"
#include "craft/craft.h"
#include "craft/recipe_def.h"

#include <string>

// A recipe names three things it does not own: a product, and up to four
// ingredients, each a row of one of the other two tables. Every one of those joins
// is a string, and a string is exactly the kind of join that breaks in a commit
// about wording — §16.2b's lesson, which is why the fixture table and the species
// table are both checked the same way.
//
// What a broken join looks like without this: a recipe that never appears, or one
// that appears and cannot be built, with nothing anywhere saying which name was
// wrong. Checked here it is a line printed before the window opens, and the program
// refuses to start.
namespace {

std::string RecipesNameThingsThatExist() {
    std::string wrong;

    const auto fault = [&wrong](const std::string &line) {
        if (!wrong.empty()) wrong += "; ";

        wrong += line;
    };

    for (int i = 0; i < craft::Count(); i++) {
        const craft::RecipeDef &def = craft::Of(craft::Recipe{i});

        const std::string what = std::string("recipe '") + def.name + "' ";

        if (!craft::Named(def.makes, 1)) fault(what + "makes '" + def.makes + "', and there is no such row");

        // A name in both tables would make the lookup's order into a decision, and
        // one nobody wrote down. There is no such name today; this is what says so
        // on the day somebody adds one.
        if (craft::Ambiguous(def.makes)) fault(what + "makes '" + def.makes + "', which is both an item and a material");

        if (def.yields < 1) fault(what + "yields " + std::to_string(def.yields) + ", which is nothing");

        int asked = 0;

        for (int n = 0; n < craft::kMaxNeeds; n++) {
            const craft::Need &need = def.needs[n];

            // An unasked slot is one with no name. A slot with a name and no count
            // is a row half written, and it has to be told apart from an empty one
            // or it reads as free.
            if (need.what == nullptr) {
                if (need.count != 0) fault(what + "asks for " + std::to_string(need.count) + " of nothing");
                continue;
            }

            asked++;

            if (need.count < 1) fault(what + "asks for " + std::to_string(need.count) + " '" + need.what + "'");

            if (!craft::Named(need.what, 1)) fault(what + "needs '" + need.what + "', and there is no such row");

            if (craft::Ambiguous(need.what)) {
                fault(what + "needs '" + need.what + "', which is both an item and a material");
            }

            // The same ingredient twice would be counted twice and spent twice, and
            // `Inventory::Remove` would take both — so a recipe asking for two wood
            // and one wood costs three and reads as asking for two.
            for (int before = 0; before < n; before++) {
                if (def.needs[before].what == nullptr) continue;

                if (std::string(def.needs[before].what) == need.what) {
                    fault(what + "asks for '" + need.what + "' twice");
                }
            }
        }

        if (asked == 0) fault(what + "asks for nothing at all");
    }

    return wrong;
}

const registry::Checker named{RecipesNameThingsThatExist};

} // namespace
