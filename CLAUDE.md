# CppGame — working notes

A 2D terrain game on raylib. World generation is documented in `planning.md`;
this file is about building it, measuring it, and the rules the performance work
rests on.

---

## 1. Building

**Ninja + GCC, not MSVC.** The project targets `cxx_std_26`
(`CMakeLists.txt:83`). MSVC 19.38 (VS 2022 17.8) does not know that feature and
the configure step fails outright — `the compiler feature "cxx_std_26" is not
known to CXX compiler "MSVC"`. GCC 16.1 (MinGW-w64 UCRT) does. The build scripts
pin the generator for this reason; a newer MSVC may well work, but it has not
been tried here.

```bash
./build.sh          # macOS / Linux
```

```bash
.\build.bat
```

`build.bat` forces `-G Ninja` with `gcc`/`g++`, and reconfigures automatically if
`build/` was left behind by another generator — it clears the caches under
`build/` and `build/_deps/*-subbuild` but keeps `_deps/*-src`, so nothing is
re-cloned. Output is `build/CppGame.exe` (single-config, so no `Release/`
subdirectory). Arguments pass through: `.\build.bat -j 8`.

**Designated initialisers must be in declaration order.** Clang only warns;
GCC makes it an error. If a build fails on macOS-authored code, this is usually
why.

---

## 2. The bar: changes must be byte-identical

The performance work was done under one rule, and it should stay: **a change
made for speed must not change a single pixel or a single number.** This is
checkable, and checking it is cheap.

Build a reference binary from before the change — stash only the files you
touched, so an unrelated fix in the tree does not go with them:

```bash
git stash push src/world.cpp src/world.h src/light.cpp && ./build.sh && cp build/CppGame /tmp/ref && git stash pop && ./build.sh
```

Then compare the three reports that cover the generator, the light solve and the
whole render path (`build/CppGame.exe` on Windows):

```bash
diff <(/tmp/ref --sun 0 0) <(./build/CppGame --sun 0 0)
```

```bash
diff <(/tmp/ref --column 240 100 400) <(./build/CppGame --column 240 100 400)
```

```bash
/tmp/ref --probe 0 -100 640 400 /tmp/a.png && ./build/CppGame --probe 0 -100 640 400 /tmp/b.png && cmp /tmp/a.png /tmp/b.png
```

`--probe` is the important one: it renders through the real draw path, including
the chunk texture cache, so a cache that drifts shows up as a different PNG.

`--sodcheck [frames]` walks two identical worlds along the same path — one
remembering its grass band between frames, one made to work the whole band out
again with `World::ForgetGrass()` — and reports any column they disagree about.
Both see the same chunks come and go, which is what makes it a test of the
memory and not of chunk residency.

**`--column` has one unstable number in it, and it is not yours.** The `coal`
column moves by a thousandth or two between runs of the *same binary* — measured
at −0.652, −0.652, −0.653 and −0.651 over four consecutive runs with nothing
rebuilt in between. Every other material is stable, and `--sun` is stable. So a
`--column` diff that shows coal alone having moved is noise, and a change should
not be blamed for it; run it three or four times before believing it. Everything
else in the report still holds to the bar.

The cause is not yet found. `terrain::Quantile` and `World::CalibrateSpawn` are
both sequential, so it is not a race between workers; the likeliest remaining
candidate is that coal, being the commonest ore, is the one whose sample count in
`CalibrateSpawn` sits at the `kMinPerAxis` floor, where one sample entering or
leaving `values` moves the quantile. Worth fixing — a world that is not
reproducible cannot be held to a bar at all.

---

## 3. Measuring

`src/profile.h` is a nesting zone profiler. `PROFILE_ZONE("name")` opens a zone
for the enclosing scope; the report gives calls per frame, inclusive ms, **self**
ms (inclusive minus what nested zones took) and share of frame. It is off unless
`profile::Begin()` has been called, and it is not thread safe — put zones around
whole phases, never inside a `pool::For` body.

