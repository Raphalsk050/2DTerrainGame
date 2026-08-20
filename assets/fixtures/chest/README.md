# Chest art

One PNG per **bank size**. The row already names this folder (`fixture::Def::art =
"chest"`), so adding, replacing or animating any of these is a file and nothing else —
no code, no table, no build.

## The files

| file | when it is drawn |
|---|---|
| `small_chest.png` | one chest standing alone |
| `medium_chest.png` | two joined |
| `large_chest.png` | three joined |
| `huge_chest.png` | four, if `joins` is ever raised to it |

The name is `<size>_<row name>.png`, and both halves come from the row — so there is no
filename anywhere in the table to misspell. As many are looked for as the row `joins`,
which is three today.

**A bank is one picture, not a row of tiles.** Two chests joined are one drawing of two
chests joined. That is why there is nothing here called `left` or `middle`: there are no
seams to hide, because there are no seams.

A size with no file falls back to the hand-drawn faces on the row, which *do* tile — so
a folder with only `small_chest.png` in it still draws a bank, just not the drawn one.
`--chest` says which sizes loaded and fails if one is missing.

## The canvas

**64 x 64**, the same canvas every tool is drawn on. Draw the chest anywhere in it: only
the drawn-on part reaches the screen, and it is stood on the floor of the cells the bank
occupies. There is no margin to get right and no ground line to line up with.

**One texel is one world pixel.** A build cell is 18 px, so a chest 20 texels wide sits
over one cell with a pixel of overhang either side, and one 42 texels wide sits over
three cells (54 px) with the floor showing at each end. Nothing is ever stretched to fill
its cells — the art is drawn at its own size and centred on the bank, which is what
furniture does. Draw it wider if you want it to cover more.

For scale: the character is 12 px wide and 26 tall, and a cell is 18.

## The animation

A file may be a **horizontal strip of 64-wide frames**: 64 px is one frame, 128 is two,
256 is four. **Frame 0 is shut and the last frame is wide open** — the lid is a
play-head between them, driven by whether this chest's bank is the one the player has
open, over `fixture::Def::opens` seconds.

A one-frame file is a chest that never appears to open, and is correct. That is what
these three are today.

The frames share one window, worked out as the union of what is drawn on across all of
them, so the chest does not jump between frames.

`sheet::Load` refuses a strip whose width is not a whole number of 64s rather than
drawing a smeared one.

## Facing

**Front-on, always.** A fixture has no facing and is never mirrored: a chest seen from
the side could not show how wide the bank behind it is, which is the whole of what these
three sizes are for.

## The source

`chest.aseprite` is the working file the three were exported from.
