#include "backdrop.h"
#include "config.h"
#include "debug_view.h"
#include "editor.h"
#include "grove.h"
#include "hotbar.h"
#include "light_layer.h"
#include "liquid_layer.h"
#include "player.h"
#include "profile.h"
#include "console.h"
#include "scuff.h"
#include "sod.h"
#include "soil.h"
#include "raylib.h"
#include "terrain.h"
#include "weather.h"
#include "world.h"

#include <algorithm>
#include <string>
#include <cctype>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr const char *kSeasonNames[] = {"spring", "summer", "autumn", "winter"};

// Seconds a refusal stays on screen.
constexpr float kNoticeTime = 2.2f;

// Fraction of the distance to the player the camera closes per second. Framing
// the character with a slight lag reads as smoother than pinning the view to
// the body, which makes every jump shake the whole screen.
constexpr float kCameraFollow = 8.0f;

// The liquid automaton advances in fixed increments. Feeding it the frame time
// would make a long frame move liquid several cells at once, which the flow
// limits are not built to absorb.
constexpr float kWaterStep = 1.0f / 60.0f;

// Upper bound on the time carried into the next frame, so a stall does not
// queue up hundreds of steps and stall the frame after it as well.
constexpr float kMaxAccumulated = 0.25f;

// Liquid is simulated over a band wider than the view, so that what happens
// just off screen has already settled by the time it scrolls in.
//
// Wider than a chunk, which is what the promise above actually requires and what
// a hundred and twenty-eight pixels did not deliver: a chunk is a hundred and
// ninety-two, so at the old figure a chunk could be created and scroll into view
// without a single vertex of it ever having been stepped.
//
// It also has to stay inside the band World::Update generates, or StepWater reads
// through to freshly generated noise for the parts outside it — see the reserve
// there, which is sized against this.
constexpr float kSimulationMargin = 256.0f;

// The lantern as linear light, at a given strength.
light::Radiance Lantern(float strength) {
    constexpr float kByte = 1.0f / 255.0f;

    return {config::kLanternGlow.r * kByte * strength, config::kLanternGlow.g * kByte * strength,
            config::kLanternGlow.b * kByte * strength};
}

// The world region the frame covers. Read from the window rather than from the
// configured size, so a resized window shows more of the world instead of the same
// amount of it stretched.
// Divided by the zoom, so that what it describes is the ground the frame covers
// rather than the pixels it is drawn with. Zoomed in, that is less world for the
// same window — which is exactly what everything reading this wants to be told,
// since a chunk off the edge of a zoomed-in view is a chunk nobody has to
// generate, light or grow grass on.
Rectangle ViewBounds(const Camera2D &camera) {
    const float zoom = (camera.zoom > 0.0f) ? camera.zoom : 1.0f;

    const float width  = static_cast<float>(GetScreenWidth()) / zoom;
    const float height = static_cast<float>(GetScreenHeight()) / zoom;

    return {camera.target.x - camera.offset.x / zoom, camera.target.y - camera.offset.y / zoom, width, height};
}

// Half-side of the box an aimed swing lands in, in world pixels.
//
// A little over the slack the cursor uses to *choose* the axe, so that a click the
// cursor accepted is a click that connects. The two are separate figures because
// they answer separate questions — what the hand is willing to aim at, and what the
// blow covers — and tying them together would make widening the aim quietly widen
// the axe.
constexpr float kAimedBlow = 12.0f;


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

    if (verb == "weather") {
        for (int i = 0; i < weather::kMoodCount; i++) out.push_back(Lower(world.Sky().MoodNamed(i)));

        // Not a row of the table, but a thing the command takes: hand the sky back
        // to its own timer.
        out.emplace_back("auto");
    } else if (verb == "wind") {
        out.emplace_back("a speed in px/s, negative to blow left");
        out.emplace_back("auto");
    } else if (verb == "season") {
        for (const char *name : kSeasonNames) out.emplace_back(name);
    } else if (verb == "give") {
        // Both tables, items first and then the materials, and the join is the
        // command's whole index: anything below kItemCount is a row of the item
        // table and anything above it is a row of the element table. A hand can hold
        // either — see Stack::holds — so a give that reached only one of them could
        // not fill a slot the player can plainly carry soil in.
        for (std::size_t i = 0; i < kItemCount; i++) out.push_back(Lower(Def(static_cast<Item>(i)).name));

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

void RunCommand(const std::string &line, World &world, Grove &grove, Inventory &inventory, Player &player,
                Camera2D &camera, console::Console &chat) {
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
        const int found = Named(arg(1), kSeasonNames, 4);

        if (found < 0) {
            chat.Say(arg(1).empty() ? "which season?" : "no season called " + arg(1), console::Tone::Failed);
            SayUsage(verb, world, chat);
            return;
        }

        world.SetSeason(found);
        chat.Say(std::string{"season held at "} + kSeasonNames[found], console::Tone::Done);
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
        const bool material = found >= static_cast<int>(kItemCount);

        const int row = material ? found - static_cast<int>(kItemCount) : found;

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


Rectangle Expand(Rectangle rect, float margin) {
    return {rect.x - margin, rect.y - margin, rect.width + 2.0f * margin, rect.height + 2.0f * margin};
}

PlayerInput ReadPlayerInput(const Camera2D &camera, bool chopping) {
    PlayerInput input;

    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) input.moveX -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) input.moveX += 1.0f;

    input.jumpPressed   = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_W);
    input.jumpHeld      = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W);
    input.crouchHeld    = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
    // The key remains, and the mouse is the other way in. `chopping` is the left
    // button held over a trunk — held rather than pressed, so laying into a tree is
    // one held button and not a drumroll; what stops it becoming one blow a frame is
    // the swing's own cooldown, which is where that rule already lived.
    input.attackPressed = IsKeyPressed(KEY_J) || chopping;

    // The vertical axis is the same two keys as jump and crouch. Only flight
    // reads it, and while flying neither of those actions applies, so there is
    // nothing for it to conflict with.
    if (input.jumpHeld) input.moveY -= 1.0f;
    if (input.crouchHeld) input.moveY += 1.0f;

    input.flyToggled = IsKeyPressed(KEY_F);
    input.sprintHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    input.aimWorld = GetScreenToWorld2D(GetMousePosition(), camera);

    return input;
}

// Whether the hand is asking for the view rather than for the bar.
//
// The wheel already steps through the hotbar, and control-wheel is what every
// other program on the machine zooms with, so the modifier is read in one place
// and both readers are told about it — otherwise a zoom would also change the
// slot in hand.
bool ZoomModifier() {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER)
        || IsKeyDown(KEY_RIGHT_SUPER);
}

// Steps the view in and out, in whole multiples and nothing between them.
//
// Whole, and that is not a matter of taste. Everything in the world is drawn on
// one of two pixel grids — five world units for the terrain's outline, two for a
// plant's texel — and both are chosen to be a whole number of screen pixels. A
// multiplier of one and a half turns the first into seven and a half, which
// rasterises as columns alternating seven pixels wide and eight, and that is the
// one thing art at this size may never do. At a whole multiple every texel keeps
// its shape whatever the view is doing, so the picture zooms rather than
// resamples.
//
// One is the floor because it is what the world was framed against: it shows the
// most ground of any setting, and there is nothing to be gained by pulling
// further back except a character too small to read. Everything above it is the
// player's own comfort.
void ReadZoom(Camera2D &camera) {
    int level = static_cast<int>(std::lround(camera.zoom));

    if (IsKeyPressed(KEY_PAGE_UP)) level++;
    if (IsKeyPressed(KEY_PAGE_DOWN)) level--;

    if (ZoomModifier()) {
        const float wheel = GetMouseWheelMove();

        if (wheel > 0.0f) level++;
        if (wheel < 0.0f) level--;
    }

    camera.zoom = static_cast<float>(std::clamp(level, config::kMinZoom, config::kMaxZoom));
}

void FollowPlayer(Camera2D &camera, const Player &player, float dt) {
    const Vector2 target = player.Centre();

    // Exponential approach, expressed so that the rate is the same whatever the
    // frame duration.
    const float t = 1.0f - std::exp(-kCameraFollow * dt);

    camera.target.x += (target.x - camera.target.x) * t;
    camera.target.y += (target.y - camera.target.y) * t;
}

// A line of the readout, dark with a pale edge under it.
//
// Grey on its own was unreadable, and measurably so rather than as a matter of
// taste: the daytime sky at the top of the screen sits at very nearly the same
// brightness as GRAY does, so the text and the background were the same value and
// only the letter shapes separated them. Black alone fixes the day and breaks the
// night, which is the same fault the other way up. The shadow is what makes one
// colour work against a bright sky, a dark one, wet rock and open water alike,
// because it supplies the contrast the background refuses to.
//
// Drawn twice. That is the whole cost, on six lines of text.
void DrawLabel(const char *text, int x, int y, Color colour) {
    // Outlined on all four sides rather than given a shadow on one.
    //
    // This was dark ink with a pale offset a pixel down and right, and it failed
    // at both ends of the world it has to be read against. Over white cloud the
    // pale half vanished into the cloud and what was left was thin dark letters;
    // in a cave the dark half vanished and what was left was the offset. And a
    // single offset is not an outline — half of every letter has nothing between
    // it and the background at all.
    //
    // Light letter, dark outline, and the two cover each other's ground: the
    // letter carries a cave and the outline carries a cloud. Five draws on six
    // lines of text, which is nothing next to a screenful of terrain.
    constexpr Color kEdge = {8, 10, 14, 235};

    DrawText(text, x - 1, y, 14, kEdge);
    DrawText(text, x + 1, y, 14, kEdge);
    DrawText(text, x, y - 1, 14, kEdge);
    DrawText(text, x, y + 1, 14, kEdge);

    DrawText(text, x, y, 14, colour);
}

// Badge showing how wide the brush is, next to the bar that decides what it
// works with.
//
// Size is the one property of the brush with no mark of its own out in the
// world — the ring shows where it is and what it would put down, but not what
// the last press of the size key did, since a ring four pixels wider is not a
// thing anyone sees change.
void DrawBrushSize(const Editor &editor) {
    // Grey out of reach, which is the same thing the ring in the world says and
    // is worth saying twice: out there the ring is the only mark on screen, and
    // a player who has not noticed it is a player wondering why the button
    // stopped working.
    const Color color = editor.Reachable() ? Color{190, 198, 212, 255} : Color{120, 126, 138, 255};

    const char *text = TextFormat("brush %.0f  (- / +)", editor.Radius());
    const int width  = MeasureText(text, 14);

    // Sat just clear of the bar, which reaches 76 pixels up from the bottom.
    const Rectangle badge = {(GetScreenWidth() - width) / 2.0f - 8.0f, GetScreenHeight() - 104.0f, width + 16.0f,
                             22.0f};

    DrawRectangleRec(badge, {30, 34, 42, 220});
    DrawRectangleLinesEx(badge, 2.0f, color);
    DrawText(text, static_cast<int>(badge.x + 8.0f), static_cast<int>(badge.y + 4.0f), 14, color);
}

// One strip of the world drawn straight to a file, at one screen pixel per world
// pixel.
//
// The offline probe this project was tuned with lived outside the repository and
// went with it. This is its replacement, and it is inside the game on purpose:
// what it draws is the world these settings describe rather than a copy of them
// kept in step by hand, and a probe that has to be maintained alongside what it
// measures is a probe that will one day be measuring something else.
//
// Drawn unlit, for the same reason F6 exists — what a material's own colour is
// doing and what the light is doing to it are two questions, and answering them
// together is how a palette gets tuned against a time of day.
void DrawProbe(World &world, Grove &grove, Inventory &gathered, Rectangle strip, const char *path, int zoom,
               float seconds, bool plants, int lit, int mood, int quarter) {
    // Held at one weather and one season where asked for, and this is what makes the
    // probe worth anything for judging the wind: what a gale does is only legible
    // against the calm afternoon beside it, and two pictures taken of whatever the
    // spell happened to be doing are not a comparison. Taken before the clock is run
    // on, so the whole of that run happens under the weather being asked about.
    if (mood >= 0) world.ForceWeather(mood);
    for (int step = 0; step <= quarter; step++) world.CycleSeason();

    // Run the clock on before drawing, so a still picture can be taken of a world
    // that has been blowing for a while. The sway and the gust are both pure
    // functions of this clock, so two probes a second apart are two frames of the
    // same wind rather than two unrelated pictures.
    // Always at least one step, whatever was asked for: what the sky is giving
    // off is worked out by StepWeather and is nothing until it has run, so a
    // probe that skipped it would draw every lit picture at midnight.
    world.StepWeather(1.0f / 60.0f);

    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) world.StepWeather(1.0f / 60.0f);

    world.Update(strip);

    // The ground's own pictures, before the probe opens a target of its own —
    // and deliberately, so that what this draws is the cached path the game
    // takes rather than the fallback beside it.
    world.PaintChunks(strip);

    RenderTexture2D canvas = LoadRenderTexture(static_cast<int>(strip.width), static_cast<int>(strip.height));

    Camera2D camera = {};
    camera.offset   = {0.0f, 0.0f};
    camera.target   = {strip.x, strip.y};
    camera.zoom     = 1.0f;

    BeginTextureMode(canvas);

    // The sky's own colour, so the silhouette of the ground reads against
    // something rather than against black.
    ClearBackground({92, 132, 176, 255});

    // The plants too, and in the order the frame draws them, because half the
    // things worth checking here are about whether two of these agree with each
    // other about where the ground is.
    if (plants) grove.Update(world, strip, {strip.x, strip.y}, world.Sky().Time(), 1.0f / 60.0f, gathered);

    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    BeginMode2D(camera);
    world.DrawTerrain(strip);
    sod::DrawTufts(world.Grass(), strip, world.Sky().Time(), world.Settings().seed);

    // Left out on request, so that a check for something drawn clear of the
    // ground has only the ground and the grass to look at. A fern is full of
    // holes by design, and any test that cannot tell one of those from a tuft
    // hanging in the air will report the wrong thing every time.
    if (plants) grove.Draw(world.Sky(), season, world.Sky().Time());

    // The fruit and the falling leaves too, in the frame's own order. Left out of
    // this probe for a long time, which is exactly why the leaf field could be
    // judged only by eye in a live window — the one part of the world whose whole
    // point is how it answers the weather had no still picture to be looked at.
    if (plants) {
        grove.DrawFruit(world.Sky(), season, world.Sky().Time());
        grove.DrawLeaves(world.Sky(), season, strip, world.Sky().Time());
    }

    world.DrawLiquids(strip);

    // What is falling, and then the fog, in the frame's own order.
    //
    // Both are pure functions of the clock and the weather and the probe can force
    // both, so this is the only way to look at a storm — or at a shower coming down
    // as snow, which needs cold country as well as a storm and is otherwise a thing
    // that has to be waited for in two places at once.
    world.DrawRain(strip);
    world.DrawMist(strip);

    // And the light over all of it, which is the other half of every question
    // about how dark something came out: what a material's own tones do and what
    // the multiply does to them are two answers, and tuning either while looking
    // at both together is how a palette ends up fighting a time of day.
    if (lit != 0) {
        // The canopies first, exactly as the frame does it. Shade is re-offered
        // every frame and is gone the moment nobody offers it, so a probe that
        // skipped this would be measuring a world with no woods in it.
        if (plants) grove.Shade(world, world.Sky().Time());

        world.StepLight(strip);

        // One draws the world as it is seen; the other draws the light on its
        // own, one flat block per probe. The second is the one to reach for when
        // the question is where the light is rather than what it is doing to a
        // palette, because a picture of the two multiplied together cannot answer
        // either of them on its own.
        if (lit == 2) {
            debug_view::DrawLight(world, strip);
        } else {
            LightLayer probeLight;
            probeLight.Update(world.Light());
            probeLight.Compose();
        }
    }

    EndMode2D();

    EndTextureMode();

    Image image = LoadImageFromTexture(canvas.texture);

    // A render texture is filled bottom row first, so what comes back off it is
    // upside down.
    ImageFlipVertical(&image);

    // Nearest neighbour, because the whole subject is where one texel ends and
    // the next begins. Anything that filters would answer a question about the
    // tones by inventing colours that are not in the ramp.
    if (zoom > 1) ImageResizeNN(&image, image.width * zoom, image.height * zoom);

    ExportImage(image, path);

    UnloadImage(image);
    UnloadRenderTexture(canvas);
}