| mode | what it measures |
|---|---|
| `--profile [frames] [still]` | the real loop, draw included. Full screen, flying |
| `--frame x y [frames]` | the simulation phases only, headless, at a fixed place |
| `--sun x y` | the solved light as numbers |
| `--build [cells]` | sets a run of build cells and digs it back out; checks the exchange is exact |
| `--wind`, `--caves`, `--ore`, `--covers`, `--woods`, `--settle`, `--column`, `--tones` | generator reports |

`--woods x0 x1` is the companion to `--covers`: it walks the scatter's own cells
and reports, per species, how many plants grow and what ground each one is
standing on. It exists because a placement rule is very easy to author into a
wood that is never there or a desert quietly full of oaks, and neither errors.
The line to read is the sand column — it has to be zero for every species but the
scrub.

**Always measure full screen with the player moving.** `--profile` forces both on
its own; pass `still` only when deliberately measuring the cached case. This is
not a detail:

- Nearly everything in the frame is priced by the **area of the view** — the
  light solves a region around it, the water steps one, the ground is drawn over
  it. A 1000×600 window is under a third of a 1920×1080 screen.
- Standing still is the case every cache in here is at its best. Nothing streams
  in, nothing is invalidated.

The same build measured **25 ms** windowed and still, and **131 ms** full screen
and flying. Only the second number is the game.

---

## 4. Where the frame goes

Full screen, flying, on a 16-core desktop, after the work below:

| zone | ms | note |
|---|---|---|
| light solve | 13.1 | 55% of the frame; already parallel |
| `DrawScene` itself | 2.9 | sky, clouds, rain, stars, player |
| `lights.Update` | 1.7 | texture upload |
| `world.Update` | 1.5 | streaming and the grass band |
| `StepWater` | 1.2 | |
| `DrawHud` | 0.7 | |
| `PaintChunks` | 0.6 | |
| `DrawTerrain` | 0.6 | a blit |
| `DrawTufts` | 0.5 | |
| `DrawMist` | 0.5 | overcast only; nothing on a clear day |
| **frame** | **24.1** | 42 fps, from 131 ms / 8 fps |

Since measured, `kPixelSize` went from 5 to 3 so that a built block is drawn the
size of the cell it was placed in — see §10.4. That is the one deliberate
regression in this table: `PaintChunks` 0.75 → 1.41 ms and the frame 24.0 → 26.2,
which is 42 fps to 38. Measure over 300 frames rather than 200; a shorter run
caught the streaming still settling and reported 36 ms, which is not the frame.

The light was 9.5 ms and is now 13.0. The difference is one more cascade, and it
was not bought for looks: at four, light was gathered over **510 world pixels**
— cascade zero's interval is the six-pixel lattice step, and the stack reaches
that times (4⁴−1)/3. A screen at the framing this game is built against is
nineteen hundred across, so a lamp anywhere but the near quarter of the view
contributed nothing at all to a player standing in front of it, and did so with
a hard edge. Five reaches 2046 and covers a full screen corner to corner. Half
of the extra is `march 4` at 2.0 ms; the rest is the merge under it.

What is left is still the light. Porting the cascades to fragment shaders is the
next real step; the two things that make it awkward are that `spread` is a
distance transform (on the GPU it becomes jump flooding, which is an
*approximation* and would be the first thing to leave the byte-identical bar) and
that `World::LightLevelAt` is read on the CPU by plant growth (`grove.cpp`) and
the HUD, which would need a readback.

---

## 5. Rules the performance work depends on

Each of these is load-bearing. Breaking one will not fail a build or a test — it
will quietly make the frame several times slower, or introduce a race.

### 5.1 Never create a thread per pass

`src/pool.h` holds a persistent pool. Use `pool::For(count, body, least, block)`.

This is not a micro-optimisation. The light solver used to create a
`std::thread` per worker per pass — seven passes a frame at sixteen workers is
**105 threads created and joined every frame**. Thread creation is dear on
Windows and the cost *scales with the core count*, so the same solve ran slower
on a sixteen-core desktop than on an eight-core laptop. That was the original
report of "the game runs worse on the better machine".

`least` and `block` matter in opposite directions and the defaults only suit one
kind of pass:

- Many cheap items (light probes, medium cells): large blocks, so two workers
  never write into one cache line. Defaults are for this.
