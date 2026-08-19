#include "core/registry.h"
#include "core/tool.h"
#include "item/item_def.h"
#include "probes/report.h"
#include "raylib.h"
#include "world/element.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// `--tools` — how long every material takes to break with every tool.
//
// The claim a tool table makes is a claim about *seconds*, and seconds are the one
// thing none of the three tables it is spread across actually says. A material's row
// gives a hardness and a `Tool`; an item's row gives a kind and a tier; `BreakSeconds`
// is where they meet. Everything a player feels — that a pickaxe is worth making,
// that it is worth making a stone one after, that it does nothing at all for dirt —
// is a consequence of arithmetic nobody can see by reading either table.
//
// So the whole grid is printed, through `BreakSeconds` itself and never a copy of it.
//
// And it reports a verdict, because two of the ways this can be wrong are silent:
//
//   - **A tool that is no faster than a fist at the thing it is for.** A row with a
//     `kind` that no material asks for, or a tier of one, compiles and plays and is
//     simply furniture.
//   - **A tool that is faster at something it is not for.** Minecraft gives the tier
//     multiplier only where the tool is the right one, and the day that rule is
//     dropped one pickaxe ends the need for every other tool in the game.
namespace {

const char *Named(Tool kind) {
    switch (kind) {
    case Tool::Hand: return "hand";
    case Tool::Pick: return "pick";
    case Tool::Shovel: return "shovel";
    case Tool::Axe: return "axe";
    case Tool::Sword: return "sword";
    }

    return "?";
}

int Run(const probes::Bench &bench) {
    (void)bench;

    // Every tool in the game, plus the bare hand it is measured against.
    std::vector<std::pair<std::string, tool::Kit>> hands;

    hands.emplace_back("bare hand", tool::Kit{});

    for (int i = 0; i < item::Count(); i++) {
        const ItemDef &def = item::Table().At(i);

        if (!def.tool.Any()) continue;

        hands.emplace_back(def.name, def.tool);
    }

    std::printf("\n%-13s %8s %7s %6s", "", "hardness", "asks", "needs");

    for (const auto &[name, kit] : hands) std::printf(" %13s", name.c_str());

    std::printf("\n");

    std::printf("%-13s %8s %7s %6s", "", "", "for", "it");

    for (const auto &[name, kit] : hands) {
        std::printf(" %13s", (kit.kind == Tool::Hand) ? "-" : TextFormat("%s x%.0f", Named(kit.kind), kit.speed));
    }

    std::printf("\n\n");

    bool idle    = false; // A tool no faster than a fist at what it is for.
    bool leaking = false; // A tool faster at something it is not for.

    for (std::size_t e = 0; e < kElementCount; e++) {
        const ElementDef &def = kElements[e];
        const auto element    = static_cast<Element>(e);

        const float bare = BreakSeconds(element, tool::Kit{});

        std::printf("%-13s %8.1f %7s %6s", def.name, def.hardness, Named(def.tool), def.needsTool ? "yes" : "no");

        for (const auto &[name, kit] : hands) {
            const float took = BreakSeconds(element, kit);

            std::printf(" %13s", TextFormat("%.2f s", static_cast<double>(took)));

            if (kit.kind == Tool::Hand) continue;

            const bool right = def.tool != Tool::Hand && kit.kind == def.tool;

            // Faster than a fist where it has no business being. The tolerance is a
            // hundredth of a second, which is far under any real difference and well
            // over what a float rounds away.
            if (!right && took < bare - 0.01f) leaking = true;
        }

        std::printf("\n");
    }

    std::printf("\n");

    // And the other way round: every tool has to be worth carrying for something.
    for (const auto &[name, kit] : hands) {
        if (kit.kind == Tool::Hand) continue;

        bool helps = false;

        for (std::size_t e = 0; e < kElementCount; e++) {
            const auto element = static_cast<Element>(e);

            if (BreakSeconds(element, kit) < BreakSeconds(element, tool::Kit{}) - 0.01f) helps = true;
        }

        // A sword is the deliberate exception and says so on its own row: no material
        // asks for `Tool::Sword`, and what it is for is the swing. Anything else that
        // helps with nothing is a row that was meant to do something.
        if (helps || kit.kind == Tool::Sword) continue;

        std::printf("IDLE: '%s' breaks nothing faster than a bare hand does\n", name.c_str());

        idle = true;
    }

    if (leaking) std::printf("LEAKING: a tool is faster at something it is not the tool for\n");

    std::printf("%s\n\n", (!idle && !leaking) ? "every tool is faster at what it is for, and at nothing else"
                                              : "the tool table does not do what it says");

    // What a sword is for, since the grid above cannot show it: a fist and every
    // blade against the same figure.
    std::printf("%-13s %s\n", "", "damage per swing, over a bare fist");

    for (const auto &[name, kit] : hands) {
        if (kit.damage <= 0) continue;

        std::printf("%-13s +%d\n", name.c_str(), kit.damage);
    }

    std::printf("\n");

    return (!idle && !leaking) ? 0 : 1;
}

const probes::Report row = {
    .name  = "--tools",
    .wants = 2,
    .shows = false,
    .blurb = "--tools - how long every material takes to break with every tool",
    .run   = Run,
};

const registry::Registrar<probes::Report> entry{row};

} // namespace
