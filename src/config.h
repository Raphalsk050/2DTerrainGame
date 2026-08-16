#pragma once

#include "raylib.h"

// Project-wide configuration constants.
namespace config {

// Size the window opens at. The window is resizable, so this is a starting point
// and not a bound: anything that needs to know how large the frame is now asks
// raylib rather than reading these.
inline constexpr int kScreenWidth  = 1000;
inline constexpr int kScreenHeight = 600;

// Smallest the window may be dragged to. Below this the hotbar is wider than the
// frame and the head-up display runs off the side of it.
inline constexpr int kMinScreenWidth  = 640;
inline constexpr int kMinScreenHeight = 400;

inline constexpr int kTargetFps = 60;

// How far the view may be pushed in, as whole multiples of one screen pixel per
// world pixel. See ReadZoom for why nothing between them is offered.
//
// One is the floor: it is the framing every size in this world was chosen
// against, and it shows more ground than any other setting. Three is as far in as
// is worth going — at that the thousand pixels of the window are three hundred
// and thirty of world, which is a couple of mature trees across, and past it the
// player can no longer see far enough ahead to run.
inline constexpr int kMinZoom = 1;
inline constexpr int kMaxZoom = 3;

// Spacing in pixels between neighbouring grid vertices.
inline constexpr int kResolution = 6;

// Side of one build cell, in world pixels.
//
// Written as a whole number of lattice steps rather than as a size that looked
// right, and that is the whole of the requirement: a cell has to hold the same
// vertices wherever it falls. At three steps every cell is exactly the same
// three-by-three, so two blocks of one material are the same block. At a size
// that did not divide — sixteen, say, which is what a block used to be — a cell
// would take two vertices across in one place and three in the next, and the
// same material laid twice would come out two different shapes.
//
// Three and not two or four. Two is twelve pixels, which is the width of the
// character and reads as gravel rather than as masonry; four is twenty-four,
// which is the world's terrace riser, and a wall built of them steps a third of
// the character's height at a time. Eighteen sits between, and it is what
// kBlockSide is now written against rather than the other way about.
inline constexpr int kBuildCell = 3 * kResolution;

// Lattice vertices along one side of a build cell, and in one whole cell.
inline constexpr int kBuildCellVertices = kBuildCell / kResolution;
inline constexpr int kBuildCellArea     = kBuildCellVertices * kBuildCellVertices;

// Side length of the debug square drawn on each vertex.
inline constexpr float kVertexSize = 2.0f;

// Spacing of the world height grid, in pixels. It is the scale the spawn bands
// in the element table are written against: a band reads as a range of heights,
// and this is the ruler those heights are counted on.
inline constexpr int kHeightGridStep = 64;

// Interval at which a height line is drawn brighter and labelled. Labelling
// every line at the fine step would leave the screen unreadable.
inline constexpr int kHeightGridMajor = 256;

// Radius around a vertex within which a click is attributed to it. Half the
// spacing gives each vertex exactly its own neighbourhood.
inline constexpr float kPickRadius = kResolution / 2.0f;

// Transparency of the liquid layer, applied once to the whole layer instead of
// to each piece drawn into it.
inline constexpr unsigned char kLiquidAlpha = 170;

// Draws the light as one flat block per probe rather than blending between
// them.
//
// It is a look, and it is also the more honest of the two: a probe knows the
// light over its own patch and nothing about the patch next door, and blending
// between them carries the brightness of a lit wall a full probe into the cave
// beside it, and the darkness of the cave back into the wall. Blocks keep each
// answer inside the square it was solved for.
inline constexpr bool kBlockyLight = true;

// Draws the world as square pixels rather than as smooth polygons.
//
// It changes only how the field is rasterised, not what the field says, so the
// shape is the same one the contour would have drawn. Nothing else on screen is
// touched: the character keeps whatever resolution its own art has, which is
// the reason for doing it here instead of rendering the whole frame small and
// blowing it back up.
inline constexpr bool kPixelArt = true;

// Side of one square, in world units.
//
// It has to divide **kBuildCell and the half-step the build grid is offset by**,
// and that is a stricter rule than the one written here before — which asked for
// a divisor of kResolution and then gave five, which is not one.
//
// Nothing noticed while the world was only terrain. The rasteriser anchors its
// squares to the world and gives each lattice cell the squares whose centres fall
// inside it, so a run of world that is not a whole number of squares comes out as
// a different number of them depending on where it falls. On a hillside that is
// invisible: no two stretches of it are meant to look alike anyway. On a wall of
// blocks it is the whole of what is wrong — identical pieces drawn three squares
// wide in one column and four in the next, which reads as bites taken out of the
// wall.
//
// A block's edges land at three plus multiples of six: the contour crosses half a
// lattice step out from the last filled vertex, and the vertices are at multiples
// of six. So the square has to divide three, and three is the only size worth
// having (one is twenty-five times the work). Five missed both by two pixels and
// six misses the offset by three — measured, with `--build`, which now checks it.
//
// It costs. Full screen and flying, PaintChunks went 0.75 ms to 1.46 ms and the
// frame 24.0 to 26.6 — 42 fps to 38. That is the price of the world being drawn
// at a finer grain than it was, and it buys a wall that is a wall.
inline constexpr float kPixelSize = 3.0f;

// Side of one square of a plant, in world units.
//
// Finer than the world's own, and deliberately. A tree the size the world is
// built against — a mature one is five of the character — is some twenty squares
// tall at kPixelSize, and a trunk on that grid is three squares wide: one tone
// for the lit side, one for the shadowed, and nothing left for the one between.
// At this size a mature trunk is eight squares and a crown carries four greens
// with room for the notches and the dark accents the reference art is made of.
//
// Two and not two and a half, which is what half of kPixelSize would have been.
// The camera zooms only by whole multiples — see kMaxZoom — so a square of this
// size is always exactly this many pixels on screen times the zoom, and at two
// and a half every sprite would have been drawn with its columns alternating two
// pixels wide and three however the view was set. An integer is the whole of the
// requirement; nesting inside the ground's squares is not one, since the terrain
// fills its interior a whole lattice cell at a time and only steps its outline
// at kPixelSize — there is no texel grid there for this to line up with.
inline constexpr float kFloraPixel = 2.0f;

// Outlines every material along its own boundary.
//
// Off, a material is only its fill, and what separates it from what is beside
// it is the change of colour alone. Under pixel rasterisation this also saves
// the four extra samples a square spends finding out whether it is exposed, so
// turning it off is cheaper as well as plainer.
inline constexpr bool kDrawContours = false;

// The lantern the player carries.
//
// Strength is in the same unit the sky and a torch are measured in, so it can
// be read as a fraction of either. Deliberately low: enough to place a foot,
// not enough to survey a cavern, which is what keeps an ore seam something to
// be found rather than something visible from the mouth of the tunnel.
//
// Adjustable while the game runs, with the keys below, so the balance can be
// settled by walking around at each setting rather than by argument.
inline constexpr float kLanternStrength = 2.0f;
inline constexpr float kLanternRadius   = 10.0f;
inline constexpr Color kLanternGlow     = {255, 206, 150, 255};

// How much one press changes it, and how far it can be pushed either way.
inline constexpr float kLanternStep = 0.01f;
inline constexpr float kLanternMax  = 8.0f;

// Shader path, relative to the executable. The working directory is switched to
// the binary's own directory at startup, and the build copies assets there.
inline constexpr const char *kNoiseShaderPath = "assets/noise_filter_shader.fs";
inline constexpr const char *kBlurShaderPath  = "assets/blur_shader.fs";

// How far the world behind an open panel is pushed back.
//
// A little over half its brightness, with the blur doing the rest of the work.
// Darkening alone leaves the world legible and competing for the eye; blurring
// alone leaves it as bright as the panel over it. Between them at these amounts
// the world is plainly still there and plainly not the thing being looked at,
// which is the whole requirement.
//
// Deliberately short of the two thirds it started at. That was dark enough to
// read as night having fallen rather than as the world stepping back, and the
// point is a world one can still see oneself standing in.
inline constexpr float kPanelDim = 0.58f;

} // namespace config