- Few heavy items (chunk generation, `pool::For(n, body, 2, 1)`): no floor under
  the count, one item per block.
- Unevenly spread items (the grass band, where nearly every column is cached and
  free and the handful that just scrolled in are all at one end): **small
  blocks**, or one worker takes all the expensive ones. Getting this wrong cost
  a 5× difference.

`body` must write only to what belongs to its own index. There are no mutexes.

### 5.2 What is parallel, and what makes it safe

Parallel: the light march and merge, `spread`, `gather`, the pyramid, the light
medium fill, the water read, chunk generation, the grass band, the light's column
pass, the grass render fields.

Two pieces of shared mutable state had to be dealt with for any of that:

- **`World::Find` remembers the last chunk it answered with.** It is
  `static thread_local`, keyed by `(world, chunkAge_, cx, cy)`. `chunkAge_` is
  bumped whenever a chunk is *released* — insertion cannot invalidate it, since
  `unordered_map` keeps its elements where they are across a rehash. Misses are
  never remembered.
- **`World::Skyline` memoises into a hash map.** A parallel pass must warm it
  first with `WarmSkyline(first, last)` and then set `skylineWritable_ = false`,
  which makes `Skyline` compute without inserting. Two places do this: `ReadSod`
  and `StepLight`'s column pass. Set it back to `true` afterwards.

Raylib is single threaded. Anything that submits a draw call stays on the main
thread — that is why the grass render fields are built across the cores first and
drawn after.

### 5.3 Resolve a lattice position once, not once per material

`World::Vertex` / `World::Resolve` / `ValueAt(const Vertex&, Element)`.

Every field of a chunk shares its origin and spacing, so the snap, the chunk
division and the map lookup are the same for all of them. The passes that read
every material at every vertex of a region — the light's medium and the water's
read-back — were paying for all three ten times over at each vertex. Between them
that was more than a third of the frame.

### 5.4 The caches, and what invalidates them

| cache | holds | dropped by |
|---|---|---|
| `World::sodColumns_` | the grass band's surface per absolute column | `ForgetSod`, from `WriteVertex` on a `blocksBodies` material and from `Settle` |
| `Chunk::silhouettes` | the ground's outline at each precedence, plus its filled bounds | `WriteVertex` on an `occupies` material |
| `World::painted_` | each chunk's ground rasterised into a texture | `DropPainted`, same triggers plus eviction and release |
| `World::settled_` | what the liquid held before a step | rebuilt each step |
| `World::skyline_` | the height of the ground per lattice column | `Reset` only |

If you add a path that writes into a chunk's fields **outside `WriteVertex`**,
you must invalidate the first three yourself. `WriteVertex` is the single choke
point today and the invalidation lives there.

`Chunk::silhouettes` also carries the bounds of the cells that have anything in
them. Most ranks describe nothing at all in a given chunk — a hillside holds no
ore — and skipping those outright is worth more than drawing them faster.

### 5.5 The terrain texture cache is exact, and here is why

`World::PaintChunks` rasterises each chunk's ground into a `RenderTexture2D`
once; `DrawTerrain` blits them. It is bit-identical to the rasterisation it
replaces, and it stays that way only while all four of these hold:

1. **One texel per world unit.** At that scale a screen pixel takes the texel
   whose square it falls in, which is exactly the square the rectangle would have
   covered it with. Any other scale is a resampling.
2. **`TEXTURE_FILTER_POINT`.** The equivalence is nearest-texel; bilinear breaks
   it.
3. **Every colour the ground is drawn in is opaque.** All tones in `element.h`
   are alpha 255 and `config::kDrawContours` is off, so `Outline()` returns
   `BLANK` and no outline is drawn. The texture's alpha is therefore exactly
   "there is ground at this texel", and compositing it replaces the destination
   or leaves it. Introduce a translucent terrain colour and the double blend is
   no longer the single one.
4. **The margin.** Squares are five units and a chunk is a hundred and
   ninety-two, so they do not divide and the square straddling a border is drawn
   by whichever chunk its middle falls in. `kPaintedMargin` gives it somewhere to
   land. Neighbours overlap there and draw over each other harmlessly, because
   the squares are disjoint.

