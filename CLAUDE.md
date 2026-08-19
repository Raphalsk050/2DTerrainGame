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

`kPixelSize` went from 5 to 3 so that a built block is drawn the size of the cell
it was placed in — see §10.4. That costs `PaintChunks` 0.75 → 1.44 ms and the
frame 42 fps → 38, and it is the price of a wall that is a wall. Terrain at 5 with
blocks at 1 measured 41 fps and was tried; it is not available, for the reason
§12.1 gives.

**Two things about measuring it**, both learned the hard way here:

- **300 frames, not 200.** A shorter run catches the streaming still settling and
  reported 36 ms, which is not the frame.
- **Watch a zone you did not touch.** Repeated runs during this work drifted from
  24.2 ms to 31.6 with `spread` and `lights.Update` climbing 20% — in a change
  that never went near the light solver. That is the machine heating up under
  back-to-back builds, not the diff. If an untouched zone moved, none of the
  numbers mean anything yet; let it cool and measure again.

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

### 10.5b Nothing is placed over anything

`World::CellVacant`, checked by `PlaceCell`. A cell already holding an occupying
material refuses a placement instead of replacing what is there.

It used to replace, and the reasoning was sound in isolation: two occupying materials
cannot share a vertex, so setting one into a cell has to clear whatever was there,
and clearing it *and handing it back* is more generous than destroying it. What that
misses is the economy. Once a block costs time to break (§13), a seam of diamond is
fifteen seconds a cell — and right-clicking a cobblestone into it took the diamond
out instantly and gave it to the player. The cheapest way to mine anything was to
build over it, and every figure in the hardness table was a suggestion.

Two exceptions, and they are the point rather than edge cases:

- **A wall** contests no vertex and exists to stand *behind* a block (§11.2).
  Refusing one where a block stands would make a wall impossible to put up anywhere
  it is actually wanted. `--build` checks this end to end and would fail if the guard
  were written against every material instead of against `occupies`.
- **A liquid** is poured into a space rather than pressed into one, and `ApplyStroke`
  already declines the vertices a solid fills.

The refusal is in the world and not only in the hand, so that a caller written later
cannot quietly get the old behaviour back — the same argument that puts the body's
refusal there (§10.5). The hand asks a frame early as well, so the square goes red
before the button is pressed, and it is `Editor::vacant_`: a third refusal kept apart
from the other two because it asks the player for a third thing, which is to clear
the space first.

### 10.6 The brush became square, and then it became the only shape

`World::Place` and `World::Excavate` — the circular brush of an arbitrary radius
— are gone. Everything the hand writes is now a whole cell, and a wide stroke is
a block of cells the caller walks.

That is not tidiness, it is the same bug one more time. A round tool over a
square grid can only ever leave the corners of the cells it crosses, so digging
beside a wall left rinds of it standing and the player could not tell why. The
brush size is now counted in **cells** (`Editor::span_`, one to eight) because
there is nothing else left for it to be measured in: a size between two cells
describes nothing that can happen.

`Editor::Lay` and `Editor::Set` collapsed into `Editor::Spend` when they did.
Laying ground and building stopped being different operations — both write whole
cells and both spend one block per cell newly filled. What still differs is the
rule about *where*, and that is asked once, of the cell under the cursor.

`Editor::owed_` survives, and it is worth knowing why given that a placed cell is
exactly one block: digging a cell out of a *hillside* frees however many of its
nine vertices the ground happened to fill, and the surface crosses cells at every
angle. The fraction is real on the way in and never on the way out.

---

## 11. The four building items, and the three mechanics under them

A plank, cobble, a wall and a torch. They look like one feature and they are
three, which is the whole reason they are worth writing down together.

| item | where it lives | why |
|---|---|---|
| wood plank | `kElements`, solid | has a field, collides, is mined — it is a material |
| cobblestone | `kElements`, solid | the same |
| wood wall | `kElements`, `background` | has a field and is mined, but does not collide and is drawn behind |
| torch | `kItems` + `fixture::Fixtures` | an object with a sprite and a light, not an eighteen-pixel square of flame colour |

### 11.1 Digging stone gives cobble, and that is a table row

