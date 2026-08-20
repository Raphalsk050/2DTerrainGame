# Chest art

Drop four PNGs in this folder and set one field, and the chest is drawn from them
everywhere — standing alone, joined into a bank, and opening.

## The four files

| file | when it is drawn |
|---|---|
| `alone.png` | one chest on its own |
| `left.png` | the left-hand end of a joined run |
| `middle.png` | any unit between the two ends of a run of three |
| `right.png` | the right-hand end of a joined run |

Only `alone.png` is required. The other three fall back to it, so a bank of three
drawn before the ends exist is three identical chests — legible, and what most games
shipped for years.

## The frame

Each file is a **horizontal strip of frames**, laid out left to right, every frame the
same width. That width is `fixture::Def::artWide`, which is `config::kBuildCell` — **18
world pixels**. The strip's height is one frame's height and is read from the file, so
a chest may be drawn taller than its cell: it is anchored **bottom-centre** of the cell
it stands in, so anything past 18 px tall sticks up out of the cell without changing
what the cell means.

`sheet::Load` refuses a strip whose width is not a whole number of frames rather than
drawing a smeared one, so 4 frames means a 72 px wide file exactly.

## The animation

**Frame 0 is shut and the last frame is wide open.** The lid is a play-head between
them, driven by whether this chest's bank is the one the player has open, over
`fixture::Def::opens` seconds. A single-frame strip is therefore a chest that never
appears to open, and it is still correct.

The lid runs on the frame clock rather than the world's, because a panel being open
stops the world — see `Fixtures::Animate`.

## Facing

**Front-on, always.** A fixture has no facing and is never mirrored: a chest seen from
the side could not show which of its neighbours it had joined with, which is the whole
of what a bank has to say for itself. Draw the front of the box, and let `left`,
`middle` and `right` differ only in how their edges meet.

## Switching it on

One field, in `src/entity/fixture.h`, on the chest's row:

```cpp
.art = "chest",
```

Nothing else in the project learns that the chest has a picture. Until it is set the
row's hand-drawn `picture` is used, which is why there is no warning today.
