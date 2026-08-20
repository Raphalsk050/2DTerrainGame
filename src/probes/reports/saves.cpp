#include "core/config.h"
#include "core/registry.h"
#include "core/stack.h"
#include "craft/craft.h"
#include "entity/fixture.h"
#include "entity/mob/herd.h"
#include "entity/player/player.h"
#include "flora/grove.h"
#include "item/inventory.h"
#include "probes/report.h"
#include "save/record.h"
#include "save/save.h"
#include "world/terrain.h"
#include "world/world.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// `--saves` — a world written down, read back, and written again.
//
// This is CLAUDE.md §2's bar pointed at the one file the player cannot make again.
// The rule there is that a change must not move a single number; the rule here is that
// **a save read back and written out must be the same file, byte for byte.** Anything
// that is dropped on the way in shows up as a shorter second file, anything that is
// mangled shows up as a different line, and the check names the line.
//
// It is worth having for exactly the reason the round trip is worth doing at all: a
// save is the only state in this program whose faults are invisible until a player has
// already lost something. A chest whose contents are written but not read is a chest
// that looks fine for the whole session it was filled in.
//
// Four verdicts:
//
//   - **The file survives the round trip.** Every line but the timestamp, which is a
//     fact about the room rather than about the world and is deliberately new on every
//     write.
//   - **The world survives it.** Counted from the live objects rather than from the
//     file — the journal, the wood, what is standing, what is in it, what is carried,
//     what is remembered about the animals. A file that round-trips through a reader
//     that quietly drops everything would pass the first check and fail this one.
//   - **A name that is not there is refused.** Broken deliberately, because a guard
//     nobody has watched fire is not a guard (§19.2).
//   - **Renaming changes the name and nothing else.** Which is what makes it safe to do
//     to a world somebody has played for a week.
namespace {

// Where the check writes. Under the saves folder like any other, and swept up
// afterwards — a probe that leaves worlds lying in the player's list is a probe nobody
// runs twice.
constexpr const char *kFirst  = "probe-round-a";
constexpr const char *kSecond = "probe-round-b";

std::vector<std::string> Lines(const std::string &path) {
    std::vector<std::string> lines;

    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return lines;

    std::string line;

    for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file)) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();

            continue;
        }

        line += static_cast<char>(c);
    }

    if (!line.empty()) lines.push_back(line);

    std::fclose(file);

    return lines;
}

// What the two files disagree about, or nothing.
//
// The `written` record is skipped, and it is the only one: it is seconds since the
// epoch at the moment of writing, so two writes a second apart legitimately differ and
// a check that demanded they match would fail on being correct.
std::string Differs(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    if (a.empty()) return "the first file is empty";

    for (std::size_t i = 0; i < a.size() || i < b.size(); i++) {
        if (i >= a.size()) return TextFormat("line %d: the second file has more in it — %s", static_cast<int>(i) + 1,
                                             b[i].c_str());

        if (i >= b.size()) return TextFormat("line %d: the second file stops early — %s", static_cast<int>(i) + 1,
                                             a[i].c_str());

        if (a[i] == b[i]) continue;
        if (a[i].rfind("written ", 0) == 0 && b[i].rfind("written ", 0) == 0) continue;

        return TextFormat("line %d: %s   against   %s", static_cast<int>(i) + 1, a[i].c_str(), b[i].c_str());
    }

    return {};
}

// What the world is holding, as five numbers.
//
// Counted off the live objects and never off the file, which is the whole point of it:
// a reader that parsed every line perfectly and then threw the result away would give
// two identical files and an empty world.
struct Tally {
    int edits   = 0;
    int trees   = 0;
    int chests  = 0;
    int kept    = 0;
    int rules   = 0;
    int carried = 0;
    int wear    = 0;
    int patches = 0;
    int beasts  = 0;

    bool operator==(const Tally &) const = default;
};

// Where the probe stands its chests. Written down once so that counting what is in
// them after a load asks the same cells the setup used — the save carries the
// positions, so the two agreeing is itself part of what is being checked.
constexpr int kBankAt  = 1;
constexpr int kAloneAt = 8;
constexpr int kChestRow = -41;