`ElementDef::yields`, read only through `YieldOf`. `Element::Count` stands for
"itself", since a constexpr row cannot name itself in its own initialiser.

Rock yields cobblestone, which is Minecraft's rule and is what gives the player
anything to build with before there is any crafting. It also means the
generator's own rock is the only rock there is — a player can put cobble back but
never stone — so ground that was never dug goes on looking different from ground
that was.

`Editor::owed_` still banks the debt against the material actually dug, not
against what it yields, so a seam of rock and a wall of cobble never pour their
remainders into one another.

### 11.2 The wall is the only thing that shares a cell, and it cost the journal a field

A wall is `occupies = false` **on purpose**. Two occupying materials can never
share a vertex — that is what makes "what is here?" a question with one answer —
and a wall has to share, because the whole point of it is to stand behind a block
and still be there when the block is dug out.

Three things follow, and each is somewhere different:

- **It is painted in its own pass**, first, inside the same chunk texture. Being
  first in there is being behind the ground *and* behind the character, since all
  the terrain is drawn before the character is — so it costs no second texture, no
  second blit and no change to the order of the frame. Its colour is dark
  **opaquely**, never by alpha, or the cache equivalence in §5.5 breaks.
- **`WriteVertex` drops the chunk's texture for it** even though it clears no
  silhouette. A wall is in no silhouette — it contests nothing — but it is in the
  picture, and without this a wall appears only when something else happens to
  drop the texture.
- **`World::Edit` grew a second field.** `element` and `behind`. One record per
  vertex holding both layers is what keeps a wall from being forgotten when the
  block over it is dug, which is the first case a player will hit.

**Digging order is load-bearing**: `ExcavateCell` takes whatever is in front, and
only where it took nothing does it take the wall. A spade that took both at once
would make a wall impossible to keep; one that took neither would make it
impossible to remove.

`--build` checks all of it, including walking the chunk out of residency and back.
It did not catch either of the two faults below, and it is worth knowing why: it
counts the exchange — blocks in, blocks out — and a wall that covers the hillside
or erases it costs nothing and balances perfectly.

**A wall must never cover what is in front of it.** The most it may do is show
through the gaps. Two separate faults broke that, and they looked like one bug:

- **It was drawn twice, and the second copy was in front.** The last loop in
  `DrawTerrain` draws "anything that neither flows nor claims its space" from its
  own field — which is the wall and nothing else — and it ran *after* the ground
  and the grass, over every chunk, painted or not. So a painted chunk, which
  already holds its walls behind everything, got a second copy laid over the
  finished picture. The copy underneath was right and invisible. That loop is now
  the **first** of the fallbacks and skips any chunk with a picture, which is the
  same shape as every other fallback in the function.
- **The journal cleared a layer the record was not about.** `Edit` held
  `std::optional<Element> element` and read an empty one as *dug out* — but a
  record made by hanging a wall has an empty `element` because it says nothing
  about the front layer at all. `ApplyEdits` cleared it anyway, so the next time
  the chunk was built the generated hillside behind the wall was gone. `Edit` now
  carries `front` and `back`: the flag says whether the record speaks for that
  layer, and the optional says what was left there. **`std::optional` cannot mean
  both "nothing" and "not asked" at once**, which is the general form of this and
  the thing to watch for the next time a record grows a field.

  It survives a session either way, because an edited chunk is pinned — so the
  repro is to walk far enough away to drop it and come back. That is what
  `--probe`'s wall check does by updating the world forty thousand pixels away and
  then back again.

### 11.3 A torch is a third table, and three is the right number

`fixture.h`. An element is a material with a field, a threshold, a rank and a
spawn rule; an item is a name, a picture and a count. A torch is neither: it has
a **place** — one cell, chosen by a player — and a picture of a thing rather than
of a material's face.

It used to be an element and was the wrong shape twice: drawn as a material it
was a square of flame colour, and laid by the brush it went into every vertex the
brush covered, so one click lit a room like a furnace. The old row's own comment
admitted the second.

- **Sparse and permanent**, keyed by cell, on the model of `Grove::remembered_`.
  No function of position produces a torch somebody hung.