// What the covers actually do over a stretch of world, measured rather than
// guessed.
//
// A cover's extent is decided by a climate bell, and a bell is very easy to
// author into a material that is simply never there: nothing errors, nothing is
// drawn, and the only symptom is a world with no deserts in it. This walks the
// columns and reports the share of them each cover claims and how thick it gets,
// which is the same treatment terrain::Calibrate gives cave coverage and for the
// same reason.
void ReportCovers(const terrain::Settings &settings, float fromX, float toX, float step) {
    struct Tally {
        double thickness = 0.0;
        float deepest    = 0.0f;
        float where      = 0.0f;
        int columns      = 0;
    };

    std::array<Tally, kElementCount> tally{};

    float coldest = 1.0f;
    float hottest = 0.0f;
    float driest  = 1.0f;
    float wettest = 0.0f;

    int columns = 0;

    float highest = kUnboundedDepth;
    float lowest  = -kUnboundedDepth;

    for (float x = fromX; x <= toX; x += step, columns++) {
        const terrain::Climate climate = terrain::ClimateAt(x, settings);
        const float surface            = terrain::Height(x, settings);

        coldest = std::min(coldest, climate.temperature);
        hottest = std::max(hottest, climate.temperature);
        driest  = std::min(driest, climate.humidity);
        wettest = std::max(wettest, climate.humidity);

        // Y grows downward, so the highest ground is the smallest number.
        highest = std::min(highest, surface);
        lowest  = std::max(lowest, surface);

        for (std::size_t e = 0; e < kElementCount; e++) {
            const ElementSpawn &spawn = kElements[e].spawn;
            if (spawn.generator != Generator::Cover) continue;

            const float thickness = CoverThickness(spawn, x, surface, climate.temperature, climate.humidity);

            // A cover thinner than one terrain pixel is not on the ground, it is
            // a rounding error, and counting it would report a desert nobody can
            // see.
            if (thickness < config::kPixelSize) continue;

            Tally &into = tally[e];

            into.thickness += thickness;
            into.columns++;

            if (thickness > into.deepest) {
                into.deepest = thickness;
                into.where   = x;
            }
        }
    }

    std::printf("%d columns over %.0f px, every %.0f\n", columns, toX - fromX, step);
    std::printf("temperature %.2f..%.2f   humidity %.2f..%.2f\n", coldest, hottest, driest, wettest);

    // The ground's own range, because a cover with a crest is measured against it
    // and a snow line written above the highest peak in the world is a material
    // that never appears — with nothing anywhere to say why.
    std::printf("surface y %.0f..%.0f   (level %.0f, so %.0f px of relief above it)\n\n", highest, lowest,
                settings.surface.level, settings.surface.level - highest);

    for (std::size_t e = 0; e < kElementCount; e++) {
        if (kElements[e].spawn.generator != Generator::Cover) continue;

        const Tally &t = tally[e];

        if (t.columns == 0) {
            std::printf("%-6s  never\n", kElements[e].name);
            continue;
        }

        std::printf("%-6s  %5.1f%% of columns   mean %4.1f px   deepest %4.1f px at x %.0f\n", kElements[e].name,
                    100.0 * t.columns / std::max(columns, 1), t.thickness / t.columns, t.deepest, t.where);
    }
}

// What the scatter actually grows over a stretch of world, and what it grows it
// on.
//
// The companion to ReportCovers, and it exists for exactly the fault ReportCovers
// exists for: a placement rule written into a table is very easy to author into a
// wood that is never there or a desert that is quietly full of oaks, and nothing
// errors either way. The only symptom of the second is a screenshot, which nobody
// takes of the one stretch of world where it is wrong.
//
// It walks the cells rather than the columns, because a cell is what the scatter
// decides one plant per, and it reports the ground under each plant so that
// "trees in the desert" is a number rather than an impression.
void ReportWoods(const flora::Settings &flora, const terrain::Settings &terrain, float fromX, float toX) {
    struct Tally {
        int grown = 0;
        int onSoil = 0;
        int onSand = 0;
        int onSnow = 0;
        int onRock = 0;
    };

    std::array<Tally, flora::kSpeciesCount> tally{};

    // The scatter wants a surface to stand its plants on. Taken from the shape of
    // the land alone rather than from a world, since what is being measured is the
    // placement rule and not what anybody has dug.
    constexpr float kStep = 6.0f;

    const int columns = static_cast<int>((toX - fromX) / kStep) + 3;

    std::vector<float> top(static_cast<std::size_t>(columns));

    for (int i = 0; i < columns; i++) {
        top[static_cast<std::size_t>(i)] = terrain::Height(fromX + static_cast<float>(i) * kStep, terrain);
    }

    const flora::Ground ground = {
        .top = top.data(), .sunk = nullptr, .count = columns, .originX = fromX, .spacing = kStep};

    std::array<int, kElementCount + 1> cells{};

    std::vector<flora::Plant> plants;

    for (std::size_t l = 0; l < flora::kLayerCount; l++) {
        const auto layer = static_cast<flora::Layer>(l);

        flora::Scatter(layer, fromX, toX, flora, terrain, ground, plants);

        for (const flora::Plant &plant : plants) {
            Tally &into = tally[flora::SpeciesIndex(plant.species)];

            into.grown++;

            const std::optional<Element> cover = SurfaceCoverAt(plant.base.x, terrain);

            if (!cover.has_value()) into.onRock++;
            else if (*cover == Element::Soil) into.onSoil++;
            else if (*cover == Element::Sand) into.onSand++;
            else if (*cover == Element::Snow) into.onSnow++;
        }
    }

    // And how much of the world is each kind of ground, so a species count can be
    // read against the room there was for it.
    int walked = 0;

    for (float x = fromX; x <= toX; x += 40.0f, walked++) {
        const std::optional<Element> cover = SurfaceCoverAt(x, terrain);

        cells[cover.has_value() ? ElementIndex(*cover) : kElementCount]++;
    }

    std::printf("%.0f px of world   soil %.1f%%  sand %.1f%%  snow %.1f%%  bare %.1f%%\n\n", toX - fromX,
                100.0 * cells[ElementIndex(Element::Soil)] / std::max(walked, 1),
                100.0 * cells[ElementIndex(Element::Sand)] / std::max(walked, 1),
                100.0 * cells[ElementIndex(Element::Snow)] / std::max(walked, 1),
                100.0 * cells[kElementCount] / std::max(walked, 1));

    for (std::size_t e = 0; e < flora::kSpeciesCount; e++) {
        const Tally &t = tally[e];

        std::printf("%-6s  %6d grown   on soil %6d   sand %6d   snow %6d   bare rock %6d\n", flora::kSpecies[e].name,
                    t.grown, t.onSoil, t.onSand, t.onSnow, t.onRock);
    }
}

// How each material's paint divides itself, measured rather than argued.
//
// Three numbers per material, all in ramp steps, and each one answers a rule the
// art has to obey:
//
//  - `form` is how far apart a lit top face and a shaded underside come out. This
//    is the term that has to carry the picture, and it is the only one allowed to
//    move a texel more than a step.
//  - `stipple` is how far apart two neighbouring texels come out *inside* the
//    material, where there is no face and the texture is all there is. The rule
//    is that this stays under one step: a stipple breaks the boundary between two
//    tones, and past a step it stops breaking a boundary and starts drawing one.
//  - `jumps` is the share of neighbouring pairs that land two whole tones apart,
//    which is what the eye reads as static rather than as texture. It is the
//    number that caught the first setting here: grain was authored at 0.72 steps,
//    which is under a step and looked reasonable in the table, but two neighbours
//    at opposite ends of it are 1.44 apart and a fifth of them crossed two tones.
void ReportTones() {
    constexpr int kAcross = 200;
    constexpr int kDown   = 60;

    std::printf("%-8s %7s %8s %8s %7s %7s\n", "", "form", "across", "down", "jump-x", "jump-y");

    for (const ElementDef &def : kElements) {
        const soil::Paint paint = soil::For(def, 0);

        // A face at its most extreme either way: the top of a ledge and the belly
        // of an overhang, both hard against the surface.
        const marching_squares::Texel top{{0.0f, 0.0f}, 0.0f, {0.0f, -1.0f}};
        const marching_squares::Texel under{{0.0f, 0.0f}, 0.0f, {0.0f, 1.0f}};

        const float form = (soil::Shading(paint, top).form - soil::Shading(paint, under).form) / soil::kStep;

        // Then the inside, where the form is nothing and the texture is the whole
        // of what is drawn. Sampled on the terrain's own texel grid, because that
        // is the grid the grain is quantised to and any other spacing would
        // measure a different picture from the one drawn.
        //
        // Both axes, and that is not symmetry for its own sake: the bedding term
        // is stretched flat by kStrataAspect, so it barely changes along a row and
        // changes fast down a column. Measured across only, it does not appear at
        // all — which is exactly the reading that would let a material be authored
        // into horizontal stripes and pass.
        std::vector<float> lit(static_cast<std::size_t>(kAcross) * kDown);

        for (int j = 0; j < kDown; j++) {
            for (int i = 0; i < kAcross; i++) {
                const Vector2 at = {static_cast<float>(i) * config::kPixelSize,
                                    static_cast<float>(j) * config::kPixelSize};

                lit[static_cast<std::size_t>(j) * kAcross + i] =
                    soil::Shading(paint, {at, kUnboundedDepth, {0.0f, -1.0f}}).Lit();
            }
        }

        double apart[2] = {0.0, 0.0};
        int pairs[2]    = {0, 0};
        int jumps[2]    = {0, 0};

        const auto compare = [&](float a, float b, int axis) {
            apart[axis] += std::fabs(a - b) / soil::kStep;
            pairs[axis]++;

            if (std::abs(static_cast<int>(a * kElementRamp) - static_cast<int>(b * kElementRamp)) >= 2) jumps[axis]++;
        };

        for (int j = 0; j < kDown; j++) {
            for (int i = 0; i < kAcross; i++) {
                const float here = lit[static_cast<std::size_t>(j) * kAcross + i];

                if (i > 0) compare(here, lit[static_cast<std::size_t>(j) * kAcross + i - 1], 0);
                if (j > 0) compare(here, lit[static_cast<std::size_t>(j - 1) * kAcross + i], 1);
            }
        }

        std::printf("%-8s %7.2f %8.2f %8.2f %6.1f%% %6.1f%%\n", def.name, form, apart[0] / std::max(pairs[0], 1),
                    apart[1] / std::max(pairs[1], 1), 100.0 * jumps[0] / std::max(pairs[0], 1),
                    100.0 * jumps[1] / std::max(pairs[1], 1));
    }
}

// Every material's field down one column, against the surface it is placed
// relative to.
//
// The question this answers is the one that cannot be answered by looking: two
// materials whose contours should meet on the same line, and a picture in which
// they plainly do not. A drawn edge is the field interpolated, thresholded and
// quantised onto texels, and any of those three can be where the discrepancy
// entered — so the field itself has to be readable on its own.
void ReportColumn(const World &world, const terrain::Settings &settings, float worldX, float fromY, float toY) {
    std::printf("x %.0f   surface y %.1f\n\n", worldX, terrain::Height(worldX, settings));

    std::printf("%8s %8s", "y", "depth");
    for (const ElementDef &def : kElements) std::printf(" %8s", def.name);
    std::printf("\n");

    for (float y = fromY; y <= toY; y += static_cast<float>(config::kResolution)) {
        std::printf("%8.0f %8.1f", y, terrain::Depth({worldX, y}, settings));

        for (std::size_t e = 0; e < kElementCount; e++) {
            const float value = world.ValueAt(static_cast<Element>(e), {worldX, y});

            // Printed as the distance from its own threshold, since that is what
            // decides whether the material is there and what the contour
            // interpolates through. Zero is the edge.
            std::printf(" %8.3f", value - kElements[e].threshold);
        }

        std::printf("\n");
    }
}

// The daylight reckoning down a run of columns.
//
// Prints, for each probe column, the first solid probe and what the two terms of
// the spread are giving it. What this is for is telling apart the two things that
// look identical on screen: a column the sun term never reached, and one it
// reached with a dark answer.
void ReportSun(const World &world, Rectangle region) {
    const light::Field &field = world.Light();

    std::printf("%6s %6s %8s %8s %8s\n", "col", "row", "solid", "depth", "sunlit");

    for (int i = 0; i < field.Cols(); i++) {
        int first = -1;

        for (int j = 0; j < field.Rows(); j++) {
            if (field.SolidAt(i, j) > 0.0f) {
                first = j;
                break;
            }
        }

        if (first < 0) continue;

        const Vector2 at = field.ProbePosition(i, first);
        if (at.x < region.x || at.x > region.x + region.width) continue;

        std::printf("%6.0f %6.0f %8.2f %8.1f %8.3f\n", at.x, at.y, field.SolidAt(i, first),
                    field.SunDepthAt(i, first), light::Luminance(field.SunlitAt(i, first)));
    }
}

