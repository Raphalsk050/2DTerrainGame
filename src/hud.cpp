#include "hud.h"

#include "flora.h"
#include "hotbar.h"
#include "inventory.h"
#include "view.h"

#include <cmath>

namespace {

// Badge showing how wide the brush is, next to the bar that decides what it
// works with.
//
// Size is the one property of the brush with no mark of its own out in the
// world — the ring shows where it is and what it would put down, but not what
// the last press of the size key did, since a ring four pixels wider is not a
// thing anyone sees change.
void BrushSize(const Editor &editor) {
    // Grey out of reach, which is the same thing the ring in the world says and
    // is worth saying twice: out there the ring is the only mark on screen, and
    // a player who has not noticed it is a player wondering why the button
    // stopped working.
    const Color color = editor.Reachable() ? Color{190, 198, 212, 255} : Color{120, 126, 138, 255};

    // The brush has no part in building, so its size is not what this badge is
    // for while a piece is in hand. It says which hand the player is holding
    // instead — the grid out in the world already says where, and the size keys
    // do nothing here.
    const char *text = TextFormat("brush %dx%d  (- / +)%s", editor.Span(), editor.Span(),
                                  editor.Building() ? "  |  building" : "");
    const int width  = MeasureText(text, 14);

    // Sat just clear of the bar, which reaches 76 pixels up from the bottom.
    const Rectangle badge = {(GetScreenWidth() - width) / 2.0f - 8.0f, GetScreenHeight() - 104.0f, width + 16.0f,
                             22.0f};

    DrawRectangleRec(badge, {30, 34, 42, 220});
    DrawRectangleLinesEx(badge, 2.0f, color);
    DrawText(text, static_cast<int>(badge.x + 8.0f), static_cast<int>(badge.y + 4.0f), 14, color);
}


} // namespace

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
void hud::Label(const char *text, int x, int y, Color colour) {
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

// Everything drawn in the coordinates of the frame rather than of the world.
void hud::Draw(const World &world, const Grove &grove, const Player &player, const Editor &editor,
             const Camera2D &camera, const debug_view::Toggles &debug, float lantern, const char *notice,
             float noticeFor) {
    // Outside the camera transform, unlike every other overlay: what it shows is
    // the image that was baked, not a place in the world.
    if (debug.atlas) grove.DrawSheet();

    // Near white, because the outline under it is what does the separating now.
    const Color ink = {238, 243, 250, 255};

    // And the wet lines keep their meaning by going pale blue rather than dark.
    const Color wet = {152, 206, 255, 255};

    Label("A/D: move  |  shift: run  |  space: jump  |  S: crouch  |  mouse: aim  |  F: fly  |"
              "  pg up/dn or ctrl+wheel: zoom",
              10, 10, ink);
    Label("left: swing - dig, chop, cut grass  |  right: place what is held  |"
              "  planks and cobble build on the grid  |"
              "  1-9 or wheel: slot  |  tab: inventory  |  esc: menu  |  - / +: brush size  |  R: regenerate",
              10, 28, ink);
    Label(TextFormat("V: vertices  |  B: bounce %s  |  L: light limits  |  F3: chunks  |  F4: height grid  |"
                         "  F5: light field  |"
                         "  F6: unlit %s  |  F7: fast weather %s  |  F8: next quarter  |  F9: season %s  |"
                         "  F10: sheet  |  F11: stock up  |  F12: weather %s  |  C: cloud shade %s  |"
                         "  , . : lantern %.1f",
                         world.LightSettings().bounce ? "on" : "off", debug.unlit ? "on" : "off",
                         debug.fastWeather ? "on" : "off", flora::kSeasonNames[world.Sky().Turn().index],
                         (world.Sky().ForcedMood() < 0) ? "auto" : world.Sky().MoodName(),
                         world.SkyCover() ? "on" : "off", lantern),
              10, 46, ink);

    Label(TextFormat("chunks: %d (%d pinned)   edits kept: %d   plants: %d (%d drawn, %d kept)   rays: %ld",
                         world.ResidentChunks(), world.PinnedChunks(), world.RememberedEdits(), grove.VisiblePlants(),
                         grove.DrawnPlants(), grove.RememberedPlants(), world.Light().Rays()),
              10, 70, ink);

    Label(TextFormat("on the ground: %d", grove.Fallen().Live()), 10, 160, ink);

    const Vector2 centre = player.Centre();
    const auto under     = editor.Under();

    Label(TextFormat("y: %d   under cursor: %s   light here: %.2f   light at cursor: %.2f",
                         static_cast<int>(centre.y), under.has_value() ? Def(*under).name : "open",
                         world.LightLevelAt(centre), world.LightLevelAt(editor.Aim())),
              10, 88, ink);

    // The weather, and then the cloud standing in it. Read together they show the
    // chain working: the weather sets the level, the sky over this spot fills to it,
    // the shadow follows the cloud that casts it, and the rain leaves from the
    // underside of that same cloud rather than from a height of its own.
    const weather::Weather &sky = world.Sky().Now();

    Label(
        TextFormat("weather: %-8s  sky %.0f%% full   rain %.0f%%   |   here: %.0f%% cloud  %.0f%% shade  base y %d",
                   sky.name, sky.cover * 100.0f, sky.rain * 100.0f, world.Sky().CoverAt(centre.x) * 100.0f,
                   world.Sky().ShadeAt(centre.x) * 100.0f, static_cast<int>(world.Sky().UndersideAt(centre.x))),
        10, 106, sky.rain > 0.0f ? wet : ink);

    // And the wind, on its own line because it is the one reading with three parts
    // that can disagree. The mood's figure is what the table says, the gust here is
    // what this column is actually getting, and the share is what everything rooted
    // to the ground reads — so a wood standing still under a gale can be traced to
    // whichever of the three ate it, instead of being guessed at.
    Label(TextFormat("wind: %-4.0f px/s mean   %+5.0f here   |   push %+.2f   gale %.0f   |   %d adrift", sky.wind,
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

    Label(TextFormat("%02d:%02d  %-5s   daylight %.0f%%   |   humidity %.0f%%  [%.*s%.*s]", static_cast<int>(hours),
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

        Label(notice, (GetScreenWidth() - width) / 2, GetScreenHeight() - 140, {255, 214, 140, 255});
    }

    BrushSize(editor);
}