- **`Illuminate` re-offers the light every frame**, which is the contract
  `World::AddLight` already had with the lantern, so nothing in the solver knows
  fixtures exist.
- **`Undermine`** drops one whose surface was dug away, beside the grove's own
  pass and for the same reason.
- **Anchors are a bitmask** — floor, wall, roof, behind — so "any surface" is a
  real answer and a workbench asking for floor alone is one field.

`fixture::KindOf` matches an item to a fixture **by name**, the same trick
`flora::SpeciesOf` uses. The alternative is an index written down in two tables
edited at different times, where the first row inserted above either silently
makes torches out of apples.

**A torch in the hand lights the way**, added on top of the base lantern rather
than replacing it. A game that goes black when the last torch is spent has taken
something away; this gives something.

---

## 12. One texel for every material, and why it was tried the other way

`config::kPixelSize` is 3, every material uses it, and `ElementPaint::texel`
exists to say so rather than to be varied.

It was varied. Terrain ran at 5 and built blocks at 1, which measured **24.2 ms
against 26.6** and gave a block eighteen texels across instead of six — room for
authored pixel art at near enough Minecraft's 16×16. Both ends improved, because
the cost of a fine texel is paid over the area drawn in it and the world is nearly
all terrain.

It was also wrong, and the fault is worth keeping written down because the field
still looks like an invitation.

### 12.1 A pale rind down the side of every block

The draw paints each material as the union of itself and everything that outranks
it — `World::Occupancy` — and that union is what stops a gap opening between two
materials whose squares fall differently.

So a coarse material paints the *ground of the fine one above it* in its own big
squares. Those overhang the block by up to a texel; the fine material then covers
only its own exact area, and the overhang is left standing. What it looks like is
a rind along every placed block, brightest where the material underneath is snow.

Two things about it were misleading:

- **It happens with no snow anywhere near.** The union is positive over the block
  whatever the snow field says, so snow's silhouette is painted in snow's colour
  over a block hanging in clear air. It was first seen that way and read as a
  chunk-border artefact.
- **Removing the union trades it for a worse fault.** Filtering the silhouette to
  materials sharing a texel opens **two pixels of open sky** round every block,
  measured. The union is load-bearing.

The way to keep varying texels is to draw each silhouette at the finest texel of
everything it gathers, and that costs the whole chunk wherever anything is built.
Worth it for authored art; not worth it for procedural paint, which is what the
blocks went back to.

### 12.2 Do not key the silhouette on anything the filter does not use

Left behind from the attempt above: `Occupancy` kept a `texel` in its cache key
after the filter that read it was removed. The draw asked for one silhouette and
the liquid clamp asked for another, identical, and every chunk built both.

**7.4 ms a frame** — 34.1 ms against 26.6, or 29 fps against 38 — for two copies
of the same grid. It looked exactly like the machine being warm, and the thing
that separated the two was `spread` at 1.80 while warm and 1.59 once the
duplication was gone. A zone you did not touch is the instrument; see §4.
### 12.3 Where a thing lands still asks the material

`DrawnTop` answers "where was this surface actually *drawn*", and it is what makes
a tree, the grass and the player's feet sit on the ground instead of floating over
the contour. With one texel it could be asked without saying about what. With
several it cannot.

`World::FootingUnder` now reads `OccupantAt` half a step inside the surface and
quantises onto that material's grid. Without it, a plank floor drawn five times
finer than the hillside beside it would still be stood on as though it were
hillside.

The other two callers were checked and deliberately left alone, which is worth
writing down so neither is "fixed" later:

- **`sod.cpp`** — grass only ever grows on soil, and soil is terrain. Nothing laid
  by the cell grows anything, so the fine texels never reach it. This is also the
  hot path the grass band runs on, and it stays a constant.
- **`grove.cpp`** — this is the profile the *scatter* stands trees on, and the
  scatter is deliberately blind to what has been dug or built. A plant the player
  puts down goes through `FootingUnder` instead.

---

## 13. Blocks resist, and the bar is the crack

Digging used to be instantaneous: hold the button, sweep, and material came away as
fast as the cursor moved. Now a block has to be worked at. Four things in it are
decisions rather than tuning.

