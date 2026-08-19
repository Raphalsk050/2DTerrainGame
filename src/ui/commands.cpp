#include "ui/commands.h"

#include "world/element.h"
#include "flora/flora.h"
#include "item/item_def.h"
#include "core/stack.h"
#include "weather/weather.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------- the commands
//
// What a typed line means. The console itself knows only how to take the line and
// show an answer — see console.h for why the two are apart — so this is where a
// name turns into a change to the world.
//
// Every branch answers, and answers in the tone that says whether it worked. A
// command that quietly does nothing is indistinguishable from one that is misspelt,
// and the whole reason for having a log is to be able to tell those apart.

// Splits on runs of spaces. A tokeniser rather than a parser, which is all the
// grammar here needs: a verb and up to a couple of words after it.
std::vector<std::string> Words(const std::string &line) {
    std::vector<std::string> out;

    std::size_t at = 0;

    while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') at++;

        const std::size_t from = at;

        while (at < line.size() && line[at] != ' ') at++;

        if (at > from) out.push_back(line.substr(from, at - from));
    }

    return out;
}

std::string Lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return text;
}

// Matches a word against a table of names, on the whole word or on a unique
// prefix — `/weather st` is unambiguous and there is no reason to make it be typed
// out. Returns -1 for no match and -2 where the prefix fits more than one.
int Named(const std::string &word, const char *const *names, int count) {
    int found = -1;

    for (int i = 0; i < count; i++) {
        const std::string name = Lower(names[i]);

        if (name == word) return i;

        if (name.rfind(word, 0) == 0) found = (found < 0) ? i : -2;
    }

    return found;
}

// One row per command: how it is typed, and what it is for.
//
// A table rather than a run of prints, because the help is asked for in two
// different ways — the whole list, and one command on its own — and two lists that
// have to agree is exactly the arrangement that ends with a command nobody can
// discover because somebody added it to one of them.
struct Command {
    const char *verb;
    const char *shape;
    const char *what;
};

constexpr Command kCommands[] = {
    {"help", "/help [command]", "what there is, or what one command takes"},
    {"weather", "/weather [name|auto]", "hold the sky at one weather"},
    {"season", "/season <name>", "hold the year at one season"},
    {"time", "/time", "run the clock on to the next quarter"},
    {"give", "/give <item> [count]", "put something in the pack"},
    {"tp", "/tp <x> <y>", "move the character"},
    {"spawn", "/spawn <mob> [count]", "put a creature down beside the character"},
    {"wind", "/wind [px/s|auto]", "read the air here, or pin it at a speed"},
    {"clear", "/clear", "empty this log"},
};

// The words a command will take, gathered from the same tables the parser matches
// against.
//
// That is the whole point of building the list rather than writing it: a mood added
// to the sky's table, or an item added to the item table, turns up in the help and
// in the refusal message without either being touched. A help text kept by hand
// goes stale on the first change and is then worse than none, because it is
// believed.
std::vector<std::string> Choices(const std::string &verb, const World &world) {
    std::vector<std::string> out;

    if (verb == "spawn") {
        for (const mob::Def *def : mob::kinds::Table().All()) out.push_back(Lower(def->name));

        return out;
    }

    if (verb == "weather") {
        for (int i = 0; i < weather::kMoodCount; i++) out.push_back(Lower(world.Sky().MoodNamed(i)));

        // Not a row of the table, but a thing the command takes: hand the sky back
        // to its own timer.
        out.emplace_back("auto");
    } else if (verb == "wind") {
        out.emplace_back("a speed in px/s, negative to blow left");
        out.emplace_back("auto");
    } else if (verb == "season") {
        for (const char *name : flora::kSeasonNames) out.emplace_back(name);
    } else if (verb == "give") {
        // Both tables, items first and then the materials, and the join is the
        // command's whole index: anything below the item count is a row of the item
        // table and anything above it is a row of the element table. A hand can hold
        // either — see Stack::holds — so a give that reached only one of them could
        // not fill a slot the player can plainly carry soil in.
        for (int i = 0; i < item::Count(); i++) out.push_back(Lower(Def(Item{i}).name));

        for (std::size_t i = 0; i < kElementCount; i++) out.push_back(Lower(StyleOf(static_cast<Element>(i)).name));
    }

    return out;
}