// The wind each kind of weather actually produces, and what it does to the things
// that read it.
//
// This exists because the wind is the one field in the world whose whole purpose is
// a *difference* between two weathers, and a difference is the hardest thing to
// judge by eye — a wood in a gale looks windy on its own; only the calm afternoon
// beside it says whether the gale is doing anything. The failure this is built to
// catch is exactly that: the shares everything rooted to the ground reads were once
// normalised by the current weather's own envelope, so a storm and a clear day both
// came back about a half and every gale in the world was drawn as a breeze.
//
// The last two columns are the ones with a floor under them. The sway is drawn in
// whole plant texels, so a crown or a blade moving less than one of them does not
// move at all — a calm row whose lean rounds to zero is a wood that stands frozen,
// and no amount of looking at a storm will show it.
void ReportWind(World &world, Grove &grove, Inventory &gathered, Rectangle strip) {
    const float pixel = config::kFloraPixel;

    // The tallest thing that sways, so the sway is judged where it is largest. A
    // shorter tree moves proportionally less and hits the texel floor sooner.
    float tallest = 0.0f;

    for (std::size_t s = 0; s < flora::kSpeciesCount; s++) {
        const flora::SpeciesDef &def = flora::Def(static_cast<flora::Species>(s));
        tallest = std::max(tallest, def.height[flora::StageIndex(flora::Stage::Mature)]);
    }

    std::printf("gale %.1f px/s   tallest crown %.0f px   plant texel %.0f px\n\n", world.Sky().Gale(), tallest, pixel);

    std::printf("%-9s %7s %15s %9s %9s %9s %9s\n", "mood", "mean", "|push|", "held", "shake", "grass", "leaf");
    std::printf("%-9s %7s %15s %9s %9s %9s %9s\n", "", "px/s", "min..max", "px max", "px min", "px min", "px max");

    for (int mood = 0; mood < weather::kMoodCount; mood++) {
        world.ForceWeather(mood);
        world.StepWeather(1.0f / 60.0f);

        const weather::Sky &sky = world.Sky();

        float lowest = 1.0f, highest = 0.0f;

        // How far the wind holds a crown over, and how far it shakes one. They are
        // measured apart because they fail apart: the hold is what says a gale is
        // blowing, and the shake is the one that has to clear the texel grid or a
        // calm afternoon is a photograph.
        // The hold is taken at its largest and the shake at its smallest, because
        // that is where each one fails: a gale is judged by how far over it holds a
        // crown at its hardest, and a calm is judged by whether anything is still
        // moving at its quietest.
        float held  = 0.0f;
        float shook = 1e9f, blade = 1e9f;
        float leaf  = 0.0f;

        // Walked over both axes the field varies on. A gust is a wave crossing the
        // world, so a single column at a single moment is one point of it and says
        // nothing about the range; and the quarter turns on a clock of its own, so a
        // sweep that did not also run the clock would miss the lull entirely.
        // Long enough to carry the quarter round several whole turns, and stepped
        // finely enough not to skip over a gust crest on the way. A sweep shorter
        // than the backing period reports whatever the wind happened to be doing
        // during it and calls that the range.
        constexpr int kColumns = 64;
        constexpr int kSteps   = 2400;

        for (int step = 0; step < kSteps; step++) {
            for (int i = 0; i < kColumns; i++) {
                const float x = static_cast<float>(i) * 137.0f;

                const float push = sky.Stir(x);

                lowest  = std::min(lowest, push);
                highest = std::max(highest, push);

                // What the shares are worth once something reads them. Trees and
                // grass take the share; anything in flight takes the speed.
                //
                // The quiver keeps its floor here exactly as it does where it is
                // drawn, and that is the point of measuring rather than multiplying
                // out an envelope: scaled by the wind alone it would report zero in
                // dead air, which is the one reading that must not be wrong.
                const float quiver = kSwayIdle + (1.0f - kSwayIdle) * std::sqrt(push);
                const float ripple = sod::kBladeIdle + (1.0f - sod::kBladeIdle) * std::sqrt(push);

                held  = std::max(held, tallest * kSwayHold * push);
                shook = std::min(shook, tallest * kSwaySwing * quiver * 2.0f);

                blade = std::min(blade, static_cast<float>(sod::kBladeTall) * pixel * sod::kBladeSwing * ripple * 2.0f);

                // A leaf off the tallest crown, which is the longest fall and so
                // the furthest the air can take one.
                leaf = std::max(leaf, std::fabs(weather::Carry(sky.WindAt(x), 1.0f, tallest / kLeafFall,
                                                               weather::kLeafDrag)));
            }

            // Long enough a run to carry the quarter round through a lull, or the
            // calm end of every row would be whatever the bearing happened to be.
            world.StepWeather(4.0f);
        }

        std::printf("%-9s %7.1f %6.2f ..%6.2f %9.1f %9.1f %9.1f %9.0f\n", sky.Now().name, sky.Now().wind, lowest,
                    highest, held, shook, blade, leaf);
    }

    // And what the wood actually sheds under each of them, counted rather than
    // reasoned about.
    //
    // Every season against every weather, because the two failures this is built to
    // catch are both invisible in any single cell of the table: a wood that sheds
    // the same number of leaves in a gale as in still air, and one that only sheds
    // at all in autumn. Neither shows up while looking at one afternoon, and both
    // have happened here.
    std::printf("\nleaves adrift over a %.0f px view of wood\n\n", strip.width);

    std::printf("%-10s", "season");
    for (int mood = 0; mood < weather::kMoodCount; mood++) {
        world.ForceWeather(mood);
        world.StepWeather(1.0f / 60.0f);
        std::printf("%10s", world.Sky().Now().name);
    }
    std::printf("\n");

    for (int quarter = 0; quarter < flora::kSeasonCount; quarter++) {
        world.CycleSeason();

        std::printf("%-10s", kSeasonNames[world.Sky().Turn().index % 4]);

        for (int mood = 0; mood < weather::kMoodCount; mood++) {
            world.ForceWeather(mood);

            // Counted over a run rather than on one frame. A gust is a wave and the
            // quarter turns behind it, so a single frame is one point of the range
            // and reports it as the whole.
            int most = 0;

            for (int step = 0; step < 900; step++) {
                world.StepWeather(2.5f);
                world.Update(strip);
                grove.Update(world, strip, {strip.x, strip.y}, world.Sky().Time(), 1.0f / 60.0f, gathered);

                const auto turn = static_cast<flora::Season>(world.Sky().Turn().index);

                grove.DrawLeaves(world.Sky(), turn, strip, world.Sky().Time());

                most = std::max(most, grove.Drifting());
            }

            std::printf("%10d", most);
        }

        std::printf("\n");
    }

    // Back to whatever the clock says, so this leaves nothing held.
    world.ForceWeather(-1);
}

// The surface, column by column, and how much it moves between one and the next.
//
// What this is looking for is roughness at the scale of a lattice column. The
// light is solved one probe per column, so a surface that jumps between
// neighbouring columns is a surface the light cannot follow smoothly however the
// solve is written — the spikes arrive already in the ground.
void ReportSurface(const terrain::Settings &settings, float fromX, float toX) {
    const float step = static_cast<float>(config::kResolution);

    double total = 0.0;
    int steps    = 0;

    float worst  = 0.0f;
    float wheres = 0.0f;

    int spikes = 0;

    float previous = terrain::Height(fromX, settings);

    for (float x = fromX + step; x <= toX; x += step, steps++) {
        const float here = terrain::Height(x, settings);
        const float move = std::fabs(here - previous);

        total += move;

        if (move > worst) {
            worst  = move;
            wheres = x;
        }

        // A step taller than one terrain texel is one the eye reads as an edge
        // rather than as a slope.
        if (move > config::kPixelSize) spikes++;

        previous = here;
    }

    std::printf("%d columns of %.0f px\n", steps, step);
    std::printf("mean step %.2f px   worst %.1f px at x %.0f   over one texel: %.1f%%\n", total / std::max(steps, 1),
                worst, wheres, 100.0 * spikes / std::max(steps, 1));

    // Then a run of them, so the shape of the roughness can be read rather than
    // summarised.
    std::printf("\n%8s %9s %8s\n", "x", "height", "step");

    previous = terrain::Height(fromX, settings);

    for (float x = fromX + step; x <= fromX + step * 40.0f; x += step) {
        const float here = terrain::Height(x, settings);

        std::printf("%8.0f %9.2f %8.2f\n", x, here, here - previous);

        previous = here;
    }
}

// What the cave settings actually carve, measured rather than argued.
//
// Four questions, and each is a way the underground can be wrong without looking
// wrong in any one picture:
//
//  - **How much of the rock is gone.** Every layer adds into the same union and
//    none of them can see the others, so the total is the one figure no single
//    setting is responsible for and the one easiest to move by accident.
//  - **How much of it can be reached from the sky.** The one that matters most
//    and the one that cannot be seen from inside the game: a sealed pocket looks
//    exactly like a cave right up until there is no way out of it, and the rarer
//    the caves are the more each sealed one costs.
//  - **How tall the passages are.** The character is 26 px standing and 14
//    crouched, so a passage is walked, crawled, or is scenery — and which of the
//    three cannot be read off a width setting, because a corridor's own width is
//    measured across the corridor and not against gravity.
//  - **How often the surface opens.** An entrance nobody finds is the same as no
//    cave at all.
//
// Sampled on the lattice the world is stored on, because that is the grid the
// contour is drawn from: anything finer would report passages the marching
// squares cannot represent, and anything coarser would miss the ones it can.
void ReportCaves(const terrain::Settings &settings, Rectangle region) {
    const float step = static_cast<float>(config::kResolution);

    const int cols = static_cast<int>(region.width / step);
    const int rows = static_cast<int>(region.height / step);

    if (cols < 2 || rows < 2) {
        std::printf("region too small: %d x %d cells\n", cols, rows);
        return;
    }

    const auto count = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);

    // Read and discarded, so that what is reported below is this scan's own work
    // rather than the world's calibration, which samples scattered points at
    // startup and misses every one of them by construction.
    terrain::Effort();
    const auto index = [cols](int i, int j) { return static_cast<std::size_t>(j) * cols + i; };

    // The whole region held at once. The flood fill has to see it as one
    // connected thing, and a streaming version would have to keep the frontier
    // anyway, which is most of the memory with none of the simplicity.
    std::vector<unsigned char> solid(count);
    std::vector<float> depth(count);

    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            const Vector2 at = {region.x + static_cast<float>(i) * step, region.y + static_cast<float>(j) * step};

            // Both numbers from one call, since the surface is much the most
            // expensive part of either and SampleGround already pays for it once.
            const terrain::Ground ground = terrain::SampleGround(at, settings);

            depth[index(i, j)] = ground.depth;
            solid[index(i, j)] = (ground.density > terrain::kSurfaceLevel) ? 1 : 0;
        }
    }

    // Volume, in bands measured from the surface rather than from an absolute Y.
    // Depth is what every cave layer is written against, and a band of absolute
    // height would mix the underside of a mountain with the open air beside it.
    constexpr int kBands      = 8;
    constexpr float kBandSpan = 450.0f;

    std::array<long, kBands> bandRock{};
    std::array<long, kBands> bandVoid{};
    std::array<long, kBands> bandOpen{};

    const auto bandOf = [&](std::size_t c) { return std::min(static_cast<int>(depth[c] / kBandSpan), kBands - 1); };

    for (std::size_t c = 0; c < count; c++) {
        if (depth[c] <= 0.0f) continue;

        if (solid[c] != 0) {
            bandRock[bandOf(c)]++;
        } else {
            bandVoid[bandOf(c)]++;
        }
    }

    // Then what the sky can get to, by flooding from the open air above the
    // ground rather than from the top edge of the region. Seeding from the edge
    // would call a cave unreachable purely because the rectangle was cropped
    // above the hill it opens on.
    std::vector<unsigned char> reached(count, 0);
    std::vector<int> frontier;

    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            const std::size_t c = index(i, j);

            if (solid[c] != 0 || depth[c] > 0.0f || reached[c] != 0) continue;

            reached[c] = 1;
            frontier.push_back(static_cast<int>(c));
        }
    }

    const auto spread = [&](std::vector<int> &stack, std::vector<unsigned char> &seen, unsigned char mark) {
        long size = 0;

        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            size++;

            const int i = c % cols;
            const int j = c / cols;

            const auto visit = [&](int ni, int nj) {
                if (ni < 0 || ni >= cols || nj < 0 || nj >= rows) return;

                const std::size_t n = index(ni, nj);
                if (solid[n] != 0 || seen[n] != 0) return;

                seen[n] = mark;
                stack.push_back(static_cast<int>(n));
            };

            visit(i - 1, j);
            visit(i + 1, j);
            visit(i, j - 1);
            visit(i, j + 1);
        }

        return size;
    };

    spread(frontier, reached, 1);

    for (std::size_t c = 0; c < count; c++) {
        if (depth[c] > 0.0f && solid[c] == 0 && reached[c] != 0) bandOpen[bandOf(c)]++;
    }

    // Whatever open rock the sky never arrived at is a pocket. Their sizes are
    // worth more than their number: a hundred single cells left by the roughness
    // term are noise, and one pocket the size of a room is a cave nobody can
    // enter.
    std::vector<unsigned char> pocket(count, 0);
    std::vector<long> pockets;

    long sealed = 0;

    for (std::size_t c = 0; c < count; c++) {
        if (solid[c] != 0 || reached[c] != 0 || pocket[c] != 0 || depth[c] <= 0.0f) continue;

        pocket[c] = 1;

        std::vector<int> stack{static_cast<int>(c)};
        const long size = spread(stack, pocket, 1);

        sealed += size;
        pockets.push_back(size);
    }

    // Size a sealed void stops being a fault in the field and starts being a cave
    // nobody can enter.
    //
    // A cave network in two dimensions cannot be both sparse and wholly connected
    // — corridors only join where they happen to cross, and thinning them thins
    // the crossings faster than the corridors. So the figure worth holding is not
    // how much of the void the sky reaches but how much of it is *worth* reaching
    // and does not: a hundred cells of blind crack behind a wall is rock with a
    // hole in it, and two thousand cells is a hall the player will never see.
    //
    // Two hundred cells is about eight hundred pixels across if it were square,
    // which is a screen's width of cave.
    constexpr long kWorthFinding = 200;

    long lost      = 0;
    long lostCount = 0;
    long largest   = 0;

    for (const long size : pockets) {
        largest = std::max(largest, size);

        if (size < kWorthFinding) continue;

        lost += size;
        lostCount++;
    }

    long undergroundVoid = 0;
    long undergroundRock = 0;

    for (int band = 0; band < kBands; band++) {
        undergroundVoid += bandVoid[band];
        undergroundRock += bandRock[band];
    }

    // Clearance, per unbroken vertical run of open cells. Per run and not per
    // cell, because what decides whether a passage is walked is the height of the
    // passage, and a tall chamber would otherwise vote once for every cell in it.
    //
    // Only runs wholly under the crust are counted. A run that reaches open air
    // has no ceiling, and averaging the sky into the passage heights is how a
    // world of crawlways reports itself as walkable.
    std::vector<float> runs;

    for (int i = 0; i < cols; i++) {
        int open      = 0;
        bool grounded = true;

        for (int j = 0; j < rows; j++) {
            const std::size_t c = index(i, j);
            const bool air      = solid[c] == 0;

            if (air && depth[c] > settings.caves.crust) {
                open++;
                continue;
            }

            // A run ends at rock, and is thrown away if it ended by running out
            // of crust instead — that one is a cave mouth, not a passage.
            if (open > 0 && grounded) runs.push_back(static_cast<float>(open) * step);

            grounded = !air;
            open     = 0;
        }
    }

    std::sort(runs.begin(), runs.end());

    const auto share = [](long part, long whole) { return 100.0 * static_cast<double>(part) / std::max(whole, 1L); };

    std::printf("region %.0f x %.0f px at (%.0f, %.0f)   %d x %d cells of %.0f px\n\n", region.width, region.height,
                region.x, region.y, cols, rows, step);

    {
        const terrain::Work work = terrain::Effort();

        std::printf("cave memo\n");
        std::printf("  %ld lookups, %ld rebuilt (%.1f%% missed), %ld placement tests\n", work.asked, work.built,
                    100.0 * static_cast<double>(work.built) / std::max(work.asked, 1L), work.sited);
        std::printf("  %.1f lookups and %.2f rebuilds per sample\n\n",
                    static_cast<double>(work.asked) / std::max(count, std::size_t{1}),
                    static_cast<double>(work.built) / std::max(count, std::size_t{1}));
    }

    std::printf("volume\n");
    std::printf("%14s %9s %12s %12s\n", "depth", "void", "of it open", "cells");

    for (int band = 0; band < kBands; band++) {
        const long total = bandRock[band] + bandVoid[band];
        if (total == 0) continue;

        const auto from = static_cast<int>(band * kBandSpan);

        if (band == kBands - 1) {
            std::printf("%9d+     %8.1f%% %11.1f%% %12ld\n", from, share(bandVoid[band], total),
                        share(bandOpen[band], bandVoid[band]), total);
        } else {
            std::printf("%9d-%-5d %8.1f%% %11.1f%% %12ld\n", from, static_cast<int>((band + 1) * kBandSpan),
                        share(bandVoid[band], total), share(bandOpen[band], bandVoid[band]), total);
        }
    }

    std::printf("%14s %8.1f%% %11.1f%% %12ld\n\n", "all rock", share(undergroundVoid, undergroundVoid + undergroundRock),
                share(undergroundVoid - sealed, undergroundVoid), undergroundVoid + undergroundRock);

    std::printf("connectivity\n");
    std::printf("  reachable from the sky  %6.1f%% of the void\n", share(undergroundVoid - sealed, undergroundVoid));
    std::printf("  sealed                  %6.1f%% in %zu pockets, largest %ld cells (%.0f px across if square)\n",
                share(sealed, undergroundVoid), pockets.size(), largest,
                std::sqrt(static_cast<double>(largest)) * step);
    std::printf("  caves lost              %6.1f%% of the void in %ld sealed voids over %ld cells\n\n",
                share(lost, undergroundVoid), lostCount, kWorthFinding);

    if (runs.empty()) {
        std::printf("clearance\n  no passages under the crust\n\n");
    } else {
        double sum   = 0.0;
        long upright = 0;
        long crouch  = 0;

        // Counted by area as well as by passage, because the two answer different
        // questions and the first is easy to read as the second. Half the passages
        // being too low to stand in sounds like a world of crawlways, but a
        // passage is a run of any length and the roughness leaves a great many
        // slivers a texel deep in the wall of a hall. What the player is actually
        // in most of the time is the share of the *space* that is stand-up-able.
        double standing = 0.0;
        double stooped  = 0.0;

        for (const float height : runs) {
            sum += height;

            if (height >= player_config::kHeight) {
                upright++;
                standing += height;
            }

            if (height >= player_config::kCrouchHeight) {
                crouch++;
                stooped += height;
            }
        }

        std::printf("clearance          (character %.0f px standing, %.0f crouched)\n", player_config::kHeight,
                    player_config::kCrouchHeight);
        std::printf("  %zu passages   median %.0f px   mean %.0f   tallest %.0f\n", runs.size(),
                    runs[runs.size() / 2], sum / static_cast<double>(runs.size()), runs.back());
        std::printf("  by passage: upright %5.1f%%  crouchable %5.1f%%\n",
                    share(upright, static_cast<long>(runs.size())), share(crouch, static_cast<long>(runs.size())));
        std::printf("  by space:   upright %5.1f%%  crouchable %5.1f%%\n\n", 100.0 * standing / std::max(sum, 1e-9),
                    100.0 * stooped / std::max(sum, 1e-9));
    }

    // The groundwater, on the same footing as the rock: what share of the open
    // space is under water, and how far the table tilts.
    //
    // The tilt is the figure that decides whether any of this works. A water
    // surface only stays where it is put if it is level, so the table has to move
    // by less than a lattice step across the width of a cave — measured here over
    // the widest passage found above, since that is the worst case in this world
    // rather than an assumed one.
    {
        // Width to judge the table's flatness over: a wide chamber, so the figure
        // is the worst a real cave would see rather than an average.
        constexpr float kCaveWidth = 300.0f;

        long wet     = 0;
        long steps   = 0;
        float across = 0.0f;

        for (int i = 0; i < cols; i++) {
            const float x                   = region.x + static_cast<float>(i) * step;
            const terrain::WaterTable table = terrain::TableAt(x, settings);

            if (i > 0 && table.level != terrain::TableAt(x - step, settings).level) steps++;

            // How far the surface moves across the width of a cave, which is the
            // whole of what decides whether it is level enough to stay put.
            across = std::max(across, std::fabs(table.level - terrain::TableAt(x - kCaveWidth, settings).level));

            for (int j = 0; j < rows; j++) {
                const std::size_t c = index(i, j);

                if (solid[c] == 0 && depth[c] > 0.0f && region.y + static_cast<float>(j) * step > table.level) wet++;
            }
        }

        std::printf("water\n");
        std::printf("  %.1f%% of the void is under the table   moves %.0f px across a %.0f px cave, worst\n",
                    share(wet, undergroundVoid), across, kCaveWidth);
        std::printf("  a step of %.0f px every %.0f px   (the lattice is %.0f)\n\n", settings.aquifer.step,
                    (steps > 0) ? region.width / static_cast<double>(steps) : region.width, step);
    }

    // The roughness field's own mid-point, which is the number RoughnessSettings
    // has to be told and cannot work out: the mean of a folded sum of octaves is
    // not the mean of one of them, and the analytic figure is well off what the
    // field does. Authoring it away from this puts every wall in the world that
    // many pixels out in the same direction.
    {
        const terrain::NoiseShape shape = settings.caves.roughness.shape;

        double folded = 0.0;
        long taken    = 0;

        for (int j = 0; j < rows; j += 3) {
            for (int i = 0; i < cols; i += 3) {
                folded += std::fabs(terrain::Signed({region.x + static_cast<float>(i) * step,
                                                     region.y + static_cast<float>(j) * step},
                                                    shape));
                taken++;
            }
        }

        std::printf("wall\n  folded roughness averages %.3f — RoughnessSettings::bias is authored at %.3f\n\n",
                    folded / std::max(taken, 1L), settings.caves.roughness.bias);
    }

    // And the mouths: columns where the sky itself reaches into the rock.
    //
    // Air within a short depth is not enough on its own, and the difference is
    // exactly what the entrance gate is for — a shaft held shut for its first
    // stretch and open below leaves a void under an unbroken lid, which is a cave
    // with no way in and reads on this test as a mouth unless the flood fill is
    // consulted. Asking whether the sky got there answers what is actually being
    // counted.
    const float lid = 40.0f;

    long mouthColumns = 0;
    long mouths       = 0;
    long widest       = 0;
    long run          = 0;

    for (int i = 0; i < cols; i++) {
        bool open = false;

        for (int j = 0; j < rows && !open; j++) {
            const std::size_t c = index(i, j);
            open = solid[c] == 0 && reached[c] != 0 && depth[c] > 0.0f && depth[c] <= lid;
        }

        if (open) {
            mouthColumns++;
            run++;
            widest = std::max(widest, run);
            continue;
        }

        if (run > 0) mouths++;
        run = 0;
    }

    if (run > 0) mouths++;

    std::printf("mouths             (sky reaching within %.0f px of the surface)\n", lid);

    if (mouths == 0) {
        std::printf("  none over %.0f px — the underground has no way in\n\n", region.width);
    } else {
        std::printf("  %ld over %.0f px, one every %.0f px   mean %.0f px wide, widest %.0f\n\n", mouths, region.width,
                    region.width / static_cast<double>(mouths), static_cast<double>(mouthColumns) / mouths * step,
                    static_cast<double>(widest) * step);
    }
}