`PaintChunks` opens a render target, so it must run **outside** `BeginDrawing` —
it is called from the main loop beside `liquids.Capture` and `backdrop.Capture`.

The grass is deliberately *not* in the texture: its ramp is a function of the
season and the wind, so it changes every frame.

### 5.6 The water step is a feedback loop

`StepWater` runs fixed steps from the accumulated `dt`. A slow frame means a
larger `dt` means more steps means a slower frame. At 8 fps it was running
**7.87 times per frame** and was alone 47% of it. Anything that makes the frame
slow makes the water worse, and vice versa — so when the frame time moves a lot,
check `StepWater`'s calls per frame before believing the rest of the report.

The write-back only writes vertices the step actually moved (`settled_`). A wet
vertex is always written even when unchanged, because the write is also what
tells its chunk it is holding liquid and the flag was cleared at the top of the
step.

### 5.7 A pickup is thirty-six squares, so pickups have to gather

`Drops` merges settled pickups of one kind into single stacks every frame — see
`Drops::Merge`. It is what the player expects of a pile of wood, and it is also
the only thing standing between a felled wood and a frame spent drawing it: a
pickup is a six-by-six picture, so up to thirty-six `DrawRectangleV` calls, and
the pool holds two hundred and fifty six of them. Unmerged, a hillside dug out
with a full bag is nine thousand quads a frame for a few dozen blocks of stone.

Nothing in that pool expires. A block the player dug and could not carry is a
block they earned, so there is no lifetime and a pool with no free slot pours the
overflow into a stack of its own kind already lying there (`Drops::Spill`). The
one state that can still lose something is two hundred and fifty six *full*
stacks of other materials in one view, which is sixteen thousand items on the
ground. If you add a caller, do not add one that can lose a stack more easily
than that.

### 5.8 Split parallel work by cell, not by column

The light cascades all hold the same number of samples, but the coarsest lays
them out over a quarter as many columns each way. Splitting by column left the
top cascade with fewer columns than there were workers, so it ran on one core —
and it is the cascade with the longest rays. It was the single most expensive
pass in the frame and the one pass not being shared.

---

## 6. Things that turned out not to matter

Recorded so they are not re-investigated:

- **The soil painter's noise.** Two octaves of Perlin per texel sounds
  expensive; measured with a compile-time switch it was 1.6 ms of a 6.1 ms pass.
  Caching it was not worth the machinery. (The texture cache made the question
  moot.)
- **Carrying `SampleAt` along a row** in `marching_squares::DrawPainted`, to turn
  five field reads per square into three. It measured as *slower* until the
  sampling was made conditional, and then made little difference — the pass was
  bound by draw submission, not by sampling. The carry is still in place and is
  correct; do not expect much from that direction.
- **Measuring with `getenv` in an inner loop.** It prevented inlining and made
  the pass six times slower, which invalidated the whole A/B. Use a compile-time
  switch for this kind of test.

---

## 7. Files this work added

- `src/pool.h` — the persistent worker pool. See §5.1.
- `src/profile.h` — the zone profiler. See §3.
- `build.bat` — the Windows build, which did not exist as a batch file before
  (it held a copy of `build.sh`, so `cmd` read it line by line, rejected
  `cmake --build build "$@"`, and printed `pronto` having built nothing).
- `CLAUDE.md` — this file.

`--profile`, `--sodcheck` and `World::ForgetGrass()` were added to `main.cpp` and
`world.h` in the style of the other report modes. `--sodcheck` exists only to
test the grass band cache; if that cache ever goes, it goes with it.

---

## 8. A biome is five tables agreeing

There is no biome table and there does not need to be one. What there is instead
is one pair of climate fields — `terrain::ClimateSettings`, temperature and
humidity, one feature every dozen screens — and five separate tables that each
place their own thing against it with the same `ElementClimate` bell:

| table | what it places |
|---|---|
| `kElements` covers | which of soil, sand and snow lies on the rock |
| `flora::kSpecies` climate | how thick each species' wood is, and its treeline |
| `sod::kCovers` | what colour the grass is, and how much of the ground it holds |
| `weather::Settings::drought` | where the rain does not fall |
| `weather::Settings::snow` | where it comes down frozen instead |
| `flora::SpeciesGround` | which of the covers a species will root in |
| `ElementSpawn::crest` | how high the ground must stand to hold a cover |