### 13.1 The table holds Minecraft's hardness, not a time

`ElementDef::hardness` is the wiki's own figure -- stone 1.5, cobble 2, dirt and sand
0.5, planks 2, every ore 3 -- and the seconds are worked out from it. That is the
right way round: a time is a *consequence* of the hardness, of whether what is held
can harvest the block at all, and of the tool's speed.

Minecraft computes damage per tick as `speed / hardness / divisor`, with the divisor
**30** where the tool can harvest and **100** where it cannot. Twenty ticks to the
second makes that `hardness x 1.5` seconds when it can and `hardness x 5` when it
cannot -- `kHarvests` and `kRefuses`. Dirt drops to a fist, so a bare hand gets 1.5
and dirt is three quarters of a second. Stone drops nothing without a pickaxe, so a
bare hand gets 5, and 1.5 x 5 is the seven and a half seconds everybody who has
punched stone remembers.

`BreakSeconds` is the only place any of that happens.

| ours | stands for | hardness | by hand |
|---|---|---|---|
| rock | Stone | 1.5 | 7.5 s |
| soil | Dirt | 0.5 | 0.75 s |
| sand | Sand | 0.5 | 0.75 s |
| snow | Snow Block | 0.2 | 1.0 s |
| wood plank, wood wall | Oak Planks | 2 | 3.0 s |
| cobblestone | Cobblestone | 2 | 10 s |
| coal, copper, iron, gold, diamond, emerald | the ores | 3 | 15 s |
| water | -- | 0 | at a touch |

Our grass is a band drawn on soil rather than a material, so Minecraft's grass block
has nothing here to attach its 0.9 to.

**The scale is not corrected and that is a decision to revisit.** A cell is 18 px and
the character four of them tall, where a Minecraft player is under two blocks -- so a
cell is about half a block on a side and a quarter of one by area, and clearing the
same *volume* of stone costs four times what Minecraft charges. Dividing the whole
table by four is the correction if that is wanted; nothing else would have to move.

### 13.2 What a stroke costs, and why the brush size does not change the rate

Charged per **vertex** actually standing there, over `kVerticesPerBlock` of them to
the cell. A cell buried in a hillside costs the whole figure; one at a ragged contour
edge holding two vertices of nine costs two ninths. So an edge comes away quickly
instead of at the price of solid rock, and the time is the time to clear what is
really there.

It follows that digging runs at the same **vertices per second** whatever
`Editor::span_` is set to. That is the same invariant the economy already keeps -- a
cell costs one block to place and returns one to dig, whatever brush laid it -- and
giving a wide brush a discount would make the span a power setting rather than a
choice.

### 13.2b The tool data is in the table and the tools are not

`ElementDef::tool` says which of `Tool::{Hand, Pick, Shovel, Axe}` a material gives
way to, and `ElementDef::needsTool` whether that tool is *required* to get anything
out of it. The second is the difference between dirt and stone and is what makes the
hand times above come out right.

Nothing can be held that satisfies either yet: `ToolInHand()` returns `Tool::Hand`
and `ToolSpeed` returns 1. Those two are the only lines that change when there is a
pickaxe -- Minecraft's tiers run 2, 4, 6, 8, 12 for wood through netherite, and
`BreakSeconds` already divides by whatever comes back.

`Tool::Hand` in a row means **nothing in particular**, not "bare hands are right for
it".

### 13.2c A tree is its logs

`flora::Settings::toughness` used to be "hits a mature tree takes", which is the same
number in the wrong unit: a hit is a property of the swing, so the figure could not be
set from anything or compared with anything. It is now **logs**, and an oak at five is
five logs.

Minecraft's oak log is hardness 2 and drops to a fist, so it costs the harvesting
rate: `flora::kLogSeconds` is 3. Chopping here is a swing rather than a hold, so the
two meet through the cadence -- `kChopBlow` in `main.cpp` is
`kAttackCooldown / kLogSeconds`, one blow being one cadence's share of one log. A
whole oak therefore takes about fifteen seconds by hand, which is what its logs would
take, and is meant to be a reason to want an axe.

