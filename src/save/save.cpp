#include "save/save.h"

#include "entity/fixture.h"
#include "entity/mob/herd.h"
#include "entity/player/player.h"
#include "flora/grove.h"
#include "item/inventory.h"
#include "world/world.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

// The one number that says what a save file is.
//
// Bumped when the *format* changes in a way an older reader would misread — not when
// a field is added, because a reader that walks records by tag simply does not see a
// tag it has no case for. That is the whole reason the format is records rather than
// a struct laid out in order.
constexpr long long kVersion = 1;

// Seconds since the epoch, for sorting the list. The wall clock and not the weather
// one: what it answers is "which of these did I play last", which is a question about
// the room and not about the world.
std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

bool save::Write(const Slot &slot, const Stamp &stamp, const Game &game, const Image *shot) {
    if (!game.Whole()) return false;

    // Written beside the real file and moved over it at the end.
    //
    // A save is the one thing in this program a player cannot make again. A write that
    // stops halfway — the window closed, the disk full, the machine off — must not be
    // allowed to leave the old save destroyed and the new one truncated, and the only
    // way to be sure of that is never to open the real file until there is a whole one
    // to put in its place.
    const std::string interim = slot.Path() + ".part";

    {
        Writer out(interim);

        if (!out.Ok()) return false;

        out.Tag("save").Int(kVersion).Done();
        out.Tag("name").Text(stamp.name).Done();
        out.Tag("seed").Int(stamp.seed).Done();
        out.Tag("mode").Text((stamp.mode == Gamemode::Creative) ? "creative" : "survival").Done();
        out.Tag("clock").Real(game.world->Sky().Time()).Done();
        out.Tag("written").Int(Now()).Done();

        // `player` is the first section, and `Peek` stops on it — so everything above
        // this line is the head, and listing a folder of saves reads six lines each
        // rather than six megabytes.
        game.player->Save(out);
        game.pack->Save(out);
        game.world->Save(out);
        game.grove->Save(out);
        game.chests->Save(out);

        // The creatures the herd is still walking about, filed into their patches as
        // the warren writes. Gathered here rather than inside the warren because the
        // herd is the loop's and the warren has no business knowing a herd exists —
        // the same seam `Warren::Wake` already keeps.
        std::vector<mob::Life> living;

        for (const mob::Mob &one : game.herd->All()) {
            if (one.Live()) living.push_back(one.Remember());
        }

        game.herd->Memory().Save(out, living);

        if (!out.Ok()) {
            std::error_code oops;

            std::filesystem::remove(interim, oops);

            return false;
        }
    }

    std::error_code oops;

    std::filesystem::rename(interim, slot.Path(), oops);

    if (oops) {
        std::filesystem::remove(interim, oops);

        return false;
    }

    // And the picture, after the world. It is the one part of a save that can fail
    // without the save failing: a world written without a preview lists perfectly
    // well, and losing a session over a thumbnail would be absurd.
    if (shot != nullptr && shot->data != nullptr && shot->width > 0) ExportImage(*shot, slot.ShotPath().c_str());

    return true;
}

bool save::Read(const Slot &slot, Stamp &stamp, const Game &game) {
    if (!game.Whole()) return false;

    Reader in(slot.Path());

    if (!in.Opened()) return false;

    bool head = false;

    float clock = 0.0f;

    while (in.Next()) {
        if (in.Is("save")) {
            // Refused outright on a version this build does not know, rather than read
            // hopefully. A reader that walks tags survives *new* fields by ignoring
            // them; what it cannot survive is a field that has changed meaning, and
            // this number is the only warning of that there will ever be.
            if (in.Int() != kVersion) return false;

            head = true;
            continue;
        }

        // Nothing is read before the version, so a file that is not one of ours — or
        // one whose first line was lost — stops here rather than being half-applied.
        if (!head) return false;

        if (in.Is("name")) {
            stamp.name = in.Text();
            continue;
        }

        if (in.Is("seed")) {
            stamp.seed = static_cast<int>(in.Int());
            continue;
        }

        if (in.Is("mode")) {
            stamp.mode = (in.Text() == "creative") ? Gamemode::Creative : Gamemode::Survival;
            continue;
        }

        if (in.Is("clock")) {
            clock = in.Real();
            continue;
        }

        if (in.Is("written")) {
            in.Int();
            continue;
        }

        if (in.Is("player")) {
            game.player->Load(in);
            continue;
        }

        if (in.Is("pack")) {
            game.pack->Load(in);
            continue;
        }

        if (in.Is("edits")) {
            game.world->Load(in);
            continue;
        }

        if (in.Is("trees")) {
            game.grove->Load(in);
            continue;
        }

        if (in.Is("fixtures")) {
            game.chests->Load(in);
            continue;
        }

        if (in.Is("patches")) {
            game.herd->Memory().Load(in);
            continue;
        }

        // A record this build has no case for is skipped rather than refused, which is
        // what makes the format additive: a save written by a later build with a
        // section this one has never heard of loads as the world without that section,
        // and the version above is what says when that is no longer safe.

        if (!in.Ok()) return false;
    }

    if (!head || !in.Ok()) return false;

    // The clock last, after everything that was recorded against it. Nothing here
    // reads the sky while loading, so the order is not load-bearing — but a world
    // whose trees were written down at hour nine and whose sky says noon while they
    // are being read is a state that only exists inside this function, and it is
    // cheaper to never have it than to remember it is harmless.
    game.world->SetClock(clock);

    return true;
}
