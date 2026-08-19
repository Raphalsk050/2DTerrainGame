#include "entity/mob/wardrobe.h"

#include "entity/mob/mob_def.h"
#include "raylib.h"

#include <string>
#include <unordered_map>

namespace {

// Where a creature's art lives, and what the three clips are always called.
//
// Written here once rather than on every row: a table full of file paths is a table
// full of things that can be misspelt, and the misspelling shows up as a creature
// drawn from its fallback art with nothing saying why.
const char *kUnder = "assets/mobs/";

std::unordered_map<std::string, mob::Wardrobe> &Racks() {
    static std::unordered_map<std::string, mob::Wardrobe> racks;

    return racks;
}

sheet::Strip Wear(const std::string &folder, const char *clip, int wide) {
    const std::string path = std::string(kUnder) + folder + "/" + clip + ".png";

    return sheet::Load(path.c_str(), wide);
}

} // namespace

const mob::Wardrobe &mob::Dressed(const Def &def) {
    static const Wardrobe bare;

    if (def.art == nullptr) return bare;

    Wardrobe &rack = Racks()[def.art];

    if (rack.tried) return rack;

    rack.tried = true;

    rack.idle = Wear(def.art, "idle", def.artWide);
    rack.walk = Wear(def.art, "walk", def.artWide);
    rack.run  = Wear(def.art, "run", def.artWide);

    if (!rack.Any()) {
        // Said once, and only once, because `tried` is already set.
        //
        // A warning rather than a refusal: a creature with no art still has the
        // hand-drawn `look` on its row and is perfectly playable from it. What must
        // not happen is silence — art that quietly did not load is a creature that
        // looks wrong for a reason nobody can see.
        TraceLog(LOG_WARNING, "mob '%s': no art found under %s%s/", def.name, kUnder, def.art);
    }

    return rack;
}

void mob::Undress() {
    for (auto &[folder, rack] : Racks()) {
        sheet::Unload(rack.idle);
        sheet::Unload(rack.walk);
        sheet::Unload(rack.run);

        rack.tried = false;
    }

    Racks().clear();
}