An axe will be a `ToolSpeed` and will divide the same seconds the ground's does.
Nothing about the swing changes.

### 13.3 A bar, because there is no block to crack

Minecraft cracks the block's own face. That cannot be done here and the reason is
§5.5: a cell is drawn from a contour it shares with its neighbours, out of a
chunk-wide texture that is cached and blitted. There is no per-cell picture to put a
crack on, and giving each cell one would cost the whole texture cache.

The bar sits over the block being worked, which is where the outline of that block is
already drawn, so the eye is there anyway.

### 13.4 Letting go gives the work back, and says so

`Editor::LetGo` runs the bar back up to full while fading it out, over `kLetGo`.
The two together are the message: the work is gone, the block is exactly as it was,
and stopping cost the player nothing they had.

The bite is thrown away whenever the aim leaves the block it was started against, or
when that block's contents change under it. That is the mechanic and not a
shortcoming of it — what makes breaking something feel like work is that it has to be
worked *at* — and it is why the giving-back has to be legible.

Two clocks are involved and only one is right. Both the bite and the ease run on
`GetFrameTime()` and not on the weather clock, because how long a rock takes to break
is a rule of the game: on the weather clock, F7 would break stone forty times faster.

### 13.5 Ask the world what the cell holds, through the same walk the spade uses

`World::CellHolds` counts per material over exactly the vertices `ExcavateCell` would
take. **It must not be a point sample**, and this shipped as a bug that was worth the
lesson.

It was written to read each cell's middle, on the reasoning that the middle is where
every question about a cell is asked -- which is true of what a cell *is* and false of
whether there is anything in it. At a contour edge the middle is open sky while a
corner still holds ground. That remnant is one vertex, so it draws almost nothing and
reads as empty air; but a body collides with the square around every filled vertex, so
it is solid; and a hand deciding from the middle refuses to dig it. Invisible,
impassable and immovable at once -- which is one bug and not three, and it stranded
the player.

A cell holding only a wall counts as the wall, on `ExcavateCell`'s own order: the layer
behind is taken only where nothing in front was found (§11.2). The background row is
looked up rather than named, so a second wall material needs nothing edited.

A stroke where no cell holds anything does not start a bite at all. The bar must not
appear over open sky.

---

## 14. The menu is a stack, and the world is one of its screens

`src/menu.h` holds one idea and the rest falls out of it. A screen is pushed when
the player goes into it and popped when they come back, so **back is one operation
that works from anywhere** and no screen has to know what is under it. New world is
reached from saves today and will be reached from a world list tomorrow; neither of
them says so anywhere.

`Screen::World` is on that stack too, and it is what makes pausing free:

| stack | what the player sees |
|---|---|
| `{Title}` | a fresh start, no way back, no arrow drawn |
| `{Title, Saves, NewWorld}` | three screens deep, back pops one |
| `{World}` | playing |
| `{World, Title}` | paused — back pops to the game |

So the loop asks one question, `menu.Playing()`, instead of keeping a paused flag
beside the stack that can disagree with it. Everything below that test in the loop
is the game — the simulation, the hand, the light, the draw — and none of it runs
while a screen is in front.

**Escape is read before the panel toggle, against the panel as it stands.** The
toggle clears `packOpen` in the same frame, so the test written after it sees a shut
panel and pauses the game on the very press that closed one.

**Every screen is laid out by a pure function of the window size**, called again in
the draw rather than remembered from the input pass. A layout held between the two
is a button drawn where it can no longer be clicked, which is the editor cursor's
rule one frame further out.

### 14.1 Making a world replaces the one in the object

`World::Rebuild` puts a different country into the same `World`, and it is not a
convenience: every system in the loop holds a reference to that object — the wood,
the fixtures, the editor, the light — so building a second world and swapping it in
is either a rebuild of all of them or a dangling reference nobody finds until a
chunk is asked for. It is `Reset` with the seed allowed to move.

Three things have to happen around it, and leaving any of them out is a world that
is subtly not the one the seed names:

- `terrain::Calibrate` on **main's copy** of the settings as well as on the world's.
  The cutoffs are quantiles of this world's own noise; the copy in `main` is what the
  wood is configured against and what the character's landing height is read from.