Two of them are the exceptions the uniform weather cannot express on its own, and
they are opposite kinds: the drought says a shower does not arrive at all, the
snowfall says it arrives as something else. Both are rolled per drop against the
column, so the edge of a cold region is a shower of rain and snow at once rather
than a line across the sky — and both read the same temperature field that laid
the cover on the ground, so a snowfield is always a place where it snows.

Neither may use `ClimateRamp` with its edges reversed. It guards its span with
`max(edge1 - edge0, 1e-3)`, so a descending pair divides by a thousandth and
clamps to zero everywhere; that returned no snow in the entire world and nothing
said why. Use `1 - SmoothStep(cold, warm, t)`.

A desert is what happens where the rest agree, and the last row is the only
one that is not a share. That distinction is load-bearing and it is what a
"trees in the desert" bug is made of: a bell is a *tendency*, so an oak thins out
as the country dries and thinning out still leaves oaks — and
`flora::Settings::supportFloor`, which deliberately keeps some of a wood's
thickness wherever the climate suits nobody, put a half-thickness wood on open
sand. No tuning of any bell fixes that without emptying the rest of the map. The
ground rule is a gate, it runs before the species roll and before the support
floor, and it is checked twice: once at the cell centre because the species has
to be known before the position is, and once under the trunk because the jitter
can carry a plant half a cell.

`SurfaceCoverAt(worldX, terrain)` in `element.h` is the one answer to "what is
the ground made of here". It reads the generator's covers and not the world, so
a hole somebody dug cannot rearrange a wood, cannot take the snow off a crown,
and cannot make it rain on a desert. The hand is the one place that asks the
world instead — a player who carried soil into a desert and laid a bed of it has
made ground a tree will root in, and the noise underneath knows nothing about it.

Check it with `--woods`, never by eye.

---

## 9. The mountains, and why snow needed a height

`SurfaceSettings` grew a layer: `range` says where a range is, `ridge` is the
crest itself as a folded field, and between them the ground reaches 573 px above
the level where it used to reach 158. Three numbers in it are load-bearing and
none of them is taste:

- **`ridgeSharp` under one.** `1 - |signed|` is a triangle by construction, and an
  exponent over one sharpens it: at 2.2 every summit came to a point one lattice
  column wide, which is nowhere to stand and a climb that ends on a spike. Under
  one the mid-range is lifted instead, so shoulders broaden and the summit gets a
  top. It also halved the worst step between neighbouring columns.
- **`shelfStep` under the jump.** The world's terrace is 24 px, which on a
  mountain's slope is a ledge twenty pixels wide — a smooth ramp with a texture on
  it. A 48 px shelf cut into the crest alone is a run of flat ground broad enough
  to walk along and to build on, and 48 is still two thirds of the 72 the
  character jumps.
- **`ridgeAmplitude` under the cloud deck.** The deck hangs from y = -640 to
  -320 and drops another hundred in a storm. A peak past its underside is
  standing *inside* the cloud, which the rain answers correctly by having nowhere
  left to fall from — and which reads as a summit that never gets any weather.

`--surface` is the check: it reports the mean and worst step between neighbouring
columns and the share of them that cross a texel. The whole generator reads the
surface one column at a time, so a crest that climbs faster than the lattice can
describe it is a cliff to the light, the grass and the trees alike. Before the
crest was broadened it was 29.5 px worst and 4.3%; after, 16.0 px and 1.4%, which
is where the world was without mountains at all.

Snow moved from a climate bell to a bell **and** a height, and the fault it fixes
is worth keeping written down. Altitude was supposed to need no term of its own:
the climate cools by `temperatureLapse` per pixel of elevation, so a material
wanting the tops only had to ask for cold. That is sound and it does not survive a
world that also has cold *regions* — the temperature field runs its whole range at
sea level, so the coldest lowland reads exactly like a summit and snow lay in flat
country a long way from any mountain. No bell can separate the two, because to a
bell they are the same number. `ElementSpawn::crest` can. The bell still decides
whether a range is cold enough to hold snow at all; the crest decides that it is a
mountain.