Tally Count(const World &world, const Grove &grove, fixture::Fixtures &chests, const Inventory &pack,
            const mob::Herd &herd) {
    Tally out;

    out.edits   = world.RememberedEdits();
    out.trees   = grove.RememberedPlants();
    out.chests  = chests.Held();
    out.patches = herd.Memory().Remembered();

    // Everything asleep and everything walking about. Rule five is about creatures and
    // not about cells, and a warren that came back with all its patches and none of its
    // animals would pass a count of patches perfectly.
    out.beasts = herd.Memory().Resting() + herd.Live();

    for (int slot = 0; slot < Inventory::kSlots; slot++) {
        out.carried += pack.At(slot).count;
        out.wear += pack.At(slot).wear;
    }

    // What is in the chests, through the same `Run` and `Store` the panel opens them
    // with. Rule four is the one a file can round-trip perfectly while losing: the
    // fixtures come back, the slots inside them do not, and nothing about the count of
    // fixtures says so.
    for (const int cx : {kBankAt, kAloneAt}) {
        const fixture::Joined run = chests.Run(cx, kChestRow);
        if (!run.Any()) continue;

        slots::Bank bank = chests.Store(run);

        out.kept += bank.Held();

        std::vector<slots::Kind *> rules;

        chests.Rules(run, rules);

        for (const slots::Kind *rule : rules) {
            if (rule != nullptr && rule->Any()) out.rules++;
        }
    }

    return out;
}

// Puts something in the world worth losing.
//
// Every one of the five things a save is asked to keep, and each in a way that would
// break differently: ground dug *and* built so both halves of an `Edit` are exercised,
// a bank of joined chests so the per-unit rule is, a tool with wear on it so a slot
// that is rebuilt field by field shows up, and a creature so the warren's names do.
void Build(World &world, Grove &grove, fixture::Fixtures &chests, Inventory &pack, Player &player, mob::Herd &herd,
           const terrain::Settings &settings, int &dug, int &laid, int &put) {
    // Well clear of the ground, so every cell starts empty and the count is the
    // probe's own rather than the hillside's.
    constexpr int kRow = kChestRow + 1;

    world.Update({-600.0f, static_cast<float>(kRow) * config::kBuildCell - 600.0f, 2400.0f, 1600.0f});

    for (int cx = 0; cx < 24; cx++) {
        if (world.PlaceCell((cx % 2 == 0) ? Element::WoodPlank : Element::Cobblestone, cx, kRow).filled > 0) laid++;
    }

    // And a wall behind some of them, which is the layer an `Edit` keeps apart from
    // the block in front (§11.2) and the field a save is most likely to lose.
    for (int cx = 4; cx < 12; cx++) world.PlaceCell(Element::WoodWall, cx, kRow + 1);

    for (int cx = 0; cx < 8; cx++) {
        if (world.ExcavateCell(cx, kRow).freed[ElementIndex(Element::WoodPlank)] > 0) dug++;
    }

    // Three chests in a row, which is a bank — and one apart from them, which is not.
    for (int cx = 0; cx < 3; cx++) {
        if (chests.Place(fixture::Kind::Chest, cx, kChestRow)) put++;
    }

    if (chests.Place(fixture::Kind::Chest, kAloneAt, kChestRow)) put++;
    if (chests.Place(fixture::Kind::Torch, 12, kChestRow)) put++;

    {
        const fixture::Joined run = chests.Run(kBankAt, kChestRow);

        slots::Bank bank = chests.Store(run);

        const char *kInside[] = {"coal", "iron", "diamond", "wood", "stick"};

        for (int i = 0; i < 5 && i * 7 < bank.Size(); i++) {
            const std::optional<Stack> stack = craft::Named(kInside[i], 9);

            if (stack.has_value()) bank.Put(i * 7, *stack);
        }

        // A row set aside, which is the chest's own memory and lives on the unit
        // rather than on the bank.
        std::vector<slots::Kind *> rules;

        chests.Rules(run, rules);

        const std::optional<Stack> cobble = craft::Named("cobblestone", 1);

        if (!rules.empty() && cobble.has_value()) *rules[1] = slots::Kind::Of(*cobble);
    }

    const char *kBag[] = {"rock", "soil", "wood plank", "apple", "torch"};

    for (const char *what : kBag) {
        const std::optional<Stack> stack = craft::Named(what, 12);

        if (stack.has_value()) pack.Add(*stack);
    }

    // A worn tool, because `wear` is the field four call sites once dropped by
    // rebuilding a `Stack` by hand (§29.3) and a save is a fifth chance to.
    if (const std::optional<Item> pick = item::Find("iron pickaxe"); pick.has_value()) {
        Stack worn = ItemsOf(*pick, 1);

        worn.wear = 37;

        pack.Add(worn);
    }

    pack.Select(4);

    player = Player({320.0f, terrain::Height(320.0f, settings) - 96.0f});

    // A tree the player has felled and one they have planted, which are the two things
    // a wood remembers that its own scatter cannot produce.
    grove.Plant(flora::Species::Oak, {200.0f, terrain::Height(200.0f, settings)}, 120.0f);
    grove.Plant(flora::Species::Pine, {260.0f, terrain::Height(260.0f, settings)}, 140.0f);

    // Creatures, in both of the two states a save has to keep them in.
    //
    // Asleep *and* awake, which is not thoroughness for its own sake: they are held in
    // two different places — a sleeping one is a `Life` in its patch and a waking one is
    // a `Mob` in the herd's pool — and the save has to gather them from both. Writing
    // only the patches loses every animal near the player, which is every animal they
    // can see.
    const std::optional<mob::Kind> boar = mob::kinds::Find("boar");

    if (!boar.has_value()) return;

    const float ground = terrain::Height(0.0f, settings);

    // Four put down near the origin, walked once so the warren knows about them, and
    // then left behind by a view that has moved a long way off — which is what files
    // them into their patches asleep.
    for (int i = 0; i < 4; i++) {
        const float x = 200.0f + static_cast<float>(i) * 90.0f;

        herd.Put(*boar, {x, terrain::Height(x, settings) - 40.0f});
    }

    const Rectangle near = {-900.0f, ground - 900.0f, 1800.0f, 1800.0f};
    const Rectangle away = {60000.0f, ground - 900.0f, 1800.0f, 1800.0f};

    herd.Update(world, near, player.Bounds(), player.Centre(), 100.0f, 1.0f / 60.0f, grove.Fallen());
    herd.Update(world, away, player.Bounds(), player.Centre(), 101.0f, 1.0f / 60.0f, grove.Fallen());

    // And one more standing where the player is, which stays awake — the case that was
    // silently dropped until the round trip caught it, because its cell had no record
    // for it to be filed under.
    herd.Put(*boar, {340.0f, terrain::Height(340.0f, settings) - 40.0f});
}