// What exploring a cave is worth against digging blind, measured.
//
// The claim ElementSpawn::wallBias makes is that a passage pays better than the
// rock beside it, and that is a ratio between two numbers neither of which can
// be read off a setting: the share of the rock near a cave wall that holds a
// material, against the share of the rock well away from one. Both are wanted —
// too small a ratio and the caves are scenery, too large and the rock between
// them is not worth a pickaxe.
//
// Sampled through World::SpawnValue, which reproduces the whole chunk pipeline
// one vertex at a time, contest included. Anything cheaper would be measuring a
// different world from the one generated.
void ReportOre(const World &world, const terrain::Settings &settings, Rectangle region) {
    const float step = static_cast<float>(config::kResolution);

    std::array<long, kElementCount> nearWall{};
    std::array<long, kElementCount> deepRock{};

    long near = 0;
    long deep = 0;

    for (float y = region.y; y < region.y + region.height; y += step) {
        for (float x = region.x; x < region.x + region.width; x += step) {
            const Vector2 at = {x, y};

            const terrain::Ground ground = terrain::SampleGround(at, settings);
            if (ground.depth <= 0.0f || ground.density <= terrain::kSurfaceLevel) continue;

            // How far into the rock this is, in the pixels the bias is written
            // in. The reach is read from the table rather than assumed, since it
            // is the distance the claim is about.
            const bool wall = ground.solid <= kElements[ElementIndex(Element::Coal)].spawn.wallReach;

            if (wall) {
                near++;
            } else {
                deep++;
            }

            for (std::size_t e = 0; e < kElementCount; e++) {
                if (kElements[e].spawn.generator != Generator::Vein) continue;
                if (world.SpawnValue(static_cast<Element>(e), at) <= kElements[e].threshold) continue;

                if (wall) {
                    nearWall[e]++;
                } else {
                    deepRock[e]++;
                }
            }
        }
    }

    std::printf("%ld cells of rock within reach of a wall, %ld well inside it\n\n", near, deep);
    std::printf("%-9s %10s %12s %10s %8s\n", "", "bias", "at a wall", "in rock", "times");

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementSpawn &spawn = kElements[e].spawn;
        if (spawn.generator != Generator::Vein) continue;

        const double atWall = 100.0 * nearWall[e] / std::max(near, 1L);
        const double inRock = 100.0 * deepRock[e] / std::max(deep, 1L);

        std::printf("%-9s %10.3f %11.3f%% %9.3f%% %8.1f\n", kElements[e].name, spawn.wallBias, atWall, inRock,
                    (inRock > 0.0) ? atWall / inRock : 0.0);
    }
}

// How far the liquid moves after being generated, which is the whole of whether
// the world describes water at rest or water about to fall.
//
// A pool laid down as a shape the automaton disagrees with collapses the moment
// it is stepped, and it does it again every time the chunk is rebuilt — so what
// the player sees is water that has moved every time they walk back into a
// place. The fix is not to hold the liquid in memory; it is for the generated
// state to be the settled one. This measures whether it is.
//
// Reported as mass moved against mass present, so it reads as a share and does
// not depend on how much water the region happened to contain.
void ReportSettling(World &world, Rectangle region, int steps) {
    world.Update(region);

    const float before = world.TotalWater(region);

    // A snapshot of every vertex, so that movement can be measured rather than
    // inferred from the total — the automaton conserves mass exactly, so the
    // total is unchanged whether nothing moved or everything did.
    const float span = static_cast<float>(config::kResolution);

    std::vector<float> was;

    for (float y = region.y; y < region.y + region.height; y += span) {
        for (float x = region.x; x < region.x + region.width; x += span) {
            was.push_back(world.ValueAt(Element::Water, {x, y}));
        }
    }

    for (int i = 0; i < steps; i++) world.StepWater(region);

    double moved = 0.0;
    double held  = 0.0;

    std::size_t k = 0;

    for (float y = region.y; y < region.y + region.height; y += span) {
        for (float x = region.x; x < region.x + region.width; x += span, k++) {
            const float now = world.ValueAt(Element::Water, {x, y});

            moved += std::fabs(now - was[k]);
            held += was[k];
        }
    }

    std::printf("region %.0f x %.0f at (%.0f, %.0f), %d steps\n\n", region.width, region.height, region.x, region.y,
                steps);
    std::printf("water present   %.1f units over %zu vertices\n", before, was.size());
    std::printf("water moved     %.1f units, %.2f%% of what was there\n", moved, 100.0 * moved / std::max(held, 1e-9));
    std::printf("total after     %.1f units   (the automaton conserves mass, so this is a check on itself)\n",
                world.TotalWater(region));
}

// Where a frame's time actually goes, at a place in the world.
//
// Every phase the loop runs, in the order it runs them, timed separately. It
// exists because a frame that has gone slow says nothing about which of six
// things did it, and the two costs worth finding — the ones that grow with
// distance from the surface — are invisible from any single-phase measurement.
//
// Run twice over: the first pass pays for whatever the region needed generating,
// which is a real cost but a one-off, and the second is what a frame standing
// still actually costs.
void ReportFrame(World &world, Grove &grove, Inventory &gathered, Vector2 at, int frames) {
    const Rectangle view   = {at.x - 500.0f, at.y - 300.0f, 1000.0f, 600.0f};
    const Rectangle active = Expand(view, kSimulationMargin);

    constexpr float kStep = 1.0f / 60.0f;

    std::printf("at (%.0f, %.0f), surface y %.0f, %d frames\n\n", at.x, at.y,
                terrain::Height(at.x, world.Settings()), frames);
    std::printf("%-14s %10s %10s\n", "", "first ms", "then ms");

    struct Phase {
        const char *name;
        double first;
        double rest;
    };

    std::array<Phase, 5> phases = {{{"world.Update", 0, 0},
                                    {"grove.Update", 0, 0},
                                    {"StepWater", 0, 0},
                                    {"StepWeather", 0, 0},
                                    {"StepLight", 0, 0}}};

    profile::Begin();

    for (int f = 0; f < frames; f++) {
        // The first frame generates the chunks the rest of them read, so it is
        // measured separately above and thrown away here. Everything the report
        // is about is the steady state.
        if (f == 1) profile::Reset();

        profile::Frame();

        double marks[5]{};

        double t0 = GetTime();
        world.Update(active);
        marks[0] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        grove.Update(world, view, at, world.Sky().Time(), kStep, gathered);
        marks[1] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        world.StepWater(active);
        marks[2] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        world.StepWeather(kStep);
        marks[3] = (GetTime() - t0) * 1000.0;

        t0 = GetTime();
        grove.Shade(world, world.Sky().Time());
        world.StepLight(active);
        marks[4] = (GetTime() - t0) * 1000.0;

        for (std::size_t p = 0; p < phases.size(); p++) {
            if (f == 0) {
                phases[p].first = marks[p];
            } else {
                phases[p].rest += marks[p];
            }
        }
    }

    double total = 0.0;

    for (const Phase &phase : phases) {
        const double rest = phase.rest / std::max(frames - 1, 1);

        total += rest;

        std::printf("%-14s %10.2f %10.2f\n", phase.name, phase.first, rest);
    }

    std::printf("%-14s %10s %10.2f   (%.0f fps)\n\n", "steady frame", "", total, (total > 0.0) ? 1000.0 / total : 0.0);
    std::printf("chunks resident %d, pinned %d\n", world.ResidentChunks(), world.PinnedChunks());

    profile::Report("phases");
}