- `Grove::Clear`. Its records are keyed on the cell a plant grows in, and a cell
  means something else under a different seed — a record kept across a rebuild is a
  felled tree in a country that never had one.
- `Fixtures::Clear` and `Inventory::Clear`, for the same reason one step down.

### 14.1b Making a world is measured work, and the screen says so

Creating a world took **8.3 seconds** and the window was dead for all of it. All of
it was `World::CalibrateSpawn`: each ore's cutoff is a quantile of its own noise,
so making a world means sampling up to 384 x 384 positions of the *surface* per
ore, and it walked them one at a time on one core.

Two changes, and they are independent:

- **The sampling runs on the pool**, a row per worker, gathered on the main side
  afterwards so nothing is shared while the rows are taken. 8.3 s to **1.0 s**, and
  `--sun` and `--column` are byte-identical across it — the samples land in a
  different order in the list, and a quantile does not care.
- **It is sliced.** `World::BeginRebuild` starts the measurement and
  `World::StepRebuild(budget)` advances it by about a frame's worth, stopping on
  the first row block past the budget. The loop drives it from the menu path, so
  the loading screen keeps drawing and its bar keeps moving. The startup path still
  runs it to the end in one call — there is no frame to keep alive before the
  window is up.

The screen names the seam being measured and says *why* it is slow, because the
honest answer is short and interesting: ore is measured rather than declared, so
the rarity in the table is the rarity you dig for. A bar with no words on it is
indistinguishable from a program that has hung.

**Each stage announces itself and acts on the next frame.** A stage that did its
work first and then set the line would name what the player has already waited
through, and the longest stage would be the one nobody ever saw a line for.

### 14.2 Two modes, three lines apart

`Gamemode` lives in `src/mode.h` — a header of its own, tiny as it is, because the
editor, the inventory and the menu all have to agree on it and none of the three
should have to include either of the others.

Everything about the world is the same in both. What differs is the hand:

- **Breaking** — creative writes a cost of zero into the bite rather than branching
  around it. A bite that takes no time comes away on the frame it started, which is
  the path a liquid already takes and the path the progress bar already knows not to
  draw.
- **Spending** — the slot is not drawn down, so it never empties. That is the whole
  of what "blocks do not run out" means, and it is why no other module has to ask
  about the mode.
- **Yielding** — nothing comes off a block broken in creative. What digging is for in
  that mode is the shape of the hole, and a player clearing a hillside does not want a
  hillside's worth of stone at their feet.

The creative inventory is the **same panel** with the grid replaced by a page of the
palette and a row of tabs over it, not a second panel: the bar, the cursor, the tips
and the layout cannot drift apart between two modes if there is only one of them.
Which page a thing lands on is derived rather than written into the tables — a
material is a block, an item that fixes to a surface is gear, everything else is
nature — so a new row in either table has nothing to remember. A page is the grid's
twenty-seven slots against twenty-three things in the world; if a table outgrows its
page the tail stops being reachable, and the fix is another tab rather than a
scrollbar.

---

## 15. Where the code lives, and why main.cpp is short now

`main.cpp` was **4,719 lines**. It held the game loop, every command the console
takes, every offline report, the whole draw order, the head-up display and the
argument parsing. Nothing in it was wrong; the file was, because a reader looking
for the loop had to walk past fifteen hundred lines of reporting to find it, and
every new probe made the loop harder to see.

It is **1,608 lines** now, and what it holds is the loop and the setup around it.
The rest went to files named after the one thing they do:

| file | what it is | lines |
|---|---|---|
| `probes.{h,cpp}` | the world measured rather than played — every `--` report, and the dispatch that chooses one | 2,300 |
| `commands.{h,cpp}` | what a typed line means | 470 |
| `render.{h,cpp}` | the order the world is drawn in | 260 |
| `hud.{h,cpp}` | what is drawn in the frame's coordinates rather than the world's | 190 |
| `menu.{h,cpp}` | the screens in front of the game, and the stack | 600 |
| `view.h` | what the frame covers, in world units | 40 |
| `mode.h` | which of the two sets of rules a world is played under | 30 |