void Sweep() {
    std::error_code oops;

    std::filesystem::remove_all(std::string(save::Folder()) + "/" + kFirst, oops);
    std::filesystem::remove_all(std::string(save::Folder()) + "/" + kSecond, oops);
}

int Run(const probes::Bench &bench) {
    World &world              = *bench.world;
    Grove &grove              = *bench.grove;
    terrain::Settings settings = *bench.settings;

    fixture::Fixtures chests;
    Inventory pack;
    Player player({0.0f, 0.0f});
    mob::Herd herd;

    herd.Memory().Configure(settings.seed);

    Sweep();

    int dug  = 0;
    int laid = 0;
    int put  = 0;

    Build(world, grove, chests, pack, player, herd, settings, dug, laid, put);

    const save::Game whole = {.world  = &world,
                              .grove  = &grove,
                              .chests = &chests,
                              .herd   = &herd,
                              .pack   = &pack,
                              .player = &player};

    const Tally before = Count(world, grove, chests, pack, herd);

    std::printf("\n  put in:  %d cells built, %d dug back out, %d fixtures stood up\n", laid, dug, put);
    std::printf("  holding: %d edits, %d trees, %d fixtures holding %d things behind %d rules\n", before.edits,
                before.trees, before.chests, before.kept, before.rules);
    std::printf("           %d items carried with %d of wear on them, %d patches, %d creatures\n\n", before.carried,
                before.wear, before.patches, before.beasts);

    save::Slot first;
    save::Slot second;

    first.id  = kFirst;
    second.id = kSecond;

    const save::Stamp stamp = {.name = "round trip", .seed = settings.seed, .mode = Gamemode::Survival};

    if (!save::Write(first, stamp, whole, nullptr)) {
        std::printf("  COULD NOT WRITE — %s\n\n", first.Path().c_str());

        return 1;
    }

    // Everything forgotten, exactly as the loading path forgets it before it reads.
    //
    // Through the same calls the loop makes rather than by making fresh objects,
    // because that is the state a load actually lands in — a check against a
    // pristine world would not exercise the one thing that can go wrong, which is a
    // record surviving from the country that has just gone.
    world.Reset();
    grove.Clear();
    chests.Clear();
    herd.Clear();
    pack.Clear();

    herd.Memory().Configure(settings.seed);

    save::Stamp read;

    if (!save::Read(first, read, whole)) {
        std::printf("  COULD NOT READ — %s\n\n", first.Path().c_str());

        Sweep();

        return 1;
    }

    const Tally after = Count(world, grove, chests, pack, herd);

    if (!save::Write(second, {.name = read.name, .seed = read.seed, .mode = read.mode}, whole, nullptr)) {
        std::printf("  COULD NOT WRITE THE SECOND\n\n");

        Sweep();

        return 1;
    }

    const std::string clash = Differs(Lines(first.Path()), Lines(second.Path()));

    std::printf("  read back: %d edits, %d trees, %d fixtures holding %d things behind %d rules\n", after.edits,
                after.trees, after.chests, after.kept, after.rules);
    std::printf("             %d items carried with %d of wear on them, %d patches, %d creatures\n", after.carried,
                after.wear, after.patches, after.beasts);
    std::printf("  stamp:     \"%s\", seed %d, %s\n\n", read.name.c_str(), read.seed, NameOf(read.mode));

    // The one name the reader must refuse.
    //
    // Checked by breaking a save on purpose and watching it fire, which is the only way
    // to know a guard guards anything (§19.2). A material this build has never heard of
    // has to stop the read rather than leave a hole in the hillside, and a reader that
    // shrugged would pass every other check on this page.
    bool refused = false;

    {
        save::Slot broken;

        broken.id = kFirst;

        std::vector<std::string> lines = Lines(broken.Path());

        for (std::string &line : lines) {
            if (line.rfind("edit ", 0) != 0) continue;

            const std::size_t at = line.find("\"wood plank\"");

            if (at == std::string::npos) continue;

            line.replace(at, std::string("\"wood plank\"").size(), "\"unobtainium\"");

            break;
        }

        std::FILE *out = std::fopen(broken.Path().c_str(), "wb");

        if (out != nullptr) {
            for (const std::string &line : lines) std::fprintf(out, "%s\n", line.c_str());

            std::fclose(out);

            save::Stamp ignored;

            refused = !save::Read(broken, ignored, whole);
        }
    }

    // And that a rename touches the name and nothing else.
    bool renamed = false;

    {
        const std::vector<std::string> was = Lines(second.Path());

        renamed = save::Rename(second.id, "a different name");

        save::Slot peeked;

        renamed = renamed && save::Peek(second.id, peeked) && peeked.name == "a different name";

        const std::vector<std::string> now = Lines(second.Path());

        if (was.size() != now.size()) renamed = false;

        for (std::size_t i = 0; i < was.size() && renamed; i++) {
            if (was[i] == now[i]) continue;
            if (was[i].rfind("name ", 0) == 0) continue;

            renamed = false;
        }
    }

    // The listing, which is the one part of this the menu actually reads.
    //
    // `List` and `Peek` are what the saves screen draws a row from, and they walk a
    // different path through the file from `Read` — the head only, stopping at the
    // first section. A head that `Read` understands and `Peek` does not is a save that
    // loads perfectly and cannot be found.
    bool listed = false;

    {
        const std::vector<save::Slot> found = save::List();

        int seen = 0;

        for (const save::Slot &slot : found) {
            if (slot.id != kFirst && slot.id != kSecond) continue;

            seen++;

            if (slot.seed != settings.seed) listed = false;
        }

        listed = seen == 2;
    }

    Sweep();

    const bool same = clash.empty();
    const bool held = before == after;

    if (!same) std::printf("  %s\n", clash.c_str());

    std::printf("  %s\n", same ? "written, read and written again come out the same file"
                               : "DRIFTED — the round trip changed the save");
    std::printf("  %s\n", held ? "the world read back is the world that was written"
                               : "LOST — the world holds something different afterwards");
    std::printf("  %s\n", refused ? "a name this build does not know is refused rather than dropped"
                                  : "SWALLOWED — a broken save loaded anyway");
    std::printf("  %s\n", renamed ? "renaming changes the name and nothing else"
                                  : "RENAME WRONG — it changed more than the name, or nothing at all");
    std::printf("  %s\n\n", listed ? "both worlds are found by the listing the menu reads"
                                   : "NOT LISTED — a save that loads cannot be found");

    return (same && held && refused && renamed && listed) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--saves",
    .wants = 2,
    .shows = false,
    .blurb = "--saves - a world written down, read back, and written again",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