// The world, from the sky down to the brush cursor over it.
//
// Split out from the head-up display because the two are wanted apart: with the
// inventory open the world goes through a blur and the display does not, and a
// blur needs the world rendered into a target of its own. Neither half opens the
// frame, so the caller decides whether that target is the screen.
void DrawScene(const World &world, const Grove &grove, const Inventory &inventory, const Player &player,
               const scuff::Trail &trail,
               const Editor &editor, const LiquidLayer &liquids, const LightLayer &lights, const Camera2D &camera,
               const debug_view::Toggles &debug, bool aiming) {
    const Rectangle view = ViewBounds(camera);

    BeginMode2D(camera);

    // The air first, filling the frame. It replaces clearing it rather than being
    // drawn over a cleared one: there is no height at which the sky is not some
    // colour, so there is nothing for a background to be.
    world.Sky().DrawAtmosphere(view);

    // Then the cloud standing in it, and then the ground over both. Underground the
    // band is out of view and this returns having done nothing.
    world.Sky().DrawClouds(view, world.Spacing());

    {
        PROFILE_ZONE("DrawTerrain");

        world.DrawTerrain(view);
    }

    // The tufts on top of the band the terrain drew, and under everything that
    // stands in them: a trunk belongs in front of the grass around its own foot,
    // which is the reason the ferns are drawn before the trees as well.
    //
    // On the weather clock, so the sway runs with the wind that drives it and
    // both speed up together under F7.
    {
        PROFILE_ZONE("DrawTufts");

        sod::DrawTufts(world.Grass(), view, world.Sky().Time(), world.Settings().seed);
    }

    // The plants over the ground and behind the character, and on this side of
    // the light multiply: a tree is lit by the same daylight as the ground it
    // stands on, and has to know nothing about it to be.
    // Whatever time of year it is. There is no calendar yet, so this is spring
    // unless F9 is holding one — see weather::Sky::Turn.
    const auto season = static_cast<flora::Season>(world.Sky().Turn().index);

    {
        PROFILE_ZONE("grove.Draw");

        grove.Draw(world.Sky(), season, world.Sky().Time());

        grove.DrawFruit(world.Sky(), season, world.Sky().Time());
        grove.DrawLeaves(world.Sky(), season, view, world.Sky().Time());

        // What the wood left on the ground, over the plants and under the character.
        grove.Fallen().Draw();
    }

    // Under the character and over the ground, which is where dust off a foot
    // belongs: it is in front of the hillside it came out of and behind the boot
    // that kicked it.
    trail.Draw(world.Sky().Time());

    player.Draw();

    // Rain in front of the world rather than behind it, so it falls past a cliff
    // face instead of behind one. Still inside the light, because rain in an unlit
    // place should not be the one bright thing on screen.
    //
    // Asked of the world and not of the sky: a drop stops at the first thing under
    // it, and what is under it is the world's to know.
    world.DrawRain(view);

    // And the fog over all of it, which is the last thing drawn inside the light.
    //
    // Over the rain as well as over the ground, because that is where it is: a
    // shower falling into a fog bank is seen through the fog, and drawing the two
    // the other way round puts every drop in front of the air it is falling
    // through. Inside the light for the same reason the rain is — fog in an unlit
    // place must not be the one bright thing on screen.
    {
        PROFILE_ZONE("DrawMist");

        world.DrawMist(view);
    }

    EndMode2D();

    // Composited over the character, so a submerged body is tinted by the
    // liquid it is standing in.
    liquids.Compose(config::kLiquidAlpha);

    BeginMode2D(camera);

    // Then the whole scene is multiplied by the light at once. Everything drawn
    // before this line is lit; everything after it is not, which is exactly the
    // right side of the line for anything meant to be read rather than seen.
    //
    // Skipping the multiply is all it takes to see the world unlit, since the
    // world underneath was already drawn at full brightness.
    {
        PROFILE_ZONE("lights.Compose");

        if (!debug.unlit) lights.Compose();
    }

    // The stars go on this side of that line, and they are the only part of the
    // world that does.
    //
    // A star is a light rather than something lit, and the multiply cannot express
    // one: under it nothing may come out brighter than the sky's own radiance, which
    // at midnight is a tenth — so every star was a grey smudge and its colour went
    // with its brightness. Out here it keeps both. What it costs is that the ground
    // and the cloud no longer hide a star by being drawn over it, so both are asked
    // instead.
    world.DrawStars(view);

    // Drawn over the world rather than under it: the point of an overlay is to
    // check the world against what produced it, which is impossible while the
    // world covers it.
    if (debug.vertices) world.DrawVertexOverlay(view, config::kVertexSize, RED, LIGHTGRAY);
    if (debug.layers) debug_view::DrawLayers(world, view);
    if (debug.chunks) debug_view::DrawChunks(world, view);
    if (debug.light) debug_view::DrawLight(world, view);

    // Not while the panel is up: the pointer is over a slot, not over the world,
    // and a brush ring left under an inventory says the next click will dig
    // where it is sitting when it will not.
    if (aiming) editor.DrawCursor(inventory, grove, season, camera);

    EndMode2D();
}

// Everything drawn in the coordinates of the frame rather than of the world.
void DrawHud(const World &world, const Grove &grove, const Player &player, const Editor &editor,
             const Camera2D &camera, const debug_view::Toggles &debug, float lantern, const char *notice,
             float noticeFor) {
    // Outside the camera transform, unlike every other overlay: what it shows is
    // the image that was baked, not a place in the world.
    if (debug.atlas) grove.DrawSheet();

    // Near white, because the outline under it is what does the separating now.
    const Color ink = {238, 243, 250, 255};

    // And the wet lines keep their meaning by going pale blue rather than dark.
    const Color wet = {152, 206, 255, 255};

    DrawLabel("A/D: move  |  shift: run  |  space: jump  |  S: crouch  |  J: chop  |  mouse: aim  |  F: fly  |"
              "  pg up/dn or ctrl+wheel: zoom",
              10, 10, ink);
    DrawLabel("left: dig  |  right: place what is held  |  1-9 or wheel: slot  |  tab: inventory  |"
              "  - / +: brush size  |  R: regenerate",
              10, 28, ink);
    DrawLabel(TextFormat("V: vertices  |  F3: chunks  |  F4: height grid  |  F5: light probes  |  F6: unlit %s  |"
                         "  F7: fast weather %s  |  F8: next quarter  |  F9: season %s  |  F10: sheet  |"
                         "  F11: stock up  |  F12: weather %s  |  , . : lantern %.1f",
                         debug.unlit ? "on" : "off", debug.fastWeather ? "on" : "off",
                         kSeasonNames[world.Sky().Turn().index],
                         (world.Sky().ForcedMood() < 0) ? "auto" : world.Sky().MoodName(), lantern),
              10, 46, ink);

    DrawLabel(TextFormat("chunks: %d (%d pinned)   edits kept: %d   plants: %d (%d drawn, %d kept)   rays: %ld",
                         world.ResidentChunks(), world.PinnedChunks(), world.RememberedEdits(), grove.VisiblePlants(),
                         grove.DrawnPlants(), grove.RememberedPlants(), world.Light().Rays()),
              10, 70, ink);

    DrawLabel(TextFormat("on the ground: %d", grove.Fallen().Live()), 10, 160, ink);

    const Vector2 centre = player.Centre();
    const auto under     = editor.Under();

    DrawLabel(TextFormat("y: %d   under cursor: %s   light here: %.2f   light at cursor: %.2f",
                         static_cast<int>(centre.y), under.has_value() ? Def(*under).name : "open",
                         world.LightLevelAt(centre), world.LightLevelAt(editor.Aim())),
              10, 88, ink);

    // The weather, and then the cloud standing in it. Read together they show the
    // chain working: the weather sets the level, the sky over this spot fills to it,
    // the shadow follows the cloud that casts it, and the rain leaves from the
    // underside of that same cloud rather than from a height of its own.
    const weather::Weather &sky = world.Sky().Now();

    DrawLabel(
        TextFormat("weather: %-8s  sky %.0f%% full   rain %.0f%%   |   here: %.0f%% cloud  %.0f%% shade  base y %d",
                   sky.name, sky.cover * 100.0f, sky.rain * 100.0f, world.Sky().CoverAt(centre.x) * 100.0f,
                   world.Sky().ShadeAt(centre.x) * 100.0f, static_cast<int>(world.Sky().UndersideAt(centre.x))),
        10, 106, sky.rain > 0.0f ? wet : ink);

    // And the wind, on its own line because it is the one reading with three parts
    // that can disagree. The mood's figure is what the table says, the gust here is
    // what this column is actually getting, and the share is what everything rooted
    // to the ground reads — so a wood standing still under a gale can be traced to
    // whichever of the three ate it, instead of being guessed at.
    DrawLabel(TextFormat("wind: %-4.0f px/s mean   %+5.0f here   |   push %+.2f   gale %.0f   |   %d adrift", sky.wind,
                         world.Sky().WindAt(centre.x), world.Sky().PushAt(centre.x), world.Sky().Gale(),
                         grove.Drifting()),
              10, 124, ink);

    // Then the day, and how damp it has left the ground. The clock is the world's
    // own and not the wall's: a whole turn is `Day::dayMinutes` of weather time, so
    // it runs fast under F7 with everything else.
    const weather::Daylight &today = world.Sky().Today();

    const float hours  = today.phase * 24.0f;
    const float damp   = world.HumidityAt(centre);
    const int filled   = static_cast<int>(damp * 10.0f + 0.5f);
    const char *soaked = "..........";
    const char *dry    = "          ";

    DrawLabel(TextFormat("%02d:%02d  %-5s   daylight %.0f%%   |   humidity %.0f%%  [%.*s%.*s]", static_cast<int>(hours),
                         static_cast<int>((hours - std::floor(hours)) * 60.0f), today.name, today.light * 100.0f,
                         damp * 100.0f, filled, soaked, 10 - filled, dry),
              10, 142, damp > 0.66f ? wet : ink);

    // Flight suspends gravity and collision both, which is not something to be
    // left on by accident, so it says so where the eye already is.
    if (player.IsFlying()) {
        const char *text = "flying  (F)     W/S: up and down     shift: boost";
        const int width  = MeasureText(text, 14);

        const Rectangle badge = {GetScreenWidth() - width - 26.0f, 8.0f, width + 16.0f, 22.0f};

        DrawRectangleRec(badge, {30, 34, 42, 220});
        DrawRectangleLinesEx(badge, 2.0f, {170, 130, 220, 255});
        DrawText(text, static_cast<int>(badge.x + 8.0f), static_cast<int>(badge.y + 4.0f), 14, {190, 160, 235, 255});
    }

    // Whatever the world last refused to do, over the middle of the screen where
    // the eye already is after pressing a key that did nothing.
    if (noticeFor > 0.0f) {
        const int width = MeasureText(notice, 14);

        DrawLabel(notice, (GetScreenWidth() - width) / 2, GetScreenHeight() - 140, {255, 214, 140, 255});
    }

    DrawBrushSize(editor);
}

} // namespace