Three rules came out of doing it, and they are worth keeping:

- **A shared constant belongs with the thing it describes, not with the first file
  that printed it.** `kSeasonNames` went to `flora.h` beside `Season`;
  `kSimulationMargin` went to `config.h`. Both were in main.cpp because that is
  where they were first needed, and both were needed by three files by the end.

- **A probe reports a verdict, so it has to be able to fail a build.** The first
  cut of `probes::Run` returned a bool, which quietly threw away the exit status of
  `--build` and `--sodcheck` — checks that answer *yes or no* and are meant to be
  run in a loop. It returns `std::optional<int>` now: nothing where this run is not
  a probe, and the status otherwise.

- **The bar in section 2 is what makes a move like this safe.** Every step of it —
  four files, one constant at a time — was checked with `--sun`, `--column`,
  `--tones`, `--covers`, `--build` and a `--probe` picture, byte for byte. The one
  step that moved anything moved a *warning line*, and that warning was the missing
  exit status above. A refactor that cannot be checked is a rewrite.

### 15.1 The clouds were eighteen per cent of the frame, and nobody could see it

`DrawLitWorld` had **3.5 ms of self time** — time inside it that no zone accounted
for. Section 4 says a profile with a hole in it is an instrument nobody can read,
and this is what was in the hole: `Sky::DrawClouds`, at **2.96 ms**, the single
most expensive thing the frame drew.

Two changes, both of which leave the picture exactly as it was:

- **The field is filled across the cores.** A few thousand samples of a three-octave
  field, once a frame, on one core: **2.8 ms → 0.14 ms**. Safe by construction — a
  worker writes only the column it was handed, and `ColumnAt` and `MarginAt` are
  pure functions of the settings and the position.

- **The shading walks rows of the world rather than cells of the grid.** It used to
  walk cell by cell and, inside each, the squares whose centres fell in it. Same
  squares, cut into a few hundred pieces — and the cut cost twice: a run of one
  colour ended at every cell border, so the same cloud was submitted as far more
  rectangles than it needed, and the work could not be shared because a worker
  cannot draw. Whole rows can be: the band of every square is worked out across the
  cores into one scratch buffer, and then this thread walks the rows once and draws
  them. **2.07 ms → 0.73 ms.**

  The arithmetic is written to land on the very same floats the cell walk did —
  the interpolation is still measured from the cell's own corner — and it was
  checked rather than argued: both walks were run side by side over **45.7 million
  squares** and disagreed about none of them.

The frame went from **15.9 ms to 13.9 ms** (63 to 72 fps) on `--profile`, which is
the flight over the surface. There is no cloud underground, so there was nothing
there to win.

### 15.2 SIMD, and what it was worth here

The default instruction set a compiler targets on x86-64 is the original one — SSE2,
two float lanes. Both wider baselines were tried on the whole build, with
`-ffp-contract=off` beside them so that FMA could not fuse `a * b + c` and change a
number the reports are held to.

- **`-march=x86-64-v3` (AVX2) crashes.** AVX wants its spills 32-byte aligned and
  the Windows ABI promises 16, so GCC realigns the stack itself — and on MinGW that
  does not survive being entered from a thread Windows created rather than the
  runtime, which is every callback raylib makes. Measured: `--probe` segfaulted on
  five runs out of six at v3, and on none at v2.

- **`-march=x86-64-v2` (SSE4.2) is stable and buys nothing.** 15.87 ms against
  15.77 ms, which is noise. The hot loops are memory-bound or branchy, and v2 is
  the same 128-bit width as the default: what it adds is integer work and blends
  that these loops do not do.

So the flag is not in the build. **What the cores were worth, the threads had
already taken**: the two cloud passes above, and `CalibrateSpawn` in section 14.1b,
are between eight and twenty times faster for having been shared out, against a
best case of two for the vector width. If AVX2 is ever wanted here it has to be
reached for deliberately in one hot loop with an aligned buffer of its own, not
switched on under a build that hands function pointers to the operating system.

---

## 16. Adding a block or an item