std::string Joined(const std::vector<std::string> &words) {
    std::string out;

    for (std::size_t i = 0; i < words.size(); i++) {
        if (i > 0) out += (i + 1 == words.size()) ? " or " : ", ";

        out += words[i];
    }

    return out;
}

// The wind in words, against the hardest this world can blow.
//
// A share is the honest number and a useless answer: nobody knows whether 0.44 is a
// lot. The bands are the Beaufort idea reduced to what this world actually has —
// six steps across a range whose top is a fixed, measured figure, so the words mean
// the same thing on every afternoon rather than drifting with the weather.
const char *Strength(float share) {
    if (share < 0.08f) return "dead calm";
    if (share < 0.20f) return "barely a breath";
    if (share < 0.38f) return "a light breeze";
    if (share < 0.58f) return "a fresh wind";
    if (share < 0.80f) return "blowing hard";

    return "a full gale";
}

// What one command takes, said the same way whether it was asked for or arrived at
// by getting the command wrong. A player who mistypes an argument has just proved
// they wanted this list.
void SayUsage(const std::string &verb, const World &world, console::Console &chat) {
    for (const Command &command : kCommands) {
        if (verb != command.verb) continue;

        chat.Say(std::string{command.shape} + " — " + command.what, console::Tone::Note);

        const std::vector<std::string> takes = Choices(verb, world);

        if (!takes.empty()) chat.Say("  takes: " + Joined(takes), console::Tone::Note);

        return;
    }

    chat.Say("no command called /" + verb + " — /help lists them", console::Tone::Failed);
}

} // namespace