int main(int argc, char **argv) {
    // `--probe x y w h out.png` draws one strip of the world to a file and exits.
    // See DrawProbe. Read here rather than after the window opens, so the probe
    // can ask for a hidden one: it wants a GL context and nothing else.
    const bool probing = argc >= 7 && TextIsEqual(argv[1], "--probe");

    // `--covers x0 x1 step` walks the columns and reports what each cover claims.
    // See ReportCovers.
    const bool counting = argc >= 5 && TextIsEqual(argv[1], "--covers");

    // `--woods x0 x1` walks the scatter's cells and reports what grows where. See
    // ReportWoods.
    const bool cruising = argc >= 4 && TextIsEqual(argv[1], "--woods");

    // `--tones` reports how each material's paint divides between form and
    // texture. See ReportTones.
    const bool weighing = argc >= 2 && TextIsEqual(argv[1], "--tones");

    // `--column x y0 y1` prints every material's field down one column. See
    // ReportColumn.
    const bool reading = argc >= 5 && TextIsEqual(argv[1], "--column");

    // `--caves x y w h` measures what the cave settings carve over a region. See
    // ReportCaves.
    const bool digging = argc >= 6 && TextIsEqual(argv[1], "--caves");

    // `--ore x y w h` measures what a cave wall is worth against blind rock. See
    // ReportOre.
    const bool assaying = argc >= 6 && TextIsEqual(argv[1], "--ore");

    // `--settle x y w h [steps]` measures how far generated liquid moves once it
    // is simulated. See ReportSettling.
    const bool settling = argc >= 6 && TextIsEqual(argv[1], "--settle");

    // `--frame x y [frames]` times every phase of the loop at a place. See
    // ReportFrame.
    const bool timing = argc >= 4 && TextIsEqual(argv[1], "--frame");

    // Reports a table and draws nothing, so it wants no window on screen either.
    const bool gauging = argc >= 2 && TextIsEqual(argv[1], "--wind");

    // `--sodcheck [frames]` walks a view across the world and checks the band it
    // remembers against one built from cold.
    const bool checking = argc >= 2 && TextIsEqual(argv[1], "--sodcheck");

    // `--profile [frames]` plays the game with nobody at the keys and reports
    // where the frame went. This one wants a window on screen: the draw is half
    // of what it is measuring.
    const bool profiling = argc >= 2 && TextIsEqual(argv[1], "--profile");

    // Resizable, with a floor under it: below the minimum the hotbar is wider than
    // the frame and the head-up display runs off the side of it.
    SetConfigFlags((probing || counting || cruising || weighing || reading || digging || assaying || settling || timing
                    || gauging || checking)
                       ? FLAG_WINDOW_HIDDEN
                       : FLAG_WINDOW_RESIZABLE);

    // Off while profiling, so a frame that finishes early is not slept away and
    // the report says what the frame actually costs rather than what it was
    // capped at.
    const int targetFps = profiling ? 0 : config::kTargetFps;

    InitWindow(config::kScreenWidth, config::kScreenHeight, "marching squares");
    SetWindowMinSize(config::kMinScreenWidth, config::kMinScreenHeight);
    SetTargetFPS(targetFps);

    // Escape stops closing the window, so that it can close the inventory
    // instead.
    //
    // raylib binds it to WindowShouldClose by default, which with a panel on
    // screen means the key every player presses to back out of a panel quits the
    // game instead — losing whatever they were in the middle of. The window still
    // closes by its own button, and Minecraft's escape has never quit anything
    // either.
    SetExitKey(KEY_NULL);

    // Assets are opened through paths relative to the executable.
    ChangeDirectory(GetApplicationDirectory());

    // The world, written one layer at a time. Every number below is in world
    // pixels or in features per terrain::kFeatureSpan pixels, so the settings can
    // be read against the size of the character: it is 26 pixels tall, 12 wide,
    // and jumps 72.
    //
    // Not const, and only for one reason: `Calibrate` writes the measured cutoffs
    // back into it further down. Nothing else here may be touched after this
    // point — the whole generator is a pure function of these numbers, and a
    // setting that changed while the world was running would mean two halves of the
    // same map generated from two different worlds.
    terrain::Settings settings =
        {
            .surface =
                {
                    .level = 144.0f,

                    // Where the land is broadly high and where it is broadly low.
                    // One feature spans a couple of screens, so this is what the
                    // player reads as having travelled somewhere.
                    .relief          = {.frequency = 0.45f, .octaves = 2, .seed = 4401},
                    .reliefAmplitude = 150.0f,

                    // The hills actually walked over. Amplitude against frequency is
                    // what sets the slope: this pair averages about a quarter, so a
                    // hillside is climbed at roughly fourteen degrees and its
                    // steepest stretches at forty.
                    .hills         = {.frequency = 2.6f, .octaves = 3, .seed = 4402},
                    .hillAmplitude = 70.0f,

                    // Where the ranges stand. One feature every twenty thousand
                    // pixels or so, which is ten screens: a range has to be
                    // something walked towards for a long while and then walked
                    // into, or it is scenery that happens to be tall.
                    .range         = {.frequency = 0.05f, .octaves = 2, .seed = 4406},
                    .rangeCoverage = 0.14f,
                    .rangeEdge     = 0.09f,

                    // And the crests inside one. Sixteen times the range's own
                    // frequency, so a range is several summits with valleys between
                    // them rather than one enormous cone — a feature every twelve
                    // hundred pixels, which is three or four peaks across a range.
                    //
                    // The frequency is half of what decides whether a mountain can
                    // be climbed, the amplitude being the other half: the crest
                    // rises over half a feature, so at this pair a flank averages a
                    // slope of 0.7 and the terrace turns that into a twenty-four
                    // pixel riser every thirty-four of run. Against a jump of
                    // seventy-two that is a staircase. At twice this it was a slope
                    // of 1.4 and a riser every seventeen, which is a wall with
                    // notches in it.
                    .ridge = {.frequency = 0.8f, .octaves = 3, .seed = 4407},

                    // Four hundred pixels at the crest, on top of the hundred or two
                    // the relief and the hills already give. A peak therefore stands
                    // some five hundred above the plains, which against a character
                    // of twenty-six is nineteen of it and very nearly the whole of a
                    // full-screen view from the valley floor.
                    //
                    // Bounded from above by the sky rather than by taste. The cloud
                    // deck hangs between y = -640 and y = -320, and a peak that
                    // climbed past the underside of it would be standing *inside*
                    // the cloud — which the rain already answers correctly, by
                    // having nowhere left to fall from, but which reads as a summit
                    // that mysteriously never gets any weather. At this figure the
                    // tops sit just under the deck and only the rare one pokes into
                    // it during a storm, when the base drops a hundred pixels.
                    //
                    // The terrace is what makes the climb possible at all — the
                    // whole slope is snapped into ledges a quarter of a jump apart,
                    // so a mountainside is a staircase rather than a wall.
                    .ridgeAmplitude = 470.0f,

                    // Under one, which broadens the crest instead of sharpening it.
                    //
                    // This was 2.2 and it drew a range of shark's teeth: every
                    // summit came to a point one lattice column wide, so there was
                    // nowhere up there to stand and the climb ended on a spike. A
                    // mountain has to be somewhere to *go*, and that means shelves
                    // on the way up and a top to arrive at — both of which live in
                    // the mid-range of the fold, which is precisely what an exponent
                    // under one keeps and one over it throws away.
                    //
                    // The three octaves of the ridge field then do the rest: with
                    // the crest flattened, their bumps land as ledges and shoulders
                    // across the summit rather than as ripples down a point.
                    .ridgeSharp = 0.72f,

                    // And the shelves cut across the face. Forty-eight pixels is
                    // two of the world's own ledges and two thirds of the jump, so
                    // a riser is climbed rather than scaled, and it is coarse enough
                    // that a shelf is a run of flat ground wide enough to build on
                    // and to meet something on.
                    //
                    // Snapped most of the way rather than all of it. At one the face
                    // is a flight of identical stairs, which is legible and dead; at
                    // this the shelves are plainly there and no two of them are the
                    // same width, because what is under them is still a mountain.
                    .shelfStep = 48.0f,
                    .shelf     = 0.66f,

                    // Texture underfoot, kept small. This is the term that turns a
                    // walkable slope into a staircase of one-pixel steps.
                    .detail          = {.frequency = 9.0f, .octaves = 2, .seed = 4403},
                    .detailAmplitude = 12.0f,

                    // Broad enough that a plain is a place rather than a gap
                    // between hills, and a floor low enough that a fully eroded
                    // stretch is genuinely flat ground.
                    .erosion      = {.frequency = 0.55f, .octaves = 1, .seed = 4404},
                    .erosionFloor = 0.22f,

                    // Half the slope is snapped into ledges a quarter of the
                    // character's jump apart, so a hillside is somewhere to stand
                    // rather than a ramp.
                    .terrace     = 0.45f,
                    .terraceStep = 24.0f,

                    // How steep the climb between two ledges is. Measured against
                    // the roughness of the ground itself — see the declaration.
                    .terraceSharp = 2.0f,

                    // Overhangs, off by default. It is the one layer that can put a
                    // hole back in open ground, so it is turned up by eye and left
                    // alone until then.
                    .warp          = {.frequency = 3.0f, .octaves = 2, .seed = 4405},
                    .warpAmplitude = 0.0f,
                    .warpDepth     = 96.0f,
                },
            .caves =
                {
                    // A hundred and ten pixels of solid ground over everything
                    // below, which is what keeps the surface a surface. Only an
                    // entrance may cross it.
                    .crust     = 110.0f,
                    .crustFade = 72.0f,

                    // The systems themselves. Every number here is a fact about a
                    // walk rather than about a field, which is the point of the
                    // change: a passage is twenty-four steps of six pixels because
                    // that is how far the digger went.
                    .systems =
                        {
                            // A cell is wide and short because a system is. Nine
                            // of them are searched per query, so the shape of the
                            // cell is what keeps that search small.
                            .cellSpan = 800.0f,
                            .cellRise = 400.0f,

                            // Half the eligible cells hold one. With the region
                            // gate over the top, that comes out at a system every
                            // couple of screens near the surface and most of the
                            // ground occupied far below.
                            .chance = 0.9f,

                            // Two hundred and forty steps of six pixels: fourteen
                            // hundred pixels of walking, which the wander folds
                            // into something under a thousand across. Six is the
                            // lattice step, so the walk cannot cut a corner finer
                            // than the world can draw.
                            .steps      = 240,
                            .stepLength = 6.0f,

                            .wander  = 0.34f,
                            .damping = 0.72f,

                            // Two fifths of a step into the vertical, so a passage
                            // is walked rather than fallen down.
                            .squash = 0.42f,

                            // Thirty-two pixels of headroom near the crust, opening
                            // to forty-eight far below — comfortably over the
                            // character's twenty-six either way, since a passage
                            // that has to be crouched through is a crawlway and
                            // this layer is the route.
                            .radius        = 16.0f,
                            .radiusAtDepth = 24.0f,
                            .growthDepth   = 1800.0f,

                            .taper = 0.16f,

                            // Three branches off each trunk, at a shade over sixty
                            // degrees: enough that a system has somewhere to go
                            // wrong in, few enough that it still reads as one
                            // route with sides rather than as a net.
                            .branches     = 3,
                            .branchLength = 0.42f,
                            .branchAngle  = 1.05f,
                            .branchRadius = 0.78f,

                            // A room every twenty steps or so, three and a bit
                            // times the width of the passage that leads into it —
                            // a hundred pixels of headroom, which is four of the
                            // character.
                            .roomChance = 0.05f,
                            .roomSteps  = 7,
                            .roomSwell  = 3.2f,
                            .roomFloor  = 0.3f,

                            // Under a third of systems reach daylight, and the one
                            // that does opens forty-four pixels wide.
                            .entranceChance = 0.9f,
                            .entranceRadius = 22.0f,
                            .entranceWander = 0.16f,
                            .entranceSteps  = 90,
                        },

                    // A tenth of the ground just under the crust is cave country,
                    // rising to half of it far below. Rarity is only ever felt at
                    // the surface; depth is where the volume belongs.
                    .region                = {.frequency = 0.6f, .octaves = 2, .seed = 4410},
                    .regionCoverage        = 0.55f,
                    .regionCoverageShallow = 0.35f,
                    .regionDeepens         = 1500.0f,

                    // The wall: a fret over the whole of it, and rounded bites
                    // taken out of it. A swept circle is smooth, and this is what
                    // stops a corridor being an outline made of arcs.
                    .roughness = {.shape         = {.frequency = 24.0f, .octaves = 2, .seed = 4418},
                                  .amplitude     = 5.0f,
                                  .bias          = 0.329f,
                                  .lobes         = {.frequency = 20.0f, .octaves = 1, .seed = 4427},
                                  .lobeAmplitude = 9.0f,
                                  .lobeBite      = 0.62f,
                                  .reach         = 22.0f},
                },

            // The groundwater. A sixth of the open rock is under it, and all of
            // that is deep: the halls the player first walks into are dry, and
            // meeting water is arriving somewhere rather than the state of the
            // underground.
            .aquifer =
                {
                    // One feature spans some hundred and seventy thousand pixels,
                    // which is what a regional water table is — it stands higher in
                    // one part of a country than in another and is level everywhere
                    // in between. The slowness is not a style: it is what makes the
                    // snapping below rare, and the snapping is what makes the
                    // surface flat.
                    //
                    // Three thousand pixels down at the middle and twelve hundred of
                    // swing either way, so it runs between about eighteen hundred
                    // below the ground and four thousand. Over that range it crosses
                    // the caves at every height, which is the case worth having: a
                    // chamber with its floor under water and its roof in the air.
                    //
                    // Twelve pixels to the step — two of the lattice. Measured, one
                    // turns up every seven hundred pixels or so, and what a cave
                    // unlucky enough to contain one gets is a two-cell ledge in its
                    // surface, which settles in a few frames and is not visible
                    // doing it. Snapping to a coarser figure would put them four
                    // times further apart and make each one a waterfall.
                    .level = {.frequency = 0.006f, .octaves = 1, .seed = 4430},
                    .depth = 3600.0f,
                    .swing = 1100.0f,
                    .step  = 12.0f,
                },

            // The two axes every biome in the world is chosen on: which cover lies
            // on the rock, which trees grow in it, what the grass under them is,
            // and how much it rains.
            //
            // One feature spans a dozen screens. It was four or five, and four or
            // five is not a region — it is a patch: a desert could be crossed in
            // under a minute of running, and the belt of steppe that is supposed to
            // approach it was a few hundred pixels wide. The fields are folded
            // Perlin and the frequency is only a scale, so nothing about *what*
            // values occur changes here — the same deserts and snowfields are still
            // reached, they are simply travelled into rather than stumbled over.
            //
            // Humidity stays the faster of the two so that a temperature band still
            // holds more than one kind of country: at equal frequencies the pair
            // move together and the world comes out as one axis with two names.
            .climate =
                {
                    .temperature = {.frequency = 0.08f, .octaves = 2, .seed = 4420},
                    .humidity    = {.frequency = 0.11f, .octaves = 2, .seed = 4421},

                    // Over the hundred and twenty pixels of elevation this relief
                    // reaches, high ground gains a fifth of the humidity range and
                    // loses a seventh of the temperature range. Enough that the peaks
                    // are visibly cloudier than the plains beside them, not enough
                    // that either runs to an extreme on altitude alone.
                    .humidityLift     = 0.0016f,
                    .temperatureLapse = 0.0011f,
                },
            .seed = 1337,
        };

    // The sky. Cloud, and the two things cloud is: shade on the ground below it and
    // rain out of the bottom of it.
    const weather::Settings sky = {
        // The air. Not a pair of colours to interpolate between: the two facts that
        // produce the gradient, and the gradient follows. Air thins with height and
        // blue scatters five times harder than red, which between them give the pale
        // band at the horizon, the deep blue overhead and the fade towards black
        // above that.
        .air =
            {
                // About half a screen, so the sky visibly deepens within one view
                // rather than only after climbing for a while.
                .scaleHeight = 320.0f,
                .thickness   = 3.5f,
                .rayleigh    = {0.52f, 1.15f, 2.60f},

                .overcast     = {150, 156, 168, 255},
                .overcastWash = 0.80f,
                .bandHeight   = 10.0f,
            },

        .stars =
            {
                // Seventy pixels apart puts a hundred-odd in a screen of sky, which
                // is enough to read as a sky and few enough that each one is a mark
                // rather than grain.
                .spacing = 70.0f,

                // Under the world's own pixel, which is five. A star at the size of
                // a terrain tile reads as a tile rather than as a light, and it is
                // the one thing here that is allowed off that lattice — nothing else
                // in the world is a point at an unreachable distance.
                .size = 3.0f,

                // A seventh of the world's motion. Enough that walking a screen
                // moves them a little and they sit behind the landscape rather than
                // on it; little enough that they are plainly a long way off.
                .parallax = 0.14f,

                // Swept clean for the first three hundred pixels above the ground.
                // A star sitting just over the treeline reads as a hole in the
                // picture rather than as a sky, and it is the first thing the eye
                // goes to because it is the part of the sky nearest the land.
                .rise = 320.0f,

                // Untouched through a clear or a fair sky — the clouds that are
                // there already hide what is behind them — and gone entirely by the
                // time the deck is closed, which is what a storm looks like from
                // underneath.
                .hideFrom = 0.55f,
                .hideAt   = 0.95f,

                // Faded across a cloud's outline rather than cut at it. The cloud is
                // rasterised from a lattice a dozen pixels across, so its drawn edge
                // and the field's exact one disagree over about a fiftieth of it —
                // and every one of those is a star left burning on the rim, which is
                // exactly where it gets noticed.
                .cloudEdge = 0.09f,

                // A fifth between the brightest and the faintest, and colour doing
                // the rest of the work. The full range reads as noise: the eye finds
                // the scatter before it finds the sky.
                .spread = 0.22f,
                .tint   = 0.70f,

                // Present but not restless. A seventh of the brightness, wavering a
                // little under twice a second — enough to be alive, little enough
                // that the field does not shimmer.
                .twinkle     = 0.15f,
                .twinkleRate = 1.7f,

                // Two ends of a colour temperature rather than one white, and both
                // well clear of grey: a star is one square against a nearly black
                // ground, and a tint that is merely suggested does not survive being
                // blended down.
                .hot  = {170, 202, 255, 255},
                .cool = {255, 186, 128, 255},
            },

        // How a cloud takes the light: Beer-Lambert with the powder term, per cell,
        // from that cell's own depth towards the sun. Nothing here takes the camera
        // as an input, which is the point.
        .shading =
            {
                .layers = 6,

                // Where the sun is comes from the day now, not from here. See
                // `Day::sunTilt` below: it is held off the vertical so the cloud
                // always keeps a lit flank, which is the whole difference between a
                // shaded shape and a lit one.
                .sunReach = 150.0f,

                // High, because the depth it is given is a field margin and those run
                // to about a third rather than to one. At two the darkest Beer term
                // was a half and every cloud sat in the top of the bands, uniformly
                // bright; this spreads them over the whole range.
                .absorption  = 7.0f,
                .powder      = 1.0f,
                .powderScale = 9.0f,

                // Short of the full range at both ends, so the brightest band stays
                // below pure sunlight and the darkest above pure ambient. A cloud
                // that reaches either is a cloud with a blown edge or a black hole in
                // it.
                .darkest  = 0.08f,
                .lightest = 0.92f,
            },

        .day =
            {
                // Long enough that a day is something lived through rather than
                // watched, and deliberately not a whole multiple of `spellMinutes`:
                // at four spells to a day every dawn would fall at the same point of
                // a spell for ever and the weather would never once break
                // differently over a sunrise. At 24 against 5 the two only realign
                // every fifth day.
                //
                // F7 runs this forty times faster along with the weather, which puts
                // a whole day in half a minute. F8 runs it on to the next quarter.
                .dayMinutes = 24.0f,

                // Eight in the morning, so a fresh world opens in full light with
                // the day ahead of it. Stated here rather than left to the default
                // because this is the block a reader comes to when asking what
                // time the game starts.
                .startAt = 8.0f / 24.0f,

                // Both below the horizon's own zero, so light arrives before the sun
                // does and outlasts it. Twelve minutes of full day, eight and a half
                // of full night, and about two minutes of turning at each end.
                //
                // Note what the top edge buys: the daylight is pinned at exactly one
                // for about half the cycle, so high noon is the sky this world had
                // before there was a clock in it, unchanged.
                .darkAt = -0.45f,
                .litAt  = 0.05f,

                // Half off vertical at noon. A sun straight overhead lights only the
                // tops of the clouds and takes the side off them.
                .sunTilt = 0.50f,

                // How much more air the light crosses along the horizon than
                // overhead. This one number is the sunset. Above about 0.8 the
                // middle of the sky passes through an olive on its way down, which
                // is real Rayleigh and reads as a bruise.
                .travel = 0.65f,

                // What a cloud can still hold back at midnight. Without it a storm
                // after dark is not dim, it is black.
                .nightShade = 0.15f,

                // How long the ground remembers a shower, and how fast it forgets.
                // A quarter of an hour back, halving every four minutes — so a storm
                // is still felt underfoot two spells of weather after it passed.
                .wetMinutes  = 15.0f,
                .wetHalfLife = 4.0f,

                // Both against a climate that already runs the whole range, so
                // neither may be large: a soaking should make a dry place damp, not
                // make every place the same.
                .wetGain = 0.55f,
                .dryGain = 0.30f,
            },

        // The weather this world has, and how long a spell of it lasts.
        //
        // Rain lives here and nowhere else. Minecraft, Terraria and Stardew all put
        // it here too: one sky for the whole world, on a timer. It is the only
        // arrangement in which it does not rain on one cloud and not the one beside
        // it, and it is also what makes a rainy sky overcast — the storm's `cover` is
        // that rule, rather than a second one written somewhere else.
        .moods =
            {
                // The winds are in pixels per second at ground level, and the spread
                // between the calmest row and the windiest is the thing to judge
                // rather than any one figure: it is the whole range every rooted
                // thing in the world swings through. The rows must stay in the order
                // of weather::Mood — this array is read by index.
                //
                // Not zero at the calm end, and it cannot be. The sway is drawn in
                // whole plant texels, so a crown moving less than one of them does
                // not move at all, and a wood standing perfectly rigid on a clear
                // afternoon reads as broken rather than as calm.
                {.name       = "clear",
                 .cover      = 0.14f,
                 .rain       = 0.0f,
                 .wind       = 7.0f,
                 .likelihood = 1.0f,
                 .sunlight   = {255, 252, 246, 255},
                 .ambient    = {150, 176, 214, 255},
                 .shade      = 0.85f},

                {.name       = "fair",
                 .cover      = 0.34f,
                 .rain       = 0.0f,
                 .wind       = 15.0f,
                 .likelihood = 1.6f,
                 .sunlight   = {255, 250, 240, 255},
                 .ambient    = {138, 162, 202, 255},
                 .shade      = 1.0f},

                // A bright, broken sky with the air tearing through it. Less cloud
                // than fair weather and more than twice the wind, which is the one
                // combination nothing derived from the cover could ever produce —
                // and the reason the wind is a column of this table at all.
                {.name       = "blustery",
                 .cover      = 0.30f,
                 .rain       = 0.0f,
                 .wind       = 40.0f,
                 .likelihood = 0.5f,
                 .sunlight   = {255, 250, 242, 255},
                 .ambient    = {140, 166, 206, 255},
                 .shade      = 0.95f},

                {.name       = "overcast",
                 .cover      = 0.78f,
                 .rain       = 0.0f,
                 .wind       = 24.0f,
                 .likelihood = 0.8f,
                 .sunlight   = {228, 232, 240, 255},
                 .ambient    = {116, 128, 152, 255},
                 .shade      = 1.0f},

                // Most of the sky, the only mood that rains, and the hardest it
                // blows. This row alone sets the envelope every share is taken
                // against — see Sky::Gale.
                {.name       = "storm",
                 .cover      = 0.94f,
                 .rain       = 1.0f,
                 .wind       = 54.0f,
                 .likelihood = 0.55f,
                 .sunlight   = {176, 184, 200, 255},
                 .ambient    = {80, 88, 108, 255},
                 .shade      = 1.0f},
            },

        // A spell is minutes, not seconds: weather is something to shelter from, not
        // something that flickers. F7 runs the clock forty times faster for looking
        // at it.
        .spellMinutes = 5.0f,
        .crossMinutes = 1.2f,
        .seed         = 7717,

        // Well above the highest ground, which the relief puts at about y 20, so
        // cloud is always sky and never something that can be walked into.
        .ceiling  = -640.0f,
        .base     = -320.0f,
        .rainDrop = 100.0f,

        // The base shape. Frequency against aspect sets the size: this pair gives a
        // cloud about four hundred pixels across and a hundred and eighty deep, so a
        // couple are in view at once and each is comfortably shorter than the band it
        // floats in.
        .shape = {.frequency = 4.6f, .octaves = 3, .gain = 0.5f, .aspect = 2.3f, .seed = 5501},
        .cloudWind = 18.0f,

        // One feature of that shape is a little over two hundred pixels, so at this
        // a cloud has re-formed into a different cloud in about half a minute —
        // roughly the time the wind takes to carry it its own width. Slow enough
        // that watching one is watching it change rather than watching it flicker.
        .evolve = 6.0f,

        // The front, demoted to rippling the cover from place to place.
        .front     = {.frequency = 0.18f, .octaves = 2, .seed = 5502},
        .frontWind = 22.0f,

        // Both small. They texture the sky; they do not overrule the weather, or a
        // storm would have clear patches in it.
        .frontInfluence    = 0.14f,
        .humidityInfluence = 0.10f,

        // The lobes, at nearly three times the base frequency so they read as bumps on
        // the cloud rather than as the cloud itself.
        .lobes     = {.frequency = 13.0f, .octaves = 2, .aspect = 1.5f, .seed = 5504},
        .worleyMix = 0.38f,

        // A lobe cell is some seventy pixels tall, so a bump climbs through its own
        // cell in about fifteen seconds: turrets rolling up through the body of the
        // cloud. The sideways term is a sixth of the wind, which is a crawl and not a
        // slide.
        .lobeCrawl = {-3.0f, -5.0f},

        // Three times the base frequency, biting a fifth of the way in. This is the
        // rim of small lobes; without it the outline is a smooth swell.
        .detail      = {.frequency = 14.0f, .octaves = 2, .aspect = 1.6f, .seed = 5503},
        .erosion     = 0.20f,
        .erosionBand = 0.16f,

        // Twice the lobes and the other way along the wind, so the rim boils while
        // the body only rolls. This is the fastest thing in the sky and it is the
        // smallest, which is the right way round.
        .detailCrawl = {4.0f, -11.0f},

        .fieldStep = 2.0f,
        .softness  = 0.13f,
        .bandTaper = 0.85f,

        // Chosen against the exposure curve rather than by the look of the number:
        // this is a quarter darker than open sky on screen. Half of it would be 4%
        // darker and read as nothing at all.
        .shade = 0.78f,

        // Rain. The drop is mixed towards this and always ends up lighter than the
        // air behind it, so it reads against noon and against a storm alike — and
        // against a night, once there is one, without anything here being touched.
        .rainLine   = {216, 234, 255, 255},
        .rainSpeed  = 620.0f,
        .rainDrift  = 8.0f,
        .rainLength = 24.0f,
        .rainSpread = 0.55f,

        .rainDensity = 240.0f,
        .rainSpan    = 1400.0f,
    };

    // Measured before anything reads the settings, and not only before the world is
    // built.
    //
    // World's constructor calibrates its own copy, which is right and is not
    // enough: every report below is handed this one, and an uncalibrated copy is a
    // different world. The mountains are what made that visible — the range cutoff
    // defaults to a value no sample clears, so `--covers` reported a world with no
    // ranges and no snow in it while the game had both. Calibration is a
    // measurement and idempotent, so running it twice costs a moment at startup and
    // nothing else.
    terrain::Calibrate(settings);

    World world(settings, config::kResolution);
    world.SetWeather(sky);

    // The woods. Left at the table's own defaults for now: what a stand is, how
    // thick it is and where its border falls are the numbers this is tuned by,
    // and they are settled by walking through a wood at each setting rather than
    // by argument, the same way the lantern and the cave coverage were.
    Grove grove;
    grove.Configure({.seed = settings.seed}, settings, world.Sky());


    if (argc >= 4 && TextIsEqual(argv[1], "--sun")) {
        const float x = static_cast<float>(std::atof(argv[2]));
        const float y = static_cast<float>(std::atof(argv[3]));

        const Rectangle region = {x, y, 900.0f, 400.0f};

        world.StepWeather(1.0f / 60.0f);
        for (int step = 0; step < 400 * 60; step++) world.StepWeather(1.0f / 60.0f);

        world.Update(region);
        world.StepLight(region);

        ReportSun(world, region);

        CloseWindow();
        return 0;
    }

    if (gauging) {
        // Over the birch wood rather than the origin: the leaf field needs
        // deciduous trees to come off, and the trees at the origin are pines.
        Inventory gauged{};

        ReportWind(world, grove, gauged, {1800.0f, -260.0f, 900.0f, 400.0f});

        CloseWindow();
        return 0;
    }

    if (argc >= 4 && TextIsEqual(argv[1], "--surface")) {
        terrain::Settings tuned = settings;

        // The knob under test, overridable from the command line so a sweep is a
        // loop in a shell rather than a rebuild each time.
        if (argc >= 5) tuned.surface.terraceSharp = static_cast<float>(std::atof(argv[4]));
        if (argc >= 6) tuned.surface.terrace = static_cast<float>(std::atof(argv[5]));

        ReportSurface(tuned, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])));

        CloseWindow();
        return 0;
    }

    if (weighing) {
        ReportTones();

        CloseWindow();
        return 0;
    }

    if (counting) {
        ReportCovers(settings, static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                     static_cast<float>(std::atof(argv[4])));

        CloseWindow();
        return 0;
    }

    if (cruising) {
        // The grove's own settings, calibrated: the coverage figures are measured
        // cutoffs and a scatter run against uncalibrated ones is a scatter of a
        // different world.
        ReportWoods(grove.Settings(), settings, static_cast<float>(std::atof(argv[2])),
                    static_cast<float>(std::atof(argv[3])));

        CloseWindow();
        return 0;
    }

    if (argc >= 2 && TextIsEqual(argv[1], "--sodcheck")) {
        // Walks two identical worlds along the same path, one remembering its
        // grass band between frames and one made to work the whole band out
        // again, and reports any column they disagree about.
        //
        // Both worlds see exactly the same chunks come and go, which is what
        // makes the comparison about the memory and nothing else: a column whose
        // chunk is resident is answered by interpolating the field and one whose
        // chunk is not is answered from the noise, so two worlds with different
        // chunks resident differ for reasons that have nothing to do with this.
        const int frames = (argc >= 3) ? std::atoi(argv[2]) : 400;

        World plain(settings, config::kResolution);
        plain.SetWeather(sky);

        int checked = 0;
        int wrong   = 0;
        float worst = 0.0f;

        for (int f = 0; f < frames; f++) {
            // Deliberately not a whole number of columns, chunks or tufts, so the
            // band lands out of step with every grid it is built on — and far
            // enough each frame that the walk crosses chunk borders, which is
            // what brings new ground into the band.
            const Rectangle view = {static_cast<float>(f) * 53.0f - 500.0f, -300.0f, 1000.0f, 600.0f};

            world.Update(view);

            plain.ForgetGrass();
            plain.Update(view);

            const sod::Blades kept  = world.Grass();
            const sod::Blades again = plain.Grass();

            if (kept.count != again.count || kept.firstColumn != again.firstColumn) {
                std::printf("frame %d: band differs (%d at %d against %d at %d)\n", f, kept.count, kept.firstColumn,
                            again.count, again.firstColumn);
                wrong++;
                continue;
            }

            for (int i = 0; i < kept.count; i++) {
                const float dTop   = std::fabs(kept.top[i] - again.top[i]);
                const float dCover = std::fabs(kept.cover[i] - again.cover[i]);

                checked++;

                worst = std::max({worst, dTop, dCover});

                if (dTop > 0.0f || dCover > 0.0f) {
                    if (wrong < 10) {
                        std::printf("frame %d column %d: top %.4f against %.4f, cover %.2f against %.2f\n", f,
                                    kept.firstColumn + i, kept.top[i], again.top[i], kept.cover[i], again.cover[i]);
                    }

                    wrong++;
                }
            }
        }

        std::printf("\n%d columns checked over %d frames, %d differ, worst %.6f\n", checked, frames, wrong, worst);

        CloseWindow();
        return (wrong == 0) ? 0 : 1;
    }

    if (timing) {
        Inventory timed{};

        ReportFrame(world, grove, timed,
                    {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3]))},
                    (argc >= 5) ? std::atoi(argv[4]) : 20);

        CloseWindow();
        return 0;
    }

    if (settling) {
        ReportSettling(world,
                       {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                        static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))},
                       (argc >= 7) ? std::atoi(argv[6]) : 600);

        CloseWindow();
        return 0;
    }

    if (assaying) {
        ReportOre(world, world.Settings(),
                  {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                   static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

        CloseWindow();
        return 0;
    }

    if (digging) {
        // The world's own settings and not the ones written above, because the
        // coverage cutoffs are measured into them by the constructor and an
        // uncalibrated copy carves nothing at all.
        terrain::Settings tuned = world.Settings();

        // Any cave setting, overridden as `name=value` after the region, so that
        // finding a set of them is a loop in a shell rather than a rebuild for
        // every value. Named rather than positional because a sweep usually moves
        // two knobs and a row of bare numbers is unreadable a day later.
        terrain::CaveSettings &c = tuned.caves;

        const std::array<std::pair<const char *, float *>, 18> knobs = {{
            {"region", &c.regionCoverage},
            {"shallow", &c.regionCoverageShallow},
            {"deepens", &c.regionDeepens},
            {"crust", &c.crust},
            {"chance", &c.systems.chance},
            {"span", &c.systems.cellSpan},
            {"rise", &c.systems.cellRise},
            {"step", &c.systems.stepLength},
            {"wander", &c.systems.wander},
            {"squash", &c.systems.squash},
            {"radius", &c.systems.radius},
            {"deep", &c.systems.radiusAtDepth},
            {"room", &c.systems.roomChance},
            {"swell", &c.systems.roomSwell},
            {"floor", &c.systems.roomFloor},
            {"mouth", &c.systems.entranceChance},
            {"fret", &c.roughness.amplitude},
            {"lobe", &c.roughness.lobeAmplitude},
        }};

        for (int a = 6; a < argc; a++) {
            const char *split = std::strchr(argv[a], '=');

            const auto knob = (split != nullptr)
                                ? std::find_if(knobs.begin(), knobs.end(),
                                               [&](const auto &k) {
                                                   return std::strncmp(k.first, argv[a],
                                                                       static_cast<std::size_t>(split - argv[a])) == 0
                                                          && k.first[split - argv[a]] == '\0';
                                               })
                                : knobs.end();

            if (knob == knobs.end()) {
                std::printf("unknown setting '%s'\n", argv[a]);

                CloseWindow();
                return 1;
            }

            *knob->second = static_cast<float>(std::atof(split + 1));
        }

        // Recalibrated after the overrides, since three of them are coverages and
        // a coverage is only a share once its cutoff has been measured.
        terrain::Calibrate(tuned);

        ReportCaves(tuned, {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                            static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))});

        CloseWindow();
        return 0;
    }

    // What has been picked up. The counterpart of Editor::Collected for anything
    // that is not a material — see item.h for why the two are separate tables.
    Inventory inventory{};

    // The two ends of a day, and they are two colours rather than one turned down.
    //
    // Noon is the near-neutral light this world was lit by before there was a clock;
    // midnight is a fiftieth of it and blue where the day is not. Both are radiances
    // and neither can be read as a brightness — light reaches the screen through an
    // exposure curve, so this midnight is a readable dark rather than the near-black
    // the ratio suggests, and a torch stops washing out against it and starts
    // reading as the warm thing it is.
    world.SetDaylight({2.6f, 2.8f, 3.1f}, {0.05f, 0.06f, 0.09f});

    if (probing) {
        const Rectangle strip = {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])),
                                 static_cast<float>(std::atof(argv[4])), static_cast<float>(std::atof(argv[5]))};

        Inventory probed{};

        DrawProbe(world, grove, probed, strip, argv[6], (argc >= 8) ? std::atoi(argv[7]) : 1,
                  (argc >= 9) ? static_cast<float>(std::atof(argv[8])) : 0.0f,
                  (argc >= 10) ? (std::atoi(argv[9]) != 0) : true, (argc >= 11) ? std::atoi(argv[10]) : 0,
                  (argc >= 12) ? std::atoi(argv[11]) : -1, (argc >= 13) ? std::atoi(argv[12]) : -1);

        CloseWindow();
        return 0;
    }

    if (reading) {
        const float x  = static_cast<float>(std::atof(argv[2]));
        const float y0 = static_cast<float>(std::atof(argv[3]));
        const float y1 = static_cast<float>(std::atof(argv[4]));

        world.Update({x - 128.0f, y0 - 128.0f, 256.0f, y1 - y0 + 256.0f});
        ReportColumn(world, settings, x, y0, y1);

        CloseWindow();
        return 0;
    }

    // Dropped in above the ground at the origin rather than at a fixed height,
    // since the surface there is now wherever the relief put it.
    Player player({0.0f, terrain::Height(0.0f, settings) - 96.0f});

    // The dust under the character's feet. Beside the player rather than inside it,
    // because what a foot throws up is the ground's answer and not the body's — see
    // scuff.h.
    scuff::Trail trail;
    Editor editor;

    LiquidLayer liquids;

    LightLayer lights;

    Backdrop backdrop;
    backdrop.Create();

    Camera2D camera = {};
    camera.offset   = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f};
    camera.target   = player.Centre();
    camera.zoom     = static_cast<float>(config::kMinZoom);

    float accumulated = 0.0f;
    float lantern     = config::kLanternStrength;

    // The last thing the world said back, and how long it has left on screen.
    const char *notice = "";
    float noticeFor    = 0.0f;

    debug_view::Toggles debug;

    // Whether the inventory panel is up.
    //
    // Held here beside the debug toggles rather than inside the inventory, for
    // the reason debug_view::Toggles gives for the same choice: what is on the
    // screen is the state of the screen, and the loop is where the screen is
    // decided. It is also what this gates the whole simulation on, and a gate
    // hidden inside the thing it gates is a gate nobody finds.
    bool packOpen = false;

    // Whether the brush is waiting for the hand to come off the button.
    //
    // A click outside the panel dismisses it, and the button is still down for
    // the several frames a human click lasts — while the brush reads it held
    // rather than pressed. Without this, getting out of the inventory digs a
    // hole in whatever was behind it.
    bool holdOff = false;

    // The chat line and its log.
    //
    // Held out here with the other things that are states of the screen. Everything
    // that reads a key is gated on it being shut, which is the whole discipline the
    // feature needs: a box that takes typing while the character still answers to
    // WASD is a box that walks you off a cliff mid-sentence.
    console::Console chat;

    chat.Say("press T to type a command — /help lists them", console::Tone::Note);

    // `--profile [frames] [still]` plays the game as normal, with nobody at the
    // keys, and reports where the frame went before it closes. The only way to
    // see the draw beside the simulation, which the headless reports cannot show.
    const int profileFrames = profiling ? ((argc >= 3) ? std::atoi(argv[2]) : 600) : 0;

    // Full screen, always, because that is how the game is played and because
    // nearly everything in the frame is priced by the area of the view: the light
    // solves a region around it, the water steps one, and the ground is drawn
    // over it. A measurement in a small window is a measurement of a different
    // game.
    if (profiling) {
        const int monitor = GetCurrentMonitor();

        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));

        if (!IsWindowFullscreen()) ToggleFullscreen();
    }

    // And flying across the world rather than standing still, unless asked
    // otherwise. Standing still is the one thing a player never does, and it is
    // the case every cache in here is at its best: nothing streams, nothing is
    // invalidated, and the frame reads far better than it plays.
    const bool flying = profiling && !(argc >= 4 && TextIsEqual(argv[3], "still"));

    // The first frames are chunk generation and shader compilation, which is not
    // what a steady frame costs.
    constexpr int kWarmup = 60;

    int played = 0;

    if (profiling) profile::Begin();

    while (!WindowShouldClose()) {
        if (profiling) {
            played++;

            if (played > profileFrames + kWarmup) break;
            if (played == kWarmup) profile::Reset();

            profile::Frame();
        }

        const float dt = GetFrameTime();

        // The wall clock rather than the weather's, so a line stays readable for as
        // long as it takes to read whatever F7 is doing to the sky.
        chat.Step(dt);

        // Opened on T, and never while the pack is up or while it is already open —
        // in the second case the T belongs in the box, and Console::Open would eat
        // it anyway.
        if (!chat.IsOpen() && !packOpen && IsKeyPressed(KEY_T)) chat.Open();

        // Everything below asks whether the player is typing before it reads a key.
        const bool typing = chat.IsOpen();

        if (typing) {
            const std::string sent = chat.Read();

            if (!sent.empty()) RunCommand(sent, world, grove, inventory, player, camera, chat);
        }

        // The frame can change size between any two frames, so the two things that
        // are sized to it are set from it every frame rather than when it changes.
        // Nothing then has to notice a resize, and there is no path where something
        // was told about one and something else was not.
        camera.offset = {GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f + GetScreenHeight() / 4.0f};
        liquids.Fit(GetScreenWidth(), GetScreenHeight());
        backdrop.Fit(GetScreenWidth(), GetScreenHeight());

        // How far in the view is set, read before anything asks what the view
        // covers. Outside the panel gate, unlike everything else about the world:
        // a player who opened the inventory to look at something and wants to look
        // at it closer is asking about the screen and not about the world.
        ReadZoom(camera);

        // Chunks are generated over the simulated band, not merely the visible
        // one. A write-back to a vertex whose chunk is absent is dropped, which
        // would quietly destroy the liquid that flowed there.
        const Rectangle active = Expand(ViewBounds(camera), kSimulationMargin);

        // Read before the gate below, since it is the one key that has to work on
        // both sides of it. Escape closes the panel as well, which it can only do
        // because the exit key was cleared at startup — see SetExitKey.
        if (!typing && (IsKeyPressed(KEY_TAB) || (packOpen && IsKeyPressed(KEY_ESCAPE)))) {
            packOpen = !packOpen;

            // Anything still on the cursor goes into the world rather than into
            // nowhere. Closing over a full hand is the one way a stack could be
            // held by a panel that is no longer drawn.
            if (!packOpen) {
                holdOff = true;

                const Stack held = inventory.Release();

                if (!held.Empty()) {
                    grove.Fallen().Toss(held, player.Centre(), GetScreenToWorld2D(GetMousePosition(), camera),
                                        world.Sky().Time());
                }
            }
        }

        // Streaming carries on with the panel up. It is the one step that is
        // about where the view is rather than about time passing, the view is not
        // going anywhere, and leaving it running means there is no state to
        // catch up on when the panel closes.
        world.Update(active);

        if (packOpen) {
            const Inventory::Gesture gesture = inventory.Update();

            if (!gesture.thrown.Empty()) {
                grove.Fallen().Toss(gesture.thrown, player.Centre(), GetScreenToWorld2D(GetMousePosition(), camera),
                                    world.Sky().Time());
            }

            if (gesture.close) {
                packOpen = false;
                holdOff  = true;
            }
        }

        if (holdOff && IsMouseButtonUp(MOUSE_BUTTON_LEFT) && IsMouseButtonUp(MOUSE_BUTTON_RIGHT)) holdOff = false;

        // Everything from here to the light solve is the world moving, and none
        // of it runs while the panel is up. Single player, so a panel is a pause;
        // and a drop left falling behind an open inventory is a drop that has
        // timed out and gone by the time it is looked at again.
        if (!packOpen) {
            PROFILE_ZONE("grove.Update");

            // Grown over the visible band rather than the simulated one. A plant is
            // drawn and nothing else — it holds no liquid and steps no automaton — so
            // there is nothing about one off screen that has to have settled by the
            // time it scrolls in.
            grove.Update(world, ViewBounds(camera), player.Centre(), world.Sky().Time(), dt, inventory);

            // The two that read the mouse wait out the click that closed the
            // panel; the plants above do not, since nothing about them is a
            // click.
            if (!holdOff && !typing) {
                hotbar::Update(inventory, ZoomModifier());

                // Handed the player's body from before it moves this frame, which
                // is what the reach is measured from, which side an overflowing
                // dig throws its blocks out on, and the room no block may be laid
                // in. A frame of lag at a run is six pixels against a reach of
                // ninety-six, and a block laid into where the body is about to be
                // is a block the body is stopped by rather than buried in.
                const char *said =
                    editor.Update(world, inventory, grove, camera, player.Bounds(), world.Sky().Time());

                if (said != nullptr) {
                    notice    = said;
                    noticeFor = kNoticeTime;
                }
            }
        }

        if (!typing) debug_view::ReadToggles(debug);
        if (!typing && IsKeyPressed(KEY_R)) world.Reset();

        // An action rather than a state, so it is read here beside the other one and
        // not held in the debug toggles. Asking again while one is running queues
        // another quarter.
        if (!typing && IsKeyPressed(KEY_F8)) world.SkipToQuarter();

        // Holds a season, for looking at one rather than waiting a year. There is
        // no year yet, so without this the world is always in spring — which is
        // exactly why the key exists: the whole seasonal path can be exercised and
        // judged before there is a calendar to drive it.
        if (!typing && IsKeyPressed(KEY_F9)) world.CycleSeason();

        // And holds a weather, for the same reason one stop further on: which
        // weather blows is a pure function of the spell and the seed, so a storm
        // is something to be waited for rather than something to be looked at.
        // Everything a gale does — the rain, the shade, the gusts, the leaves
        // coming off a wood — has to be judged with one blowing.
        if (!typing && IsKeyPressed(KEY_F12)) world.CycleWeather();

        // An action beside the other two, and for the reason F8 gives: the debug
        // toggles hold the state of the screen, and this is not a state.
        if (!typing && IsKeyPressed(KEY_F11)) inventory.Stock();

        // Turned up and down while walking, since how much light the player
        // carries is a balance question and the only way to settle it is to be
        // underground at each setting. Zero is a valid answer: it leaves the
        // dark to torches alone.
        if (!typing && IsKeyPressed(KEY_COMMA)) lantern = std::max(lantern - config::kLanternStep, 0.0f);
        if (!typing && IsKeyPressed(KEY_PERIOD)) lantern = std::min(lantern + config::kLanternStep, config::kLanternMax);

        // The accumulator is not fed while the panel is up, rather than being fed
        // and the stepping skipped. Skipping alone would leave it holding
        // kMaxAccumulated by the time the panel closed and run a quarter of a
        // second of water in one frame — the whole pond would jump.
        if (!packOpen) {
            accumulated = std::min(accumulated + dt, kMaxAccumulated);

            while (accumulated >= kWaterStep) {
                world.StepWater(active);
                accumulated -= kWaterStep;
            }

            // Taken before the step, because a landing is only visible from the
            // near side of it: once the body is resting on the ground its downward
            // speed has already been cleared, and a puff sized from that is a puff
            // every landing throws at nothing.
            const float fell = std::max(player.Velocity().y, 0.0f);

            // A neutral input while typing rather than no update at all: the
            // character has to keep falling and keep being pushed out of walls, it
            // simply must not answer to the keys that are spelling a command.
            // Flown across the world under its own power while profiling, since
            // standing still is the one thing a player never does and the one
            // case every cache in here is at its best. See `--profile`.
            const PlayerInput moves =
                flying ? PlayerInput{.moveX      = 1.0f,
                                     .jumpHeld   = false,
                                     .flyToggled = (played == kWarmup / 2),
                                     .sprintHeld = true}
                : typing ? PlayerInput{}
                         : ReadPlayerInput(camera, !packOpen && !holdOff && editor.Left() == Editor::Hand::Chop
                                                       && IsMouseButtonDown(MOUSE_BUTTON_LEFT));

            PROFILE_ZONE("player");

            player.Update(moves, world, dt);

            trail.Update(world, player.Bounds(), std::fabs(player.Velocity().x), fell, player.IsGrounded(),
                         world.Sky().Time());
            FollowPlayer(camera, player, dt);
        }

        // Only on the frame the swing began. The strike box is live for the whole
        // window, so reading that instead lands nine blows per swing.
        if (!packOpen && !typing && player.AttackStarted()) {
            // Where the blow lands. A swing off the mouse lands where the cursor is,
            // because that is what the player aimed at and the cursor has already
            // been told it is over something choppable; a swing off the key lands in
            // front of the character, which is all a key can mean. Both are bounded
            // by the same reach — the mouse one by the editor, which refuses to
            // choose the axe out of range at all.
            const bool aimed = editor.Left() == Editor::Hand::Chop;

            const Rectangle swing = aimed ? Rectangle{editor.Aim().x - kAimedBlow, editor.Aim().y - kAimedBlow,
                                                      kAimedBlow * 2.0f, kAimedBlow * 2.0f}
                                          : player.AttackHitbox();

            grove.Strike(swing, 1.0f, player.Centre(), world.Sky().Time());

            // And whatever grass the same swing went through. A tuft gives up
            // fibre where a tree gives up wood, into the same pile on the ground.

            const int mown = world.MowGrass(swing, world.Sky().Time());

            if (mown > 0) {
                const Vector2 from = {swing.x + swing.width * 0.5f, swing.y + swing.height * 0.5f};

                // Thrown away from the player rather than towards them, which is
                // the side the blow came from and the way the wood already goes.
                const float away = (from.x < player.Centre().x) ? -1.0f : 1.0f;

                grove.Fallen().Scatter(ItemsOf(Item::Fibre, mown), from, away, world.Sky().Time());
            }
        }

        noticeFor = std::max(noticeFor - dt, 0.0f);

        // The light goes out with the solve and not on its own.
        //
        // AddLight is re-offered every frame and cleared by each solve, so the
        // two have to be skipped together: skipping only the offer would let the
        // solve run against an empty list and put the lantern out, and skipping
        // only the solve would leave offers piling up against a field nothing is
        // clearing. Left alone, the light already on screen is the light of a
        // world that has stopped moving, which is the right answer.
        if (!packOpen) {
            // Re-offered every frame rather than registered once. A light that has
            // to be renewed to keep burning needs nothing told to it when the thing
            // carrying it moves, and nothing told to it when that thing is gone.
            world.AddLight(player.Centre(), Lantern(lantern), config::kLanternRadius);

            // Drifted before the light is solved, because the shade the cloud casts is
            // read during the solve. Advancing it afterwards would light every frame by
            // the sky of the frame before it, which nothing would look wrong about and
            // which would be wrong.
            {
                PROFILE_ZONE("StepWeather");

                world.StepWeather(dt * (debug.fastWeather ? debug_view::kFastWeather : 1.0f));
            }

            // Offered on the same terms as the lantern above, and for the same
            // reason: a canopy that has to be re-offered to keep shading needs
            // nothing told to it when the tree is felled.
            {
                PROFILE_ZONE("grove.Shade");

                grove.Shade(world, world.Sky().Time());
            }

            // Solved after the world has finished moving, so the light matches the
            // frame it is about to be drawn over rather than the one before it.
            world.StepLight(active);

            {
                PROFILE_ZONE("lights.Update");

                lights.Update(world.Light());
            }
        }

        // The ground, rasterised into a picture per chunk. Out here with the two
        // below because it opens a texture of its own, which cannot be done
        // inside a frame.
        world.PaintChunks(ViewBounds(camera));

        // Captured before the frame opens, since it renders to its own target.
        {
            PROFILE_ZONE("liquids.Capture");

            liquids.Capture(world, ViewBounds(camera), camera);
        }

        // And the world itself, when it is about to be put behind a panel. Same
        // constraint, one step further: a texture mode cannot be opened inside a
        // frame, so the scene is drawn into the backdrop's target out here and
        // only the blurred result is drawn once the frame is open.
        if (packOpen) {
            backdrop.Capture();
            DrawScene(world, grove, inventory, player, trail, editor, liquids, lights, camera, debug, !packOpen);
            backdrop.Finish();
        }

        BeginDrawing();

        {
            PROFILE_ZONE("DrawScene");

            if (packOpen) backdrop.Compose(config::kPanelDim);
            else DrawScene(world, grove, inventory, player, trail, editor, liquids, lights, camera, debug, !packOpen);
        }

        {
            PROFILE_ZONE("DrawHud");

            DrawHud(world, grove, player, editor, camera, debug, lantern, notice, noticeFor);

            // The panel replaces the bar rather than sitting over it, since it draws
            // those same nine slots as its own bottom row.
            if (packOpen) inventory.Draw();
            else hotbar::Draw(inventory);

            // Over everything, panel and bar included: an answer that arrived behind the
            // inventory is an answer nobody read.
            chat.Draw(dt);
        }

        {
            // The frame handed over and waited on. With a target frame rate set
            // this is also where the slack goes, so it reads as the whole of
            // whatever the rest of the frame did not use.
            PROFILE_ZONE("EndDrawing");

            EndDrawing();
        }
    }

    if (profiling) profile::Report("frame");

    world.UnloadPainted();
    grove.Unload();
    lights.Unload();
    liquids.Unload();
    backdrop.Unload();
    CloseWindow();

    return 0;
}