Both figures were measured, not argued: `--covers` reports the surface's own range
over a stretch of world, and with the ranges switched off it runs -14 to 310. The
snow line sits at -60, which ordinary ground never reaches.

---

## 10. Building is a grid, and the grid decided what a block is worth

A material now says how the right hand puts it down — `Laying::Brush` for the
ground itself, `Laying::Cell` for what is built out of it. That is one field in
`element.h` and it is read in one place, `Editor::Update`; there is no build mode
to switch on.

**The mode is derived, not toggled.** `Editor::Building()` is true because a
building material is in the selected slot, and false because the slot ran out.
The player's rule — "stay in build mode until the stack is gone" — needs no state
to implement: a spent stack is a slot that no longer holds a building material.
It is the same argument `editor.h` already makes about the two hands, that what
the right hand does depends on what is in it.

**A cell is 18 px because the lattice is 6.** `config::kBuildCell` is three
lattice steps, so every cell is the same 3×3 vertices wherever it falls. A size
the lattice does not divide — sixteen, which is what a block used to be — would
take two vertices across in one place and three in the next, and the same
material laid twice would come out two different shapes.

**And `kBlockSide` moved from 16 to 18 to follow it.** That is arithmetic, not
taste. `kVerticesPerBlock` is an area, so at sixteen it was 7.11 and a cell of
nine vertices cost 1.27 blocks — a player would set four pieces and be charged
five, and any rounding in their favour would be a way of making planks out of
nothing. At eighteen it is exactly 9: one cell, one block, one click, one item,
and digging returns precisely what was spent. `stack.h` carries a `static_assert`
tying the two together, and `--build` checks the whole exchange end to end.

`Editor::owed_` is still needed and still correct — the brush is a circle over a
lattice and still crosses part of a block far more often than a whole one. What
no longer needs it is a cell.

**`ApplyBrush` became `ApplyStroke` and took a shape.** Everything a stroke has
to get right — what it charges for, what it hands back, what it tells the grass,
the body it refuses to bury — is the same for a square as for a circle, and the
list is subtle enough that a second copy would have drifted. The shape is a
`World::Reach`, an inclusive range of vertex indices plus an optional circle.

The range is counted in vertices rather than derived from the cell's rectangle,
and that is load-bearing: `LatticeRange` over a cell's bounds includes the vertex
on its far edge, which belongs to the cell next door. Two neighbours would share a
column, and clearing one would take a slice out of the other.

### 10.1 A new material's threshold reaches into the generator

The trap this work found, and the reason to keep every row at the table's own
threshold unless there is a reason not to.

`World::ExclusionHeadroom` holds each occupying material under everything that
outranks it by `def.threshold + (other.threshold - claim) * kClampGain`. The term
is in the **other's** threshold — so a row added at a new threshold changes the
headroom over every older material beneath it, and a material placed only by hand
still takes part, because `occupies` is what puts it in the contest and not
`generator`. Planks and cobble outrank the whole table, so they sit above every
ore in it.

The clamp does not bite at the depths an ore actually sits at, so nothing was
found to have moved. Matching the table is what means it never has to be checked
again.

There is a second reason, and it is the one that shows: two materials crossing at
different values part company along their shared edge. A plank set into a hillside
at a threshold of its own would either sink into the rock or stand a fraction of a
cell off it. Matching is what makes a built wall continuous with the world it is
built into. The cost is six tenths of a pixel on the block — at one half a cell
would measure exactly its eighteen — and nothing can see it.

### 10.2 A cell is its vertices, and they sit half a step inside it

The bug this shipped with, because it is the kind that is invisible in the code
and obvious the moment anybody builds anything.

A cell owns three vertices across — for cell `cx` those stand at `18cx`, `18cx+6`
and `18cx+12`. It is tempting to call the cell `18cx` to `18cx+18` and be done.
That rectangle is the wrong one. A vertex owns the square **centred** on it: it is
the square `VertexMeets` and `OverlapsSolid` test against, and it is where the
contour crosses, half a step out from the last filled vertex. So the cell actually
covers `18cx-3` to `18cx+15`.