void commands::Run(const std::string &line, World &world, Grove &grove, Inventory &inventory, Player &player,
                   mob::Herd &herd, Camera2D &camera, console::Console &chat) {
    // Anything not starting with a slash is talk rather than an instruction. There
    // is nobody to talk to yet, but the distinction is the one every chat box makes
    // and building it in now costs a branch.
    if (line.empty() || line[0] != '/') {
        chat.Say(line);
        return;
    }

    const std::vector<std::string> words = Words(line.substr(1));

    if (words.empty()) {
        chat.Say("type a command after the slash — /help lists them", console::Tone::Failed);
        return;
    }

    const std::string verb = Lower(words[0]);

    const auto arg = [&](std::size_t n) { return (words.size() > n) ? Lower(words[n]) : std::string{}; };

    // Whatever this command takes, for the refusals below. Asked once here so that
    // no branch can answer with a list that disagrees with the one /help gives.
    const std::vector<std::string> takes = Choices(verb, world);

    if (verb == "help") {
        if (!arg(1).empty()) {
            SayUsage(arg(1), world, chat);
            return;
        }

        for (const Command &command : kCommands) {
            chat.Say(std::string{command.shape} + " — " + command.what, console::Tone::Note);
        }

        chat.Say("/help <command> for what one of them takes", console::Tone::Note);
        return;
    }

    if (verb == "clear") {
        chat.Wipe();
        return;
    }

    if (verb == "weather") {
        if (arg(1).empty()) {
            chat.Say(std::string{"it is "} + world.Sky().Now().name, console::Tone::Note);
            return;
        }

        if (arg(1) == "auto") {
            world.ForceWeather(-1);
            chat.Say("weather back on its own timer", console::Tone::Done);
            return;
        }

        // The moods alone, which is `takes` without the `auto` dealt with above.
        std::vector<const char *> raw;

        for (int i = 0; i < weather::kMoodCount; i++) raw.push_back(takes[static_cast<std::size_t>(i)].c_str());

        const int found = Named(arg(1), raw.data(), static_cast<int>(raw.size()));

        if (found == -2) {
            chat.Say(arg(1) + " could be more than one weather — " + Joined(takes), console::Tone::Failed);
            return;
        }

        if (found < 0) {
            chat.Say("no weather called " + arg(1), console::Tone::Failed);
            SayUsage(verb, world, chat);
            return;
        }

        world.ForceWeather(found);
        chat.Say("weather held at " + takes[static_cast<std::size_t>(found)], console::Tone::Done);
        return;
    }

    if (verb == "season") {
        const int found = Named(arg(1), flora::kSeasonNames, 4);

        if (found < 0) {
            chat.Say(arg(1).empty() ? "which season?" : "no season called " + arg(1), console::Tone::Failed);
            SayUsage(verb, world, chat);
            return;
        }

        world.SetSeason(found);
        chat.Say(std::string{"season held at "} + flora::kSeasonNames[found], console::Tone::Done);
        return;
    }

    if (verb == "time") {
        world.SkipToQuarter();
        chat.Say("running the clock on to the next quarter", console::Tone::Done);
        return;
    }

    if (verb == "wind") {
        if (!arg(1).empty()) {
            if (arg(1) == "auto") {
                world.ReleaseWind();
                chat.Say("wind back on the weather's own figure", console::Tone::Done);
                return;
            }

            // Parsed rather than matched, so a refusal has to be spelt out here:
            // everything else this command takes is a word, and `Named` has nothing
            // to compare a number against.
            const std::string &raw = words[1];

            const bool number = raw.find_first_not_of("+-0123456789.") == std::string::npos
                                && raw.find_first_of("0123456789") != std::string::npos;

            if (!number) {
                chat.Say(arg(1) + " is not a speed", console::Tone::Failed);
                SayUsage(verb, world, chat);
                return;
            }

            const auto speed = static_cast<float>(std::atof(raw.c_str()));

            world.SetWind(speed);

            // Said back as a share as well as a figure, because the figure alone
            // does not say whether it is a lot — which is the whole reason the
            // reading below is worded the way it is.
            chat.Say(TextFormat("wind pinned at %.0f px/s %s — %s", std::fabs(speed),
                                (speed < 0.0f) ? "to the left" : "to the right",
                                Strength(std::fabs(speed) / std::max(world.Sky().Gale(), 1e-3f))),
                     console::Tone::Done);

            if (std::fabs(speed) > world.Sky().Gale()) {
                chat.Say(TextFormat("  that is past this world's own gale of %.0f — everything rooted will sit at "
                                    "full lean",
                                    world.Sky().Gale()),
                         console::Tone::Note);
            }

            return;
        }

        const Vector2 at = player.Centre();

        const float here  = world.Sky().WindAt(at.x);
        const float sky   = world.Sky().Mean();
        const float share = std::fabs(world.Sky().PushAt(at.x));

        // Three lines and three questions, which is what this command was failing to
        // separate: it used to put every figure on one line in the units the module
        // happens to think in, and a player reading "push +0.94 of a gale of 84" has
        // been handed the implementation rather than an answer.
        //
        // Which way, how hard, and whether *here* is the same as everywhere — a gust
        // is a wave crossing the world, so the last of those is a real question and
        // the one number that explains why the tree beside you is bent further than
        // the one across the valley.
        const char *way = (std::fabs(here) < 1.0f)   ? "turning, going nowhere"
                          : (here > 0.0f)            ? "blowing to the right"
                                                     : "blowing to the left";

        chat.Say(TextFormat("wind: %s at %.0f px/s — %s", way, std::fabs(here), Strength(share)),
                 console::Tone::Note);

        // Comparing the column against the sky's own mean is what tells a gust from
        // a lull. A tenth either way is the noise floor of the gust field, so
        // anything inside that is reported as neither.
        const float over = std::fabs(here) - std::fabs(sky);

        // A tenth of the mean is the noise floor of the gust field, with a pixel a
        // second under it as an absolute floor: the mean passes through zero every
        // time the wind comes round, and a share of nothing calls every rounding
        // error a gust.
        const float notice = std::max(std::fabs(sky) * 0.10f, 1.0f);

        const char *gusting = (over > notice)    ? "a gust is passing here"
                              : (over < -notice) ? "this spot is in a lull"
                                                 : "steady here";

        chat.Say(TextFormat("  %s — %.0f here against %.0f across the whole sky", gusting, std::fabs(here),
                            std::fabs(sky)),
                 console::Tone::Note);

        chat.Say(TextFormat("  %.0f%% of a full gale, which here is %.0f px/s. the weather is %s, worth %.0f on its own",
                            share * 100.0f, world.Sky().Gale(), world.Sky().Now().name, world.Sky().Now().wind),
                 console::Tone::Note);

        // Worth saying outright. A pinned wind that goes on ignoring the weather is
        // indistinguishable from a broken one, and the player who pinned it may have
        // done so an hour ago.
        if (world.Sky().WindHeld()) {
            chat.Say("  pinned by hand — /wind auto hands it back to the weather", console::Tone::Note);
        }

        return;
    }

    if (verb == "spawn") {
        if (words.size() < 2) {
            SayUsage(verb, world, chat);
            return;
        }

        // Named rather than numbered, and matched against the creature table itself
        // — so a creature added under `entity/mob/mobs/` can be spawned by name with
        // nothing here edited. The same arrangement /give already has with items.
        const std::optional<mob::Kind> kind = mob::kinds::Find(words[1].c_str());

        if (!kind.has_value()) {
            chat.Say(TextFormat("no creature called '%s'", words[1].c_str()), console::Tone::Failed);
            SayUsage(verb, world, chat);
            return;
        }

        const int many = (words.size() >= 3) ? std::max(1, std::atoi(words[2].c_str())) : 1;

        // Beside the character rather than on top of it: a creature put down inside
        // the player's own body is unstuck on its first frame and thrown somewhere
        // neither of them chose.
        const Vector2 from = player.Centre();

        int put = 0;

        for (int n = 0; n < many; n++) {
            const float side = ((n % 2) == 0) ? 1.0f : -1.0f;

            const Vector2 at = {from.x + side * (40.0f + 18.0f * static_cast<float>(n / 2)), from.y};

            if (herd.Put(*kind, at)) put++;
        }

        chat.Say(TextFormat("%d %s", put, mob::kinds::Of(*kind).name),
                 (put > 0) ? console::Tone::Done : console::Tone::Failed);
        return;
    }

    if (verb == "tp") {
        if (words.size() < 3) {
            SayUsage(verb, world, chat);
            return;
        }

        const Vector2 to = {static_cast<float>(std::atof(words[1].c_str())),
                            static_cast<float>(std::atof(words[2].c_str()))};

        player.PlaceAt(to);

        // The camera is put there too, rather than being left to catch up.
        //
        // FollowPlayer is an exponential approach, which is right for walking and
        // wrong for arriving: left to itself it flies the whole distance, so a
        // teleport across the world reads as a very fast journey rather than as not
        // having made one. The body was always instant; this is the half that was
        // not.
        camera.target = player.Centre();

        chat.Say(TextFormat("moved to %.0f, %.0f", to.x, to.y), console::Tone::Done);
        return;
    }

    if (verb == "give") {
        if (arg(1).empty()) {
            SayUsage(verb, world, chat);
            return;
        }

        // The count is the last word where it is a number, and everything between
        // the verb and it is the name — which is what lets an item whose name is two
        // words be asked for by name.
        std::string wanted = arg(1);
        int count          = 1;

        if (words.size() > 2) {
            const std::string tail = arg(words.size() - 1);

            const bool number = !tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) {
                                    return std::isdigit(c) != 0;
                                });

            const std::size_t last = number ? words.size() - 1 : words.size();

            if (number) count = std::max(std::atoi(tail.c_str()), 1);

            for (std::size_t i = 2; i < last; i++) wanted += " " + arg(i);
        }

        std::vector<const char *> raw;

        for (const std::string &name : takes) raw.push_back(name.c_str());

        const int found = Named(wanted, raw.data(), static_cast<int>(raw.size()));

        if (found == -2) {
            chat.Say(wanted + " could be more than one thing — " + Joined(takes), console::Tone::Failed);
            return;
        }

        if (found < 0) {
            chat.Say("nothing here is called " + wanted, console::Tone::Failed);
            SayUsage(verb, world, chat);
            return;
        }

        // Which of the two tables the match landed in — see Choices.
        const bool material = found >= item::Count();

        const int row = material ? found - item::Count() : found;

        const int over = inventory.Add({.holds = material ? Holds::Material : Holds::Item,
                                        .what  = static_cast<std::uint8_t>(row),
                                        .count = count});

        if (over >= count) {
            chat.Say("no room for any of that", console::Tone::Failed);
            return;
        }

        const char *name = material ? StyleOf(static_cast<Element>(row)).name : Def(static_cast<Item>(row)).name;

        chat.Say(TextFormat("gave %d %s", count - over, name), console::Tone::Done);
        return;
    }

    (void)grove;

    SayUsage(verb, world, chat);
}