The whole design is that this is a **row**, and everything else is derived from it.
Where that has been true, adding a material has never needed a second file opened.
Where it has not, the failure is always the same shape: the new thing works
everywhere except the one place that was written as a list of names.

### 16.1 A material

One row in `kElements[]` in `element.h`, in the exclusion order the `precedence`
field decides. What the row says and what falls out of it:

| the row says | and this follows, with nothing else edited |
|---|---|
| `paint.tone`, `contour` | how it is drawn, at every zoom, and the colour of the dust off it and of its pickup |
| `icon` | the picture in the bar, the panel, the tooltip and on the ground |
| `rules.occupies`, `precedence` | where it sits in the exclusion order, and what the silhouette draws it against |
| `rules.blocksBodies`, `blocksLiquid` | collision, and whether liquid can pass |
| `rules.background` | that it stands *behind* the ground rather than in it (§11.2) |
| `spawn` | where the generator puts it, and — for a vein — the cutoff measured at startup |
| `hardness`, `tool`, `needsTool` | how long it takes to break, and with what (§13.1) |
| `stack`, `laying` | how it is carried, and whether it goes down by the cell or under the brush |
| `light.opacity`, `glow`, `strength` | what it does to the light, both as a shadow and as a source |
| `loose` | what it throws up underfoot |

That last one is the newest, and it is there as a lesson rather than a feature: it
was a `switch` over material names in `scuff.cpp`, with a `default`. The switch was
*defensible* — how much a ground kicks up is a fact about that ground and nothing
else in the row implies it — and it was still wrong, because it put a question about
a material in a file about footsteps, and answered it silently for anything added
later. **A fact about a material goes on the material's row.**

The creative palette, the hotbar, the drop table and the exclusion order all read
the table rather than a list of their own, so a new material appears in all four.
A `static_assert` in `inventory.h` fails the build if the table outgrows one page of
the palette, because that failure would otherwise be silent: the palette would just
stop listing whatever was added last.

### 16.2 An item

One row in `kItems[]` in `item.h`, and the head of that file says why an item is not
an element: an element has a field over the lattice, a threshold, a rank and a
generator, and an apple has none of those.

`placement` is what decides what the right hand does with it — nothing, root it in
the ground, or fix it to a surface — and it is also what sorts it into a tab of the
creative palette. A sapling additionally needs its species in `flora::SpeciesOf`,
and a fixture its kind in `fixture::KindOf`: both are one line, and both are the
seam between two tables that deliberately do not know about each other.

### 16.2b A row names what it needs, and the build checks that it can

Two tables join the item table from outside it, and both used to join it *loosely*:

- **A fixture** found its item by comparing name strings — `TextIsEqual(kKinds[k].name,
  Def(item).name)`. It worked, and it was a trap with a long fuse: the two tables
  were tied together by a word, so renaming either one, in a commit about wording,
  silently broke the link. What a player saw was a torch that could no longer be
  placed, and nothing in either file said the names had to agree. `Def::from` names
  the item now.
- **A species** left its sapling at the first row of the item table to mean "does not
  sow", on the reasoning that the first row is not a sapling. True, and it hid a typo
  perfectly: a species naming *any* item that is not plantable read as a species that
  does not sow, which is a legitimate answer, so nothing could tell the two apart. It
  is a `std::optional<Item>` now — there is a way to say none, and getting it wrong
  is no longer a way of saying it.

Both are backed by a `static_assert` that walks the table at compile time:
`fixture::KindsArePlaceable` and `flora::SaplingsArePlantable`. Both were checked by
breaking a row deliberately and watching the build fail, which is the only way to
know a guard guards anything.

**The rule these three share** — with the palette assert in `inventory.h` and
`ElementDef::loose` above — is that adding a row should either work or fail to
compile. Anything in between is a row that looks added and is not, and every one of
them cost the same day of looking in the wrong place.

### 16.3 What to check after adding one

- `--tones` — that its paint divides between form and texture the way the others do.
- `--build --png` — that a wall of it is drawn on the same grid as everything else.
- `--covers` or `--ore`, if it generates — that it appears at the rate its row claims.
- The palette, in creative: it should be there without anything being told about it.