The preview was ruled on the first rectangle and the block landed on the second,
so every piece appeared three pixels up and to the left of the outline that had
promised it. `World::CellBounds` and `World::ToCell` both carry the offset now —
`kCellOffset`, half a lattice step — and everything that draws or tests a cell
goes through them rather than multiplying the side out for itself.

**`--build` checks it**, and checks it as a round trip: every point inside a
cell's bounds has to name that cell, and the vertices the cell writes have to lie
within them. Setting `kCellOffset` back to zero fails the second of those
immediately, which is what a test for this is worth having.

The exchange check alone does *not* catch it — 24 cells still cost 24 blocks and
still give back 24 when the rectangle is three pixels out. An economy that
balances says nothing about where the material landed.

### 10.3 The spade squares up over anything that was built

A round brush over a square grid leaves the corners of every cell it passes, so a
player rubbing a piece back off a wall gets most of a piece removed and a rind of
it left standing. Two things make the left hand work in cells instead:

- **The build mode**: while a building material is in hand, the whole hand is
  square, both buttons.
- **Anything already built**: `Editor::Built` asks the middle of the cell whether
  what is there is a `Laying::Cell` material. A placed block went in as a unit and
  there is no sense in which it comes out as five sevenths of one.

Generated ground is untouched by this — a hillside is not made of units and the
brush is the right tool for it. The cursor follows the same rule, drawing the cell
outline instead of the ring wherever the spade is about to work squarely, since a
ring there would be describing a stroke that is not going to happen.

### 10.4 kPixelSize has to divide the cell *and* the half-step

The second bug the wall showed, and the one that cost a frame.

`marching_squares::DrawPainted` anchors its squares to the **world** — multiples
of `kPixelSize` — and gives each lattice cell the squares whose centres fall
inside it. So a run of world that is not a whole number of squares is rasterised
into a different number of them depending on where it falls.

On terrain that is invisible: no two stretches of a hillside are meant to look
alike. On a wall of blocks it is the whole complaint. At `kPixelSize = 5` an
18 px cell came out **three squares wide in one column and four in the next** —
15 px against 20 px — which reads as bites taken out of the wall.

Counting squares is not the test, and this is the trap worth remembering. A size
can give every cell the same *number* of squares and still start each run in the
wrong place, which draws the block beside the square it was promised. The run has
to begin and end on the cell's own edges.

A block's edges land at **three plus multiples of six**: the contour crosses half
a lattice step out from the last filled vertex, and vertices are at multiples of
six. So the square has to divide `kCellOffset`, not just `kBuildCell`. Measured
with `--build`:

| kPixelSize | squares across an 18 px cell | edges out by |
|---|---|---|
| 5 | 3–4 | 2.0 px |
| 6 | 3 | 3.0 px |
| 2 | 9 | 1.0 px |
| **3** | **6** | **0.0 px** |

Only 3 works, and one is the only other divisor of three — at twenty-five times
the squares. Note that a bigger cell does not rescue five: 30 px is a multiple of
both 5 and 6 and still misses, because the offset is three either way.

The old comment on `kPixelSize` asked for "a divisor of kResolution" and then gave
five, which is not one. It had simply never been checked, because until there were
blocks nothing in the world was supposed to be the same size twice.

### 10.5 A cell is all or nothing, including against the body

`ApplyStroke` skips vertices inside `keepClear` — the player's body — and lays the
rest. That is deliberate and right for a brush: a floor laid around one's own feet
is what the player aiming down at them is asking for.

It is wrong for a cell. The skipped vertices come out as a bite taken out of the
block, the block is charged for whole, and nothing on screen says why it is the
shape it is. `World::PlaceCell` therefore refuses a cell the body is standing in
rather than delivering a broken one, and `World::CellClear` is public so the hand
can ask a frame ahead and turn the square red before it is pressed.

The two refusals are kept apart — `founded_` and `roomy_` in `Editor` — because
they ask the player for opposite things: one to build from something that holds,
the other to get out of the way.
