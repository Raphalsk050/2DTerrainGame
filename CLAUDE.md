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
| `--dig [cells]` | what a cell of two materials pays out, and how long it takes with each tool (§13.6) |
| `--stuck [holes]` | ground that stops a body and draws nothing (§13.7) |
| `--wind`, `--caves`, `--ore`, `--covers`, `--woods`, `--settle`, `--column`, `--tones` | generator reports |
| `--veins out.png [zoom]` | every inclusion at its own width, drawn and measured (§27.5) |
| `--craft out.png [w] [h]` | the recipe panel in every state it has (§28.7) |
| `--gear out.png [zoom]` | every item as a slot draws it, the wear bar, and the tool in hand (§29.5) |
| `--chest out.png [w] [h]` | the store at every size it joins to, and what a break pays out (§30.11) |
| `--saves` | a world written down, read back, and written again (§31.3) |
| `--menu out.png [w] [h]` | every screen in front of the game, with worlds in the list (§31.9) |

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

### 13.6 A cell takes as long as its most stubborn part, not as long as most of it

Reported from play: *some tools break blocks that are not theirs too quickly.* It was
true, it was every hillside in the world, and the cause is one line that had been right
about a different question.

`Editor::Update` used to charge a cell at **its chief material's rate over all of its
vertices**:

```cpp
takes += BreakSeconds(*chief, kit) * solid / kVerticesPerBlock;
```

`World::ChiefOf` is the right answer to what a cell **pays out**. Nine vertices are one
block and they have to go to somebody, or a cell split five and four banks two fractions
and pays out neither — which is exactly the fault `--dig` was written for, and the reason
the collapse exists at all.

It is the wrong answer to **how long the cell takes**, and the two questions had been
folded together. A cell of five vertices of soil and four of rock is chiefly soil, so it
was charged soil's rate over the whole nine: a spade cleared the rock in it in a sixth of
a second, and `needsTool` — the promise that stone gives up nothing without a pickaxe —
never came into it. Every hillside is that cell all the way along the line where soil
meets rock, so **the cheapest way into a mountain was to aim at its edge with a shovel.**
Measured over four hundred cells of real ground: 465 violations, the worst of them a gold
shovel going through rock 53 times faster than a bare fist manages it.

It is §10.5b's hole one room over — there, building a cobblestone over a seam of diamond
was the cheapest way to mine the diamond. Both are the same shape: a rule that is correct
about one thing quietly answering for another, and the answer being a way round every
figure in the hardness table.

`Editor::WorkFor` is the rule now, and it is **the maximum over the materials present,
each counted over the vertices it actually holds**:

- **The maximum and not the chief**, so a material that needs a tool cannot be carried
  out of a cell by a tool that suits something else in the same square.
- **The maximum and not the sum**, which is the other thing this has been. A sum charges
  for the soil *and* the rock and comes out slower than solid rock, which is a mixed cell
  being harder than either of the things it is mixed from.
- **Each term over its own count**, which is what keeps §13.2 intact: a ragged edge
  holding two vertices of nine still costs two ninths.

Liquid and the wall behind are still *added* rather than maximised, because they are a
second thing standing in the same square rather than part of what the block is made of.

**The payout is untouched.** `ChiefOf` still decides what comes out, and `--dig`'s
original verdict still passes: every full cell pays a whole block, of the material most
of it was.

**`WorkFor` is public and static so `--dig` can measure the game rather than a copy of
it** — §28.7's rule about `Crafting::Draw`. That mattered here: the first version of the
check was written against the *tool* ("a tool that suits nothing in the cell must be no
quicker than a fist") and **passed while the bug was still in**, because a spade suits a
cell of soil and rock perfectly well — the soil is soil. The fault was that it took the
rock too. The check is written against the *material* now:

> A material that needs a tool does not come away without that tool any faster than a
> bare fist manages it.

Which was checked the only way a guard can be — by putting the old rule back, watching
`--dig` fail with 465 cells and a named worst offender, and then putting the fix back.

### 13.7 A degenerate block, and the two rules that made one

Reported from play: *sometimes I break a block and get stuck on something invisible.* The
collider overlay showed a lattice vertex standing on its own in open air. The player's own
words are the specification — a block like that must not exist — and they are right.

**It is one question asked twice with two different rules.**

| | what it tests |
|---|---|
| the draw (`marching_squares::DrawPainted`) | the field **interpolated at a square's centre** is over the threshold |
| a body (`World::IsSolidAt`) | the **nearest lattice vertex** is over the threshold |

Those agree in the middle of a hillside and part company by up to half a lattice step at
every edge, which is invisible because there is drawn ground beside it. At a vertex
standing on its own they part company altogether. The nearest texel centre is a quarter
of a cell away in each direction, so it reads `0.75 × 0.75 = 0.5625` of the vertex's
value, while the body reads the vertex itself. Every material in the table crosses at
0.45, so:

> **a vertex holding anything between 0.45 and 0.80 is a six-pixel square of solid ground
> with not one pixel painted anywhere in it.**

That is the whole of the bug, and the window is enormous — the values at a contour edge
are *by definition* just over the threshold.

**Where they come from is digging, and the generator is innocent.** Measured over forty
doorways cut into real hillsides: **nought** in ground nobody has touched, and **24 after
digging** — better than one every other doorway. The generator's field is smooth and a
lone spike is not a shape noise makes; a dug hole is nothing but edges. The mechanism is
§13.5's cell seen from outside: a cell at the contour edge holds one vertex of its nine,
the player clears the cells around it because those are the ones they can see, and the one
that is left is the thing they walk into.

So this is §13.5's fault one layer down. That section fixed the hand's third of
"invisible, impassable and immovable at once" — `World::CellHolds` counts over the
vertices the spade would take, so such a vertex *can* be dug. This is the two thirds that
remained: the player cannot see there is anything to dig, and walks into it instead.

**The fix is that the world does not contain one.** `World::Undegenerate` runs at the end
of a dig and takes away every vertex left stopping a body without drawing itself. Four
things about it are decisions:

- **Not "make the body collide with the contour".** That is the other repair, and it is
  the wrong one here: it would mean sampling the interpolated field on the texel grid in
  the hottest loop the bodies have — some fifteen times the reads, for every creature,
  every frame — to fix a state that should not exist in the first place. Removing the
  state keeps `IsSolidAt` a single read and makes it *true*.
- **Only on a dig.** Placing writes the material at the top of its range and cannot leave
  a sliver; the one thing it clears is the liquid it displaces, which stops nothing. A
  placed wall is never degenerate: a rim vertex blends with the full one beside it and
  samples 0.75, well over the threshold.
- **A work list, not a fixed margin.** Taking a vertex away lowers what its neighbours'
  squares sample, so it can leave one of *them* drawing nothing. It settles almost at once
  — a vertex inside rock has its own value and seven full neighbours — but "almost at
  once" is not a bound, and a fixed margin leaves the fault one vertex further out. The
  list is bounded by what actually comes away, and a vertex can only come away once.
- **The player is paid for it.** What the sweep clears goes into the stroke's own yield,
  so `Editor::Bank` carries it like anything else. It is ground that was there.

**`World::PaintedAt` is the shared rule**, and `marching_squares::Blend` came out of
`SampleAt` so that both read one interpolation. Two copies of that blend would be two
answers to *where the ground is*, which is exactly what this section is about.

### 13.7b `--stuck`, and why it sweeps three times

```bash
./build/CppGame.exe --stuck [holes]
```

It cuts six-by-six doorways into real hillsides through `World::ExcavateCell` itself —
a probe that wrote the field directly would be measuring a state the game cannot reach —
and counts vertices that `IsSolidAt` calls solid and `Degenerate` calls unpainted, before
and after.

**The three sweeps are three different questions**, and a single number would have hidden
which of them was being answered:

| sweep | asks |
|---|---|
| as the world made it | does the generator make them on its own |
| after digging | does digging make them |
| and back from memory | does taking them away *stay* taken away |

It was worth asking all three. The first is nought, which is what says the whole world does
not need re-generating and every reference picture still holds. The third is §11.2's rule
in another table: the sweep clears vertices the *stroke* was never asked about, so each one
has to be written into the chunk's journal on its own account — an edited chunk is pinned,
so a session never shows the difference, and the repro is to walk forty thousand pixels
away and come back. Taking that one line out reported **43 of them coming back**, which is
what makes the third sweep worth its runtime.

It counts the solid vertices either side of that trip as well as the stranded ones, because
a chunk that comes back with *more* ground than it went away with is the journal forgetting
the sweep — a different fault with the same symptom, and one that a count of stranded
vertices alone reports as a fresh set of them.

It also refuses to pass on having found nothing to dig — §23.1's lesson, and `--dig`'s own
guard. A sweep that met no ground has checked nothing and would go on reporting success
after a change that removed every hillside in it.

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
| `vista.{h,cpp}` | the ranges behind the world, and the distance they buy | 1,030 |
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

One row in `kElements[]` in `world/element.h`, in the exclusion order the
`precedence` field decides. **This is the last central table left** — items and
creatures register themselves now, and §19 is what that means and why. Everything
below about what a row buys still holds. What the row says and what falls out of it:

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
| `paint.vein` | whether it is a body of itself or a scatter through what it displaced (§27) |

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

**Superseded by §22** — an item is its own file now and there is no `kItems[]` to
insert into. What has not changed is why an item is not an element: an element has a field over the lattice, a threshold, a rank and a
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
- `--veins`, if it is an inclusion — that a seam of it is drawn at the share its row
  asks for, and that a seam of it can be seen (§27).
- The palette, in creative: it should be there without anything being told about it.

---

## 17. The horizon is scenery, and it is the world's own tables in another shape

`src/vista.h` and `src/vista.cpp`. Five ranges of mountains standing between the
air and the world, each moving at its own speed as the view goes past it.

It buys the one thing the generator cannot give at any price: **distance**. The
world is a single plane, so everything in it moves with the camera at exactly one
pixel per pixel, and a landscape where nothing is further away than anything else
reads as a cross-section rather than as a place.

Nothing here is generated, stored, collided with, lit, dug or built on. A range is
a heightfield along x evaluated where it is drawn and thrown away, so the module
has no state but its settings and the scratch it rasterises into.

### 17.1 It is drawn in front of the air and behind the world

Which puts it on the **unlit** side of the frame, beside the sky, and that is the
whole of why it costs nothing to keep in step with the day:

- What lights a mountain forty screens off is not the lantern in the player's hand.
- The haze it dissolves into is the very air drawn behind it — `Sky::AirAt`, at the
  texel's own height, snapped to the same ten-pixel band `DrawAtmosphere` uses. So
  an overcast afternoon greys the horizon and a sunset burns it, and neither is
  written down in `vista.h`.
- It takes the day from the same `Daylight::light` the atmosphere is scaled by, so
  dusk falls on the ranges and the sky together. **Over the whole colour and not
  over the haze alone**, which is the shape of the bug this shipped with: the day
  was applied to `air_` when it was filled, so a far row — which is mostly haze —
  went dark on time while a near row, which is barely any, stayed at noon. What that
  looked like at midnight was the foothills glowing on a black hillside, and it read
  as the near rows being wrong rather than as the day being applied in the wrong
  place. The air is now kept at full brightness and the finished texel is scaled,
  which is the order `DrawAtmosphere` does it in.
- The light on a face is `Daylight::sun` — the ranges light from the left in the
  morning and the right in the afternoon, off the same vector the clouds are shaded
  by. Nothing had to be told about the hour.

Being drawn before `LitLayer::Compose` is what puts it behind every cloud, every
hillside and every tree without any of them being asked.

**The stars are the exception, and they had to be told.** They are drawn *after*
the light — see `weather.h` for why — so they are no longer hidden by something
simply because it was drawn later. `Sky::DrawStars` already asks the land and the
cloud; `World::DrawStars` now folds `Range::CrestAt` into the ground profile it
hands over. Written into the profile rather than tested separately, because the
profile is exactly the question "how high is the world over this column" and a
mountain on the skyline is the world over that column. Nothing in `weather.h`
learns that ranges exist.

### 17.2 The palette and the shape are both read out of the tables

There is no biome name here either, and for §8's reasons:

| what | read from |
|---|---|
| the low ground's colour | `sod::LookAt` — the same meadow / steppe / taiga / desert mix that colours the grass at the player's feet, season and drought included |
| the rock above it | the rock row's `paint.tone`, through `soil::Build` |
| the snow on top | the snow row's `paint.tone`, gated by the snow row's own climate bell |
| **whether it is a range or a dune field** | the sand row's own climate bell — the very one that puts sand on the ground |

Every one of them is read at the row's own country rather than at the drawn
position, which is §17.2b and is not optional.

So walking from a pine forest into a desert turns the horizon with the ground, and
a cover added to `sod::kCovers` appears in the distance without this file being
opened.

**A desert's horizon is not a mountain range in sand colours.** That is the same
fault §8 calls a wood in the desert, one level up: a palette says what a place is
made of and a *shape* says what it is. So the ridged fold is blended out against
the sand bell into a plain summed field — no crease anywhere on it, broader by
half, and the whole lift scaled to two fifths, foot and all. Three numbers,
`duneRound`, `duneSpan` and `duneRise`, and each says one thing a dune is. The
altitude band goes with it: `alt *= 1 - dunes`, because sand has no treeline and a
grey top would put a crag back in the one place the shape was changed to take it
out of.

`Settings::duneEdge` is the one place the horizon is allowed to **disagree** with
the table it reads. §8 wants the ground's desert to have an edge; a silhouette is
the opposite case, because the ranges come down by three fifths across it — at the
ground's own width the skyline falls off a cliff inside one screen and reads as a
wall rather than as a country changing.

### 17.2b A range must stand still, and that decides where the climate is read

The fault, in one sentence: **a mountain stayed put on the skyline while its snow,
its colour and its very outline slid about underneath it**, so the distant country
rearranged itself while the player walked.

Everything about the *shape* was already a pure function of `sample`, the layer's
own content coordinate — the fold, the swell, the dune field, the two wandering
lines. What was not was the climate, which was read at the **drawn** position. A
layer's content moves at its own fraction of the camera, so a fixed piece of a far
range is drawn at a world position that runs away from it at nine tenths of walking
pace; asking the climate there asks about somewhere else, and the answer changes
every frame. That the rest was already still is exactly why it read as one feature
misbehaving rather than as the scenery moving.

`Range::Country` is the fix and it has two halves:

- **Ask at `sample`.** Nailed to the layer, so a given piece of a range has one
  climate for ever and nothing about it can change while the camera moves.
- **Divide by the distance.** Without it a far row would traverse its own country at
  a tenth of walking pace and a desert twenty screens across would show a fifth of
  itself on the horizon. With it, one screen of a row covers as much country as its
  distance says it does, the biome under the player's feet and the biome on the
  skyline change over the same walk, and a far row shows a wider sweep of the world
  across one screen than a near one — which is what distance means.

`--sky` checks it, and the check is worth keeping: render the same world at two
camera positions `Δ` apart and compare the top band against itself offset by
`distance × Δ`. For the far row at Δ = 7000 that is 700 px, and the pictures agree
to under two per cent — the residue being the nearer rows, which move by their own
amounts.

**And read it flat, without the altitude term.** `terrain::ClimateAt` cools its
answer by the elevation of the ground under it, which is right for the ground and is
a cliff generator here: a far row's country runs ten pixels for every one of the
view, so `terrain::Height` along it swings a hillside's worth between neighbouring
columns, and through the lapse rate that is a third of the temperature range — which
flips the desert on and off column by column and, since being a desert scales the
whole row, draws a wall of vertical slabs and one-column needles where a range should
be. It is also the wrong question: the lapse says how the air cools over the ground
*at that column*, and a range on the horizon is not standing on that ground. Its own
altitude is its own business, and the snow line already asks it as a height in the
row's own frame. `Range::Regional` reads the two fields at sea level.

### 17.2c The dither is a lattice, and it belongs to the row and not to the world

The complaint was that the ranges were seen **through sandblasted glass** — a fixed
speckled pane in front of the scenery, with the mountains sliding along behind it.
It is one fault and it took two goes, and the wrong go is the instructive one.

The first reading was that an *ordered* pattern is what magnifies into a screen
door: at one screen pixel per world pixel a Bayer 4x4 reads as a shade between two
tones, and at three, where a texel is nine pixels across, it reads as a grid. So the
matrix was swapped for a hash of the texel's position. That did take the grid out
and it took the drawing with it — an ordered dither is what carries a value cleanly
between two tones, and the same proportion of texels scattered at random reads as
dirt on the picture rather than as a shade.

**The lattice was never the fault. The frame was.** The threshold was keyed on the
*world's* texel indices, which is right for the terrain and wrong for anything
moving at its own speed: the pattern stayed where it was while the row slid past
underneath, so what the eye had to explain was a stationary pane in front of moving
scenery. A regular pane is simply a more legible one than a random pane, which is
why changing the pattern seemed to help and why it was the wrong thing to change.

The matrix is back, read in the row's own texel frame — the world's, shifted by the
part of the camera that row does not take, rounded to whole texels because the
pattern is a lattice of them. A fractional shift would resample it every frame,
which is a shimmer and a worse fault than the drift.

**And the rigidity check in §17.2b was blind to it**, which is worth knowing before
trusting that check again. It compared camera offsets of 1000 and 7000 px; the far
row's content moves 900 and 6300 world pixels, which are 300 and 2100 texels, and
both are multiples of four — so the matrix happened to land back on itself and the
pictures matched anyway. A Δ that moves the content a number of texels **not**
divisible by four is what catches it: at Δ = 30 the content moves nine texels and
the row moves three pixels on screen, and the pictures now agree to a tenth of a
per cent.

### 17.2d A picket fence is the octave gain, not the octave count

The near rows came out as a comb of identical needles standing shoulder to
shoulder, each one drawn as a vertical bar the whole height of the row — which is
the slope term, since a column's slope shades the whole of that column and a fold
that wiggles per texel gives a bar per texel.

The cause is arithmetic. A fold's contribution to the **slope** of the outline is
its amplitude over its wavelength, so an octave adds `lacunarity * gain` times the
steepness of the one above it. At a half against 1.93 that product is 0.97 — every
octave as steep as the last — and the finest one on a 190 px row draws a crest
every twenty-six pixels at the amplitude of a hill.

`gain` is 0.38, where the product is 0.73 and the fine work lies along the slope
instead of standing out of it. **The frequencies and the octave count are
unchanged**, which is the point: the fault was never that there was too much
detail. `Settings::finest` puts a floor under it as well — a crest narrower than
seventy pixels is noise on the outline rather than a mountain, which is §9's
lattice argument one level out; the broad rows still take all four octaves and the
near ones stop at two.

### 17.2e Measure a face in texels, not in pixels — and vertical motion is what says so

A flicker that showed **only when the player moved up or down**, and the asymmetry
is what named it.

`depth` — how far below the crest a texel is — was the true distance to where the
crest mathematically stands. That slides continuously as the camera rises while the
texels stand on the world's grid, so it drifts through a texel's worth of value; and
it drifts by the *same amount in every column at once*, because every column's apex
moves together. The lit rim and the volume under it are steep functions of it, so
the whole face changed tone in step and the eye read a flash. Sideways the identical
drift is spread over the columns at every phase, which is why it reads as texture
there and was invisible.

Counted in **rows down from the row the crest was drawn in** it is an integer, so
the shading steps exactly when the silhouette steps and never between. Which is also
the rule the rest of the picture is drawn by: a lit rim is the top so many texels of
a face, not the top so many pixels of a curve that happens to be sampled there.
`above` — which carries the treeline and the snow line — is counted the same way off
the same rounded horizon, or those two sweep through the picture between texels for
exactly the same reason.

**The check has to allow for the air.** A view moved thirty pixels up is looking
through a different part of the sky gradient, so the haze legitimately changes and a
byte-for-byte comparison is 100% different at every offset. Compare mean absolute
difference instead and look for the minimum: it lands at three pixels, which is
`distance x 30`, with 471 pixels of 48000 differing by more than sixteen.

### 17.2f The crawl that is left is inherent, and the dither width is the knob

Content that moves over a fixed lattice of texels cannot avoid temporal aliasing.
A piece of a row is rasterised on the world's grid, its dither threshold is a
lattice in the row's own frame, and the two slide past one another by up to one
cell as the camera crosses a texel — so every texel sitting near a quantisation
boundary flips as the player walks.

There is no arrangement of frames that removes it: the threshold has to be a
lattice for the dither to draw cleanly, and the picture has to be rasterised on the
world's grid for the blit in §17.4 to be exact. What can be chosen is **how much of
the picture is near a boundary at all**, which is what `ditherGround`, `ditherTone`
and `ditherSnow` say. At 0.55 a little over half of every step was dithered and the
flicker was easy to see; at 0.42 it is under half and the banding still has
somewhere to go.

### 17.3 Near to far, with a ceiling per column

Every row is opaque, so a texel covered by the range in front of it is a texel the
ones behind never have to be asked about. The stack is walked **near to far** and
each column keeps the first row not yet painted; a layer only shades what is still
open above it, and every texel of the view is shaded exactly once. Drawn the
natural way round — far to near, each row painting over the last — the same texel
is shaded five times and four of those are thrown away.

Each column is then **cut at the generated surface** (`terrain::Height + sink`),
which is what keeps the cost to the sky it is actually drawn in: everything below
is covered opaquely by the terrain or by `World::DrawUnderground` behind it. The
*generated* surface and not the built one, deliberately — a hole somebody dug shows
the deep rock the underground fill paints, and a range seen through a doorway would
be a mountain inside the hill.

### 17.4 It is a texture, and the runs are why

It was submitted as runs of one colour first, on the model of every other
rasteriser here. That measured **10.0 ms a frame** — a third of it — for one layer
of scenery.

The reason is in the haze: it mixes towards the air, the air is drawn in ten-pixel
bands, and a texel is three, so **the colour changes every third row however flat
the mountain is** and no run can ever be longer than that. Eighty thousand
rectangles a frame is not a rasterisation strategy.

The picture goes into a `Texture2D` instead and is blitted once: 10.0 ms to 0.11 ms,
and the whole horizon is **2.2 ms** of a 20 ms frame — 11% of it, nearly all in the
shading. It is exactly the argument
§5.5 makes about the ground — one texel per square of the grid the colour was
worked out on, point sampled, blitted at that scale, so the picture is identical to
the rectangles it replaces — with the one difference that this **cannot be cached
between frames**, because the parallax moves it and the day recolours it. What the
texture saves is submission, and submission was all of the cost.

The buffer is therefore **row major** and the shading takes the stride, rather than
the other way round.

### 17.5 Two things that were measured, not argued

- **Jitter a line with `Signed`, never with `Sample - 0.5`.** The folded field is
  crowded hard around its own midpoint, so `Sample - 0.5` visits about a seventh of
  its nominal swing — a snow line jittered by 130 px came out very nearly straight,
  a ruled horizontal edge across every peak on the screen. It is the same trap the
  wind envelope and the cloud cutoffs are measured to avoid, arrived at from the
  other side: there the fix is to measure the field, here it is to use the one whose
  zero set is reachable.
- **One octave of mottle.** It is the only term read per *texel* rather than per
  column, so it is the whole of what the shading costs: 1.77 ms at two octaves
  against 1.53 at one, for detail finer than the mottle is meant to have.

### 17.6 What to check

`--sky x y w h out.png [zoom] [mood] [hours]` draws the air, the ranges and the
cloud in the frame's own order.

`--probe x y w h out.png [zoom] [seconds] [plants] [lit] [mood] [season] [sky]` —
the trailing `sky` is new and is **off by default**, deliberately. Every reference
picture that probe has ever taken was taken against its flat blue, and the whole use
of them is that two runs are compared byte for byte; the day, the weather and the
ranges all have to stay out of the picture unless they are what is being looked at.
With it on, it is the only still picture of the world as it is actually seen.

Read `--covers x0 x1 step` first to find the country worth photographing. A desert
is 2% of columns in this world, so a horizon of dunes is not something to go looking
for by eye.

---

## 18. Where the files are now

`src/` was seventy-seven files in one directory. It is the same code in ten:

| folder | what is in it |
|---|---|
| `core/` | the machinery that is not the game: the pool, the profiler, the registry, the picture and figure types, config, view, mode |
| `world/` | the ground: `world`, `terrain`, `cave`, `element`, `soil`, `sod`, `water`, `marching_squares` |
| `weather/` | the sky, the horizon, the backdrop |
| `flora/` | the wood: species, the scatter, the canopy |
| `entity/` | everything that is in the world without being the world — the body, the player, the creatures, the pickups, the fixtures, the footsteps |
| `item/` | what is carried, and the pack |
| `craft/` | recipes, and what a bag of materials is worth |
| `render/` | the order the world is drawn in, and the light |
| `ui/` | what is drawn in the frame's coordinates: the display, the menu, the console |
| `hand/` | the editor — digging and building |
| `probes/` | the world measured rather than played |

**The build globs every `.cpp` under `src/`** with `CONFIGURE_DEPENDS`. There is no
list of sources anywhere, and that is half of what makes a content row register
itself: the other half is the registrar beside the row, and this is what guarantees
its file is compiled at all.

The whole move was held to §2's bar — `--sun`, `--column`, `--build`, `--tones`,
`--covers` and a `--probe` picture, byte for byte, before and after.

---

## 19. A row registers itself

`src/core/registry.h`. This section is about a change of principle, and the
principle is the Open/Closed one: **adding a thing must not mean editing a file
that already works.**

Every table used to be an array with every row in it and an `enum` naming the
positions. That is a good design right up until the table is long, and then every
addition is an edit in the middle of a file full of things that are already correct.

So a row lives in its own file, and a `registry::Registrar` beside it files the row
before `main` runs. A new item is `src/item/items/hide.{h,cpp}`. A new creature is
`src/entity/mob/mobs/wolf.{h,cpp}`. A new behaviour is
`src/entity/mob/brains/pack.{h,cpp}`. **No existing file is opened for any of them**,
including `CMakeLists.txt`.

### 19.1 Ids are sorted by name, and that is the whole difficulty

A row's id ends up in `Stack::what` and will end up in save data, so it has to be
the same number on every build of the same content. Static initialisers run in an
order the standard does not fix and the linker is free to change, so **registration
order is not an identity**. Handing out ids as rows arrive would mean a world saved
today loading as something else tomorrow, silently, with stone where the diamond was.

`content::Open()` therefore freezes every table after every registrar has run, and
assigns ids by sorting on each row's own name. The numbering is a pure function of
*the set of rows* — nothing to do with the order they arrived in, the order the files
compiled in, or the platform. A table refuses to be read before it is frozen, which
is a loud abort rather than a wrong answer.

`content::Open()` is the **first statement in `main`**, before the probes and before
the window.

### 19.2 What was given up, and what replaced it

The old tables were `constexpr`, and a handful of `static_assert`s walked them at
compile time. Those cannot survive a table assembled at run time, and pretending
otherwise would be worse than losing them.

They are `registry::Checker`s now, run together by `content::Open`, reporting **every**
fault and then refusing to start. That is weaker than a compile error and much
stronger than nothing: a bad row is found before the window opens, named in a line
you can act on, and impossible to play past.

```
content: fixture 'torch' is put up from 'tourch', and there is no such item
content: 1 fault — refusing to start
```

That was produced by breaking a row deliberately and watching it fire, which is the
only way to know a guard guards anything — the discipline §16.2b describes. The exit
status is 1.

**What must never happen is a check quietly downgraded to a warning.** The whole
argument of §16.2b is that a row which looks added and is not costs a day of looking
in the wrong place.

### 19.3 One table names another by name, not by handle

A creature's row is `constexpr` and an item's id is not known until startup, so
`Spoil::item`, `SpeciesDef::sapling`, `DropRule::item` and `fixture::Def::from` are
all `const char *`. `item::Named(name)` resolves one; `Verify` has already
established that every name in every table resolves, so a lookup that fails at run
time cannot happen in a build that started.

`nullptr` means *says nothing* — a plant that does not sow, a creature that drops
nothing. That distinction is the one §16.2b was about, and it survives the move to
names because `nullptr` and an empty string are not the same thing.

### 19.4 A seed comes from a name, never from a row number

`core/hash.h`. Indices are stable across builds and still **shift when a row is
added**, because everything after the new name moves up one. Anything that seeded a
roll from an index would have every tree already in the world drop something
different the day a new item was added — a world quietly rewriting itself for a
reason nobody could see. `hash::Of(name)` does not shift.

---

## 20. Everything that walks walks through one body

`src/entity/body/`. Three files, and the middle one is the seam the whole creature
layer hangs off.

- **`build.h`** — `body::Build`: the thirty numbers a body is made of. It was
  `player_config`, a namespace of constants, which is exactly right while there is
  one body in the world and exactly wrong the moment there are two.
- **`intent.h`** — `body::Intent`: six wishes. `moveX`, `moveY`, jump pressed and
  held, crouch, sprint.
- **`body.h/.cpp`** — `body::Body`: the walk. Gravity, the ledge it steps over, the
  corner it is nudged past, the ground it is snapped back down to, the rock it is dug
  out of, the water it floats in.

**The seam already existed.** `Player` was deliberately written to know nothing about
the keyboard, taking a snapshot of intentions instead — put there for remapping and
replays. That indirection turns out to be the thing that makes a creature cheap: an
animal is a body with the same six wishes filled in by a brain rather than by a hand.

So there is **no AI movement code anywhere in this project, and there must never be
any**. A creature that walked by some other route would drift from the character's
walk the first time either was tuned, and the symptom is a mob climbing a ledge the
player is stopped by, or falling through a floor the player stands on.

`Player` now *has* a body rather than *being* one. What is left in that class is what
is actually the character: the aim, the swing, the health and the drawing.

**Two kinds of not-falling, and they are different fields in different places.**
`Build::floats` is a creature gravity does not act on which still collides with the
mountain — a bat, and a fact about the row. `Body::Ghost` is the free flight, which
passes through rock, and is a state the player is put into.

---

## 21. Adding a creature

One file. `src/entity/mob/mobs/<name>.h` with the row, and a `.cpp` beside it holding
the registrar. Nothing else in the project is edited — not a table, not an enum, not
the build.

| the row says | and this follows, with nothing else edited |
|---|---|
| `look` | how it is drawn, at the world's own texel, in both facings |
| `build` | how it walks, falls, swims, steps over ledges and is dug out of rock |
| `temper` | which brain drives it, by name |
| `hardy`, `hits`, `knock`, `lift`, `reach`, `rest` | what it takes and what it deals |
| `notices` | how far it reacts, in whichever way its brain reacts |
| `spoils` | what it leaves, by item name |
| `haunt` | where it comes from: depth band, light band, climate bell, group size, crowd cap, keep-away |
| `burnsInDaylight` | whether the sun ends it |

**The rule to keep**: a fact about a creature goes on the creature's row. If
answering a question about a boar means writing a test on its name anywhere outside
that file, the field is missing. That is §16.1's `ElementDef::loose` lesson, one table
further on.

The `haunt` reads the same `ElementClimate` bell the covers, the woods, the drought
and the snowfall are placed by. There is no biome table for creatures either, and
there must not be one: the moment a mob names a biome, the boar's country and the
grass's country are two facts that can disagree. See §8.

### 21.1 A behaviour is a file, and a creature is not

`src/entity/mob/brains/`. Two today — `drifter` wanders, `skittish` grazes until it is
hurt and then bolts. A creature that behaves like an existing one costs nothing here; a
*new* behaviour is one file and its registrar.

There was a `stalker` that hunted the player, and a shade that used it, and a bat. All
three were taken out on the strength of playing them: a hostile creature is a decision
about what the game *is*, and it was made too early. Nothing else moved when they went
— no table, no enum, no build file — which is the registry doing exactly what it is
for. Note what the removal left behind: `Brain::Notices`, `Build::floats` and the
whole floating branch of the navigator are all still there and still correct, because
they were written as facts about a body rather than as facts about a bat.

**A brain is stateless and shared.** One `Drifter` answers for every bat in the world,
so it must hold nothing about any of them — everything a behaviour remembers goes in
the `Wits` it is handed, which is one small struct per creature rather than one object
with a vtable per creature. It is also what makes the layer safe to run across the
cores later, since nothing a brain touches belongs to anybody else.

**A brain returns a wish, not a move.** See §20.

**What a brain can see is `Sense`, and it is deliberately narrow.** No herd — a
creature that could read every other creature's position would make each brain cost
the size of the pool, and would let a boar react to a boar off screen. Nothing
mutable. No input. Widen that struct and every behaviour in the project quietly gains
a new way to be surprising.

### 21.2 A place has a population, not a spawn rate

The spawner was **stateless** and that was the whole fault. It tried a random spot
every second or so and let anything outside the view go, so walking a few screens
and back gave a different set of animals every time, killing one was undone by
turning round, and a stretch of country had no population so much as a *rate*. None
of that is what a world is.

`mob::Warren` replaces it, and the design is one field of `mob::Patch`:

- **The world is cut into cells**, 512 by 384. Two bands of them cover the surface
  and the caves under it separately, so walking about on top does not keep waking
  the bats underneath.
- **`Patch::settled` is one bit per creature kind**, set the first time that kind's
  population was rolled in that cell, and **never cleared**. That single bit is the
  whole of "the dead stay dead": nothing will ever ask this cell for boars again, so
  the ones that were killed do not come back. No death is written down anywhere —
  the record of a creature is its absence.
- **The roll is a pure function of `(cell, kind, seed)`**, so the same world holds
  the same animals in the same places in every session. Not a function of the clock,
  the view, or the order cells were visited.
- **Leaving writes a creature back**, where it got to, with what it has left and
  what it was in the middle of doing (`mob::Life`). Coming back wakes it there. The
  `wits` go with it, or a boar you frightened is grazing calmly when you return and
  every animal in the county steps off on the same foot at the same moment.

This is `Grove::TreeState::cleared`'s argument one table over, and it is worth
restating because it is the thing that is easy to undo: **the record has to outlive
the thing it is about.** A cell with no record is a cell full of animals, so
forgetting a cell the player has hunted out is the same as putting the animals back.

**A cell is settled per kind and not once**, which is what keeps night creatures
possible. A meadow settled at noon has its bit for boars set and its bit for shades
clear, so the first night the player spends standing there settles the shades. The
*content* stays deterministic; what depends on when you arrived is only which kinds
have had their conditions met yet, and there is no other answer that is both
consistent and has night creatures in it.

Two edges rather than one — `kWakeOut` at a cell and `kSleepOut` at two and a half —
because a single edge means a creature standing on it is woken and slept on alternate
frames as the camera breathes, which is the churn the module exists to remove. The
wake edge is outside the view on purpose: nothing appears in front of the player, it
walks in.

`Haunt::crowd` went with the old spawner and could not survive this. A global ceiling
on a kind would make whether a creature exists depend on how many others happened to
be awake somewhere else, so walking far enough would change how many boars a meadow
holds. What bounds a population now is `most` against the size of a cell, which is a
density — and a density is what a country has.

**Two costs, both measured.** `Close` used to walk every cell ever visited, once a
frame, so standing still got dearer the longer the session had run — 0.78 ms after a
couple of minutes, and worse from there. It walks a list of the paged-in cells now.
And a kind that cannot be placed *yet* — a shade at noon — must not re-probe every
frame; `Patch::askAgainAt` holds it for three seconds of the weather clock, which is
the difference between a millisecond a frame and nothing.

### 21.2b The spawner's own bug, kept for the lesson

Before any of the above, `spawn::Try` picked a uniform point in the simulated region
and tested it against the row. A boar's depth band is 56 pixels of a region thirteen
hundred deep, so about one candidate in twenty-five was even in the right place;
with six tries, about two attempts in a hundred succeeded. **What that looked like
was an empty world** — no error, no warning, and `--mobs` reporting the country as
perfectly suitable, because the country *was* suitable and the spawner was never
asking about it.

The height comes from the row's own band, measured off the surface at the chosen
column. Every candidate is in the band by construction and the refusals are left to
be about things worth refusing: rock, light, climate.

---

## 22. A creature walks by the same arithmetic it jumps by

`src/entity/nav/`. Three files, and the first of them is why the other two are short.

### 22.1 What a body can reach is derived, never tuned

`nav::Reach` is worked out from `body::Build` and nothing else:

| | from |
|---|---|
| how high a ledge it can land on | `jumpSpeed² / (2·gravity)`, times `kApexUsable` |
| how long it is in the air | `2·jumpSpeed / gravity` |
| how wide a hole it can clear | that times the pace it is *actually going*, times `kSpanUsable` |
| how far it will drop | equal to what it can climb — or three times that, if it has a reason |

Check the arithmetic against figures this project already had: for the character the
apex is 480² / 3200 = **72 px**, which is what `player_config` has always claimed; the
arc lasts 960 / 1600 = **0.6 s**, which is what the sprint's comment says; and a
sprint carries it 380 × 0.6 = **228 px**, which is the figure that comment gives for
clearing a hole. Nothing was invented — it is the same numbers read forwards.

So a creature added tomorrow with a jump of its own gets a navigator that knows what
it can reach, and nobody has to work it out.

**The pace is the pace it is going, not the row's top speed.** A drifter ambles at
0.45, and a navigator handed the row's figure would decide every hole was jumpable
and then fall short of all of them. `nav::Plan` takes a throttle.

**The drop is equal to the climb by default**, and that is the rule that keeps a
wandering animal out of holes it cannot get out of again — the world otherwise fills
its ravines with creatures that are alive and stuck, which is worse than either
dying or staying up top because nothing on screen says what happened. A fleeing or
hunting creature is allowed three times as much: being stuck later beats being caught
now, and that is a decision the *caller* makes rather than a field on the row.

### 22.2 Jumping at a wall does not climb it

The one piece of arithmetic in the navigator that is not obvious, and it is the
difference between a creature that climbs a terrace and one that bounces at the foot
of it for ever.

`Body::MoveAndCollide` resolves the horizontal axis first and `StepOver` refuses
while airborne — deliberately, or anything would climb any wall it brushed. So a body
that jumps *at* a wall has its horizontal speed zeroed on contact, rises straight up
the face, and comes down in the same place. It has to already be **above the top** by
the time it arrives.

`LeadFor` is how far before the wall it must leave the ground: height `h` is reached
at `t = (v - √(v² - 2gh)) / g`, and in that time it covers `pace · t`. For a boar
clearing a 24 px terrace riser at its bolt that is about fifteen pixels.

### 22.3 A leap is committed to, and the reading is cached

Two things `nav::Legs` holds, for opposite reasons.

**The leap.** A plan made afresh every frame unmakes a jump halfway through the arc:
the moment the body leaves the lip, the gap it was clearing is behind it, the scan
reports open ground, the wish to hold the jump goes away — and the shortened arc
drops the creature into the hole. The arc is held for `airtime` and not re-planned.

**The last reading**, which is the one thing in the navigator that exists for speed.
The *decision* is deliberately not cached, only the reading — a cached decision would
repeat a jump press every frame until it expired. Its distances are shifted by how far
the creature has travelled since, which keeps the jump timing exact while a strolling
animal scans once every several frames.

### 22.4 The scan has to see over the hole, not up to it

The fault `--nav` was written and immediately caught, and it is worth keeping written
down because the symptom pointed at the wrong half of the module: **every gap, at
every width, reported as uncrossable, with the creature standing at the lip.**

The horizon was `reach.gap` plus a couple of strides, measured **from the body**. So
a hole eight strides ahead used the whole of it getting there and had nothing left to
see over the hole with; the far side fell outside the scan, `gapEnds` stayed false,
and the planner correctly refused to jump into what it had been told was bottomless.
What the reading has to cover is the distance to the near lip *plus* what the creature
could clear from it, so the horizon extends once a hole is found.

### 22.5 Two of the three brains never look at the player

A performance question that had to become a design one, and it is `Brain::Notices`.

Working out whether a creature can *see* the player means walking the straight line
between the two and asking the world about every step — some thirty lookups, per
creature, per frame. A drifter notices nothing by definition, and a skittish animal
reacts to being *hit* rather than to being approached, which is what makes a boar
something you can walk up to. Only a hunter needs the answer, and most creatures were
paying for a number nothing read.

**What the cost turned out to be** is worth recording, because three plausible
suspects were wrong first: it is not the ground scan, not the sight test, and not the
settling. `herd.Think` is **the bodies** — `body::Body::Step` costs about 23 µs, which
is what the player's own zone reports for one, and twenty creatures is twenty times
that. 0.46 ms of a 16 ms frame, and it scales with how many are awake. There is
nothing clever to do about it; a walking creature costs what walking costs.

### 22.6 What to check

`--nav [mob]` builds a course out of blocks — a run of floor with a hole of a known
number of cells in it, then one with a step of a known height — and walks a body
along it. A hillside has no holes of a known width in it, so a test against the
landscape can only ever say "it got somewhere", which is what watching it does and is
not a check.

**The verdict is one-sided and that is the design of it.** A promise the body could
not keep is a fault: the planner said the hole was jumpable, the creature jumped, and
it is at the bottom of it. A hole the planner *refused* and the body could in fact
have cleared is not — `kSpanUsable` holds the planner to under three quarters of the
arc the physics allows, deliberately, so the body will always out-reach the planner
near the limit. A check that called that a disagreement would be a check against the
margin existing. The `spare` column is what makes the margin judgeable instead of
assumed.

`--mobcheck [seconds] [away]` settles a stretch of country, walks far enough away
that every creature is asleep, comes back, and then kills them all and does it again.
It compares **rolls, not bodies**: a creature standing where a dead one was might have
been rolled afresh — the fault — or might have walked in from next door, which is an
animal doing what animals do, and counting bodies cannot tell the two apart. That
distinction was found by the check reporting a failure that turned out to be a boar
going for a walk.

It also builds its course *after* streaming the ground, which it did not at first —
every cell went into a chunk that did not exist, so nothing was written, and the
report came out as a solid column of failures with no jumps in it. A probe that
builds has to stream first, exactly as the game does.

`--mobs x0 x1 [step] [hours]` is the third, and it is about the *rows* rather than
about the code: a placement rule is very easy to author into something that is never
anywhere, and nothing errors. It reports, per creature, how many spots suit it and
**why the rest were refused**, one column per band, so a row that fails says which of
its own fields did it.

**It sweeps the day by default, and that is the design of its verdict.** A creature of
the dark is legitimately nowhere at noon, so a report at one hour cannot tell "this row
is wrong" from "you asked at the wrong time". With no hour given it walks midnight,
dawn, noon and dusk and fails only a row that appears at none of them.

It also reports the **density** — one creature every so many pixels — which is the
number "there are too many boars" is actually about. See §23.3.

`--critters out.png [zoom]` is the fourth: a contact sheet of every creature at the
world's own texel, in both facings, with its collider drawn over it. A mirrored sprite
is the one fault invisible in a single still — a boar whose snout is on the wrong end
looks fine until it turns round.

---

### 22.7 Knowing *when* to jump is a different question from knowing *whether*

Three complaints arrived together and they had one cause: the planner decided a jump
was possible and then timed it against the pace the creature *meant* to be going.

**A slow creature cannot climb at all, however well the jump is timed.** A boar
ambling at 32 px/s covers fourteen pixels in its entire 0.44 s arc; a terrace riser is
twenty-four and the body is twenty wide. There is no moment to leave the ground at
that would work. The old planner did not know this, so it jumped, fell short, jumped
again — for ever. That is the head-butting.

`nav::PaceToClimb` is the missing figure. The jump is above a height `h` between
`t1 = (v − √(v²−2gh))/g` and `t2 = (v + √(v²−2gh))/g`; in that window the body covers
`pace × (t2 − t1)`, and it needs that to be at least its own half-width and a bit or
it comes down with its feet on the lip. Below that pace, **do not jump** — run.

**So seeing a ledge is what puts a creature at full pelt.** `Doing::RunUp` sets
`moveX` to the whole direction the moment a climbable ledge is in the reading, and
`nav::Advance` turns that into `sprintHeld`. An animal trots up to a step and hops it,
which is also what animals do.

**And the moment is the apex.** Jump when the face is `|velocity.x| × jumpSpeed/gravity`
away, so the body arrives at the top of its arc: the most height to spare, and the most
room either side of the instant to be wrong in. Against `velocity` and never against
the intended pace — a creature stopped dead by the very wall it is trying to climb is
going nowhere, and timing against what it meant to be doing is how it jumps on the spot.

**A body already against the face has no room left**, and no amount of re-deciding
makes any. `Legs::backing` reverses it for `kBackOff` and nothing reconsiders until
that is done — the ledge is still there the whole time saying "climb me", which is why
the back-off is tested before the reading rather than after it.

**And it gives up.** Three attempts at one ledge is a ledge this creature is not going
to climb, whatever the arithmetic says: the ground is a contour, not a stair, and there
are shapes the numbers do not describe. The tally resets when an attempt actually
changed the floor under it, or a staircase would be mistaken for one ledge tried three
times.

### 22.8 A drop is not a hole, and telling them apart was the whole fix

The second complaint — *when it means to go down a block it should not jump* — and the
third — *it climbs onto a block, freezes, and then goes back to normal* — were the same
bug, and it was in the **reading** rather than in the planner.

`nav::Ahead` used to know about two things: a rise it could not walk up, and ground
that was not there. A descent deeper than the scan's band therefore came back as a
**hole with no far side**, which is a thing a creature refuses — `moveX = 0`, stand
still. On top of a block, looking down, that is a creature that has climbed up and
frozen. And a descent whose bottom *was* found came back as a crossable hole, which is
a thing a creature jumps.

There are three kinds of thing ahead now — `toClimb`, `toDrop`, `toGap` — and the
answers are different for each: climb it, walk off it, jump it. Whichever is nearest
wins. A drop is **never** jumped: a jump at a descent throws the body further out than
it meant to go and lands it harder, and on a staircase it reads as an animal bouncing
downhill.

A drop also does not stop the scan, where a climb and a hole both do. What is past a
wall does not matter; what is past a *step down* very much does, or a creature is
walked onto a ledge over a chasm it never saw.

### 22.9 What `--nav` checks now

Both paces, and the slow one is the point — a probe that only tested the bolt said
everything was fine while an ambling boar could not climb a kerb.

| column | fault |
|---|---|
| `FELL SHORT` | the planner promised a jump the body could not make |
| `SPAMMED` | more than two attempts at one obstacle |
| `JUMPED DOWNHILL` | a descent that cost a jump |
| `spare` | refused something it could have done — **not** a fault, that is the margin |

And a final section drives the **brain** rather than the planner: a boar struck from
behind, over a step up, a drop and a wall it cannot climb, with the longest stretch it
spends motionless measured. A frightened animal may hesitate and may not stop.

Two things about that section, both learned by it passing when it should not have:

- **It has to be refused something.** Without the wall the creature never turns, the
  guard that used to freeze it is never reached, and the check passes whatever that
  guard is set to. `turns` is printed so a run that never met the wall says so.
- **Stillness is only counted while it is frightened.** Measured over the whole run it
  reported half a second of stillness that turned out to be the animal *resting* — a
  fright wears off and a calm drifter stands about for up to three seconds by design. A
  check that cannot tell a frozen creature from a grazing one fails on correct
  behaviour, which is the fastest way to get a check switched off.

---

## 23. How many creatures a place holds, and how that was got wrong

### 23.1 The cell hash was handed half its input

The worst bug of the lot, and the one nothing could see.

`Warren::Settle` mixes a stream from the cell key, and it took
`Key(cx, cy) & 0xFFFFFFFF` — the **low half**. The key packs the column into the high
half, so every cell in a row shared one stream, made one chance roll, and answered
identically. One unlucky roll therefore emptied *every cell at that height in the
world*.

What it looked like: a county with no animals in it. No error, nothing in any log, and
`--mobs` cheerfully reporting the ground as perfectly suitable — because the ground
*was* suitable and nothing had asked it.

It was found because `--mobcheck` prints its counts rather than only a verdict: **279
cells asked, 6696 spots tried, none suited** — with the very spot the probe was standing
on passing `mob::Suits` when asked directly. Nothing short of those five numbers could
have told a bad hash from bad ground, and three plausible suspects were wrong first —
the light, the placement height, and the room test.

**Every hash of a packed key folds both halves now.** It is a two-line fix and it was
an hour to find.

### 23.2 A creature is stood on the floor, not dropped into the band

The §21.2b mistake in its second form. `Settle` sampled a height inside the row's depth
band and hoped it had landed on the ground — but a boar's band is 56 px of mostly open
air and `mob::Suits` demands something solid within two pixels under its feet, so about
one try in twenty could ever succeed.

A band that reaches above the ground is a creature of the surface, and `World::SurfaceOf`
is what it stands on. One wholly below is a creature of the caves, and there the floor
has to be walked down to with `World::FootingUnder`. Either way the spot is *found*
rather than guessed at, and the refusals are left to be about light, climate and
headroom.

`mob::Suits` also stops its room box a pixel above the feet, because a body does not
occupy the ground it is standing on — without that, every spot found by walking down to
a floor was then refused for being inside it.

### 23.3 The density is measured, and Minecraft is the yardstick

"There are too many boars" is a statement about density and there was no way to measure
it, so `--mobs` now settles a warren over the stretch it walked and reports **one
creature every N pixels**.

Minecraft's own rule, which is what the rows are written against: animals come almost
entirely from **chunk generation** rather than from ongoing spawning — a pack is rolled
per chunk at a probability of 0.1, on grass, in light 9 or over. Ongoing passive
spawning exists but runs once every 400 ticks against a cap of 10 per player, so it is
nearly irrelevant. A chunk is 16 blocks and a block here is 18 px, so **Minecraft's rate
is one pack of 4 every 2880 px of walking — one animal every 720 px.**

Ours measures **one every 1481 px**, which is half Minecraft's. That is deliberate:
Minecraft scatters a pack of four through a 10-by-10 *area*, so a player meets one or
two of them at a time. This world has no depth to scatter anything through, so the whole
group is in view at once and the same felt density needs a smaller one.

`Haunt::chance` was 0.55 before the hash was fixed, and it had been tuned against a
world where whole rows of cells could never hold anything — it was compensating for a
bug, and the real density had never once been seen.

The knob is one line on the boar's row. Run `--mobs 0 40000 2000` after changing it.

---

## 24. The first art that came out of a file

Everything drawn in this world up to now was generated: `Picture` for an icon,
`figure::Figure` for a creature, `canopy` for a tree, and the ground out of
`marching_squares`. That was a choice each time — procedural paint has no asset to
keep in step and no size to get wrong.

The boar is drawn from a sprite pack now, and the change is smaller than it looks
because §12 already left the door open for it: every material is drawn at
`config::kPixelSize` because varying it opened a pale rind down the side of every
block (§12.1) — and that argument is about **materials sharing a contour and a
union**. A sprite drawn over the ground takes part in neither. §12 ends by saying a
finer texel is "worth it for authored art; not worth it for procedural paint", and
this is the authored art.

**So a strip's pixels are world pixels, one for one.** A 28-wide boar is 28 world
pixels across. That is very nearly the 24 the six-texel drawing came out at, which is
why nothing else on the row had to move — the body is still 20 by 16.

### 24.1 The cut is a tool, not a thing typed once

`tools/cut_sprites.py`. The packs are drawn for a top-down game: four rows of the same
animation, one per facing, and only the last — the side view — is any use in a
platformer.

Two decisions live in that file rather than in a shell history:

- **The window.** Across every frame of idle, walk and run the boar occupies x 1..29
  and y 5..26, and **the feet are on y = 26 in all of them**. So the bottom of the
  window is the ground line, and one window is used for every animation of a creature.
  Different windows per clip would mean a boar that changes height and jumps sideways
  the moment it starts walking.
- **Nothing is mirrored.** One right-facing strip, mirrored at draw time by a negative
  source width — the same thing `figure::Draw` already does for the hand-drawn art. A
  baked left-facing copy is twice the asset and a second thing to keep in step.

### 24.2 A row names a folder, never a file

`mob::Def::art` is `"boar"`, and the three clips inside `assets/mobs/boar/` are always
called `idle`, `walk` and `run`. There is no filename in the creature table to
misspell, and a misspelling there would show up as a creature quietly drawn from its
fallback with nothing saying why.

`mob::Wardrobe` loads them on the first draw and keeps them — a cache on the model of
`canopy::Sheet`, and asked for **by row** rather than by path. It has to be lazy:
`content::Open` runs before there is a window, and a texture needs one.

`tried` is distinct from "it worked". A creature with no art and one whose art is
missing must both stop trying, or a missing file is opened sixty times a second for as
long as the game runs. The warning is printed once.

**The hand-drawn `look` stays on the row.** Not as a leftover: it is what a missing
file falls back to, what the contact sheet draws against, and the one description of
the creature that cannot go out of date, because it is in the same file as everything
else about it.

### 24.3 The legs are driven by ground covered, not by a frame rate

`Def::stride` is eight pixels, and it is a distance rather than an fps for one reason:
feet that turn at a fixed rate skate over the ground at every speed but one, and a
creature that ambles *and* bolts has two. Driven by distance, the same six frames carry
it at both — the walk cycle is forty-eight pixels of ground, which is a little over two
of its own body lengths.

The idle is the exception and is counted in seconds, because standing still covers no
ground and a breath is a breath.

Both clocks are advanced in `Update` and never in `Draw`: a creature off screen still
has to arrive somewhere in its cycle rather than starting from the first frame the
moment it is looked at.

### 24.4 What `--critters` proves now

It draws four things on one ground line: the hand-drawn figure both ways round, every
frame of the walk strip, and — over the collider — **one creature drawn through
`Mob::Draw` itself**.

That last one is the point. Everything else on the sheet draws the art directly, which
proves the files are right without proving the game will ever show them: the clip is
chosen from the creature's own motion and the wardrobe is loaded on the first frame it
is looked at, and both of those live in `Mob`. A sheet that skipped them would go on
looking perfect while the world drew nothing at all.

---

## 25. The foot of the screen is one layout, not three

### 25.1 What was wrong

Three files each put something above the hotbar and none of them knew about the
others: `hotbar.cpp` drew the held item's name twenty pixels up, `hud.cpp` put the
brush badge at a hundred and four from the bottom and the health bar at ninety-six.
All three landed inside the same twenty pixels. What that reads as on screen is two
lines of text touching with a bar behind them — and the only way to find out was to
launch the game and take a screenshot.

`bottom::Of()` is the whole layout now, as a pure function of the window size, and
every row is measured **off the hotbar** so that a change to the bar's size or margin
carries the rest with it. It is computed again in the draw rather than remembered,
which is §14's rule for every menu screen and is the same rule here.

### 25.2 The arrangement is Minecraft's

```
        wood plank            <- what is held, centred over everything
  ♥♥♥♥♥♥♥♥♥♥        brush 1x1 <- vitals left, the hand right
  [1][2][3][4][5][6][7][8][9] <- the hotbar
```

Java Edition stacks, from the hotbar up: the experience bar, then health with hunger
beside it, then armour, with the held item's name floating over the lot. The half of
the row where hunger sits is "the thing about you that is not your health", and this
game's version of that is which tool is in hand and how wide it cuts.

**The row is split by what the vitals need, not down the middle.** A half is a guess,
and the moment a heart changes size the guess is either crowding the row or wasting
it.

One deliberate departure: **the name does not fade.** Minecraft fades it after a
couple of seconds because there it is a reminder while you scroll and the bar is
otherwise stable. Here a slot can hold a material, an item or a fixture and those are
not all obvious from the picture, so the name earns its line permanently.

There was a second, and it was a mistake worth writing down. The hearts hid themselves
at full health, on the reasoning that a row which has never moved is furniture. What
that missed is that **nothing in this game damages the player yet** — the boar's `hits`
is zero and the hostile that had a number there was taken out — so the health never
fell, so the row never appeared *once*. A display that is invisible until a condition
nothing can currently produce is not restraint; it is a feature that looks broken, and
it was reported as one within a minute of being played.

They are always on in Survival now, as Minecraft has them. The reason is not that
Minecraft's hearts move more often: a readout has to be somewhere the eye already knows
*before* the moment it matters, and one that appears for the first time during the
emergency is one the player has to find while being hit.

### 25.3 Hearts, and why `kHealth` is twenty

A bar says *how much of it* is left; a row of hearts says *how many more hits*, and
the second is the question a player in trouble is actually asking. It is also
countable out of the corner of an eye, which a bar at 40% is not.

Ten hearts of two points each, with halves — so a bare fist at `kFistDamage` takes
exactly half of one. **That is why `player_config::kHealth` is twenty and not a
hundred.** A hundred is a fine figure for a bar and a useless one for hearts: it makes
a punch a fiftieth of the row, which is a step nothing can see. The three numbers are
chosen together and only work together.

A heart is twenty-one pixels against a forty-four pixel slot, which is the proportion
Minecraft draws — nine against twenty. It was fourteen first and read as decoration:
at two pixels a cell the notch between the lobes closed up and the row stopped being
countable.

The outline round each one is not decoration either. A heart is drawn over whatever
the world happens to be behind the hotbar, and a red shape on a red hillside at sunset
is not a shape — the same argument `hud::Label` makes about the readout, in a picture
instead of in text.

### 25.4 Survival only, and drawn as one call

Minecraft hides health, hunger, oxygen, experience and armour outright in Creative,
and it is right to: a number that cannot change teaches the eye to stop looking, and
then the one time it does mean something it is missed. The mode is asked for inside
`vitals::Draw` rather than tested by the caller, so there is one place the rule lives.

The strip is drawn by **one call**, `hud::Strip`. It was two — `hud::Draw` for the
badge and the health, `hotbar::Draw` for the bar — and that is how the hearts came to
be drawn *underneath* the inventory panel, the panel replacing only the bar. Anything
laid out together has to be drawn together, or the layout is a coincidence.

### 25.5 `--hud` exists because this could not be looked at

Every other thing this project draws has a way of being seen without playing:
`--critters` for a creature, `--probe` for the world, `tools/sheet.cpp` for a tree. The
head-up display had none, which is exactly why three files could quietly claim the
same twenty pixels.

```bash
./build/CppGame.exe --hud out.png [health] [wide] [tall] [creative]
```

It draws `hud::Strip` itself and never a copy — a probe that reproduced the layout
would be checking the reproduction. Three things about how it reports:

- **The plate is four bands**, not a flat grey: sky, grass, rock and the dark. The
  readout is drawn over a moving world and the one thing it has to survive is the
  background changing under it, so a layout judged against a neutral panel is judged
  against the one background it will never have.
- **It prints the rows and the gaps as numbers**, and fails when any gap closes.
  "The information is too close together" is a statement about pixels, and a picture
  alone cannot be measured with a ruler.
- **The mode is an argument** because the Creative rule's whole effect is that
  something is *not* there, and no single picture shows an absence. Render both and
  compare.

It also has to ask the window what size it actually got: `config::kMinScreenHeight` is
a floor, so a size the window refused would give a picture of the strip hanging off
the bottom of a texture of the wrong shape.

---

## 26. Adding an item

One file. `src/item/items/<name>.h` with the row, and a `.cpp` beside it holding the
registrar. §16.1's table of what a row buys still holds; what has changed is that
there is no `kItems[]` to insert into and no `enum class Item` to extend.

§29 added two fields to that row and both follow the same rule: `art` names an
authored picture and the item is drawn from it everywhere without any caller being told
(§29.1), and `tool.lasts` says how many blows it has in it and the durability bar,
the tooltip and the three things that wear a tool all read the one number (§29.2).

A row's id is reached through an accessor written beside it:

```cpp
inline Item Hide() {
    static const Item id = item::Table().IdOf(&kHide);
    return id;
}
```

Cached on the first call, which is after the table is frozen. Call sites read
`items::Hide()` where they used to read `Item::Hide`.

`hide` was added while writing this, to prove the claim: one file, one registrar, and
it appears in the hotbar, the creative palette, the drop tables and the pickup pool
with nothing told about it.

**What is still a central table, and is the next thing to move**: `kElements` in
`world/element.h` (1,900 lines), `flora::kSpecies` in `flora/flora.h` (1,200), and the
older half of the probe dispatch in `probes/probes.cpp`, which is still two walls of
name tests written out twice and has to agree with itself. The registry, the checks
and the per-row file layout are in place for all three; what is left is mechanical.

---

## 27. An ore is drawn in the rock it is in, and the rock is never named

An ore used to be painted as a filled region of its own colour: a compact disc of
pure black or pure copper set into the hillside, with a smooth curve round it. It
read as a sticker rather than as a find. It is now a scatter of blotches through
whatever it displaced, which is Minecraft's arrangement and is also what an ore
actually is.

**Nothing about the block changed.** It occupies the same vertices, stops the same
bodies, takes the same time to break and yields the same item. `--sun`, `--column`,
`--ore` and `--covers` are byte-identical across the change, and a `--probe` picture
of open country is too. Only the paint moved.

### 27.1 The host is the pass underneath, so it never has to be written down

This is the whole design and it is one sentence: `soil::Paint` answers `BLANK` for a
texel the vein does not claim, `DrawPainted` submits nothing for a colour with no
alpha, and what stays on the screen is what the pass beneath already put there.

That pass is the material under this one in the exclusion order, painted from the
union of itself and everything that outranks it — §5.5 and §12.1, which is why the
union exists in the first place. So by the time coal is asked about, the whole of
coal's outline has already been painted stone.

**Which answers the question this was really asked for**: when there are deeper,
harder rocks with a look of their own, a vein inside one is drawn inside it with no
row edited and no code changed. It is not that the ore looks its host up — there is
no such lookup anywhere, and adding one would be the mistake. The host paints itself
first, and the ore is a hole in the sheet laid over it.

The only obligation a future rock has is the one every ground material already
meets: `occupies`, `blocksBodies`, and a `precedence` below the ores.

### 27.2 A vein must not be drawn from its silhouette

`DrawnUnioned` in `element.h`, and this is the one place the change is not merely
paint.

A material's silhouette is the union of itself and **everything that outranks it,
whether or not it is anywhere near** — that is what a union is. Coal is precedence
1, so coal's silhouette contains every copper, iron, gold, diamond and emerald vein
in the chunk, and the whole of the soil, sand and snow above them. Today that is
invisible because each later pass paints solidly over the last.

The moment a pass stops filling its outline, it is the entire picture: every diamond
in the world would be speckled with coal, copper, iron and gold. So a vein is drawn
from **its own field**, which the exclusion pass has already cut back under anything
that outranks it.

Nothing is lost by dropping the union there, because an inclusion never provided
coverage in the first place. And it is *cheaper*: six ore ranks no longer build a
silhouette, which is six full-chunk grids each taking the maximum over fourteen
fields per vertex, against a walk that skips an empty cell after four reads.
**`PaintChunks` went 0.95 ms to 0.64 ms**, with `world.Update` flat at 0.82 either
side as the thermometer §4 asks for.

### 27.3 The blotches are a hash on a lattice, not a noise field

Because the share has to *be* the share. A hash is uniform, so a cut at 0.45 keeps
45% of the blotches and the one number the row is written in means what it says.
Perlin is crowded hard around its own midpoint — §17.5's trap met from the other
side — so the same cut on a field would keep some quite different fraction.

The lattice is two texels and world-anchored, so the pattern belongs to the rock and
does not crawl as the view scrolls. Blotches that land beside each other run
together, which is where the larger clumps come from; `share` is read against the
percolation threshold of a square lattice, **0.593** — below it the blotches are
islands and a seam reads as flecks, above it they join up and it reads as a mass of
ore with rock coming through.

### 27.4 The fringe is a share of the vein, and that was measured

`ElementVein::fringe` thins the blotches out towards the vein's face. Without it the
blotches stop along the very curve the solid disc was drawn to, and the eye
reassembles the disc — a stippled disc is still a disc.

It was written as six pixels first. That looked right on coal, which is forty-eight
across, and swallowed gold, diamond and emerald whole: at eighteen to twenty-one
pixels the same six was 88% of the vein, the density never once reached the share
the row asked for, and what came out was three or four lonely texels. **A fringe is
a proportion of a shape and not a distance.** `soil::For` turns the share into the
pixels the painter measures depth in, off `ElementSpawn::veinCells`, so the two
cannot drift apart.

The two units deliberately have two names: `fringe` on the row is a share, `rim` in
the painter is pixels. One name for both would be §11.2's `std::optional` fault in
another form.

### 27.5 `--veins`, and two things it taught in its first three runs

`--veins out.png [zoom]` draws every inclusion at the width the generator actually
makes it, twice: buried in rock, and standing out of a cave wall. The second is the
one the player walks past, because `wallBias` puts ore there deliberately, and it is
the one where two dark things can collapse into one smudge. It exists because the
ores it is most needed for **cannot be found** — an `--ore` sweep of six thousand
pixels of diamond's own level reports none at all.

It then measures the same question against the world, through `soil::Shows` and
`marching_squares::DrawPainted` themselves rather than a copy of either: a painter
that records what it was handed and answers with nothing is the exact reading the
real paint gets. Both lessons below came out of that number disagreeing with the
picture.

- **`Texel::depth` is not the distance to the edge of a vein.** It is the field's
  value over the length of its gradient, which is a real distance where a field is a
  distance and a linearisation where it is a shape. An ore's noise is a shape. So a
  vein forty-eight pixels wide hands the painter a depth of forty in open rock and a
  depth of three where the exclusion clamp against a neighbouring ore has made its
  edge steep, and no rim written in pixels can be reasoned about without the
  measurement. The `depth p90` column is that number.

- **Print the count, or the tuning is against noise.** §23.1's lesson again, and it
  cost the same hour. A fixed four-thousand-pixel sweep is five thousand texels of
  coal and *thirty-eight* of diamond — one lucky vein, whose figures moved whenever
  the sweep moved, and against which gold had already been "corrected" by twenty
  points. `--veins` now walks until it has two thousand texels or sixty thousand
  pixels, and prints both the count and the distance. Emerald needs the whole sixty
  thousand to find two hundred and sixty, which is itself the interesting fact about
  emerald.

What it settles at: **drawn 54% for coal down to 39% for diamond**, from a table
whose shares only descend from 62% to 50%. The rest is geometry — a narrow vein is
mostly near its own edge — and it is the right way round, since a rich seam ought to
look rich and a gem ought to look like a gem.

### 27.6 One thing left latent

`config::kDrawContours` is off, so `Outline()` returns `BLANK` and no outline is
drawn. Were it ever switched on, `DrawPainted` gives the outline colour to every
edge square **before** asking the painter — so a vein would get its disc drawn back
round it as a line, with the blotches inside. The fix, if that day comes, is for the
edge case to ask the painter first and take the outline only where it answered.

---

## 28. Making things, and the three states that are the whole mechanic

A recipe is a bill of materials — what goes in, how much, what comes out — and the
interface for it is a strip of icons down the left edge with a card beside the one
being looked at. Two recipes today: a stick from wood, and a stone axe from cobble
and sticks.

### 28.1 Three states, decided in one place and drawn in another

| the player holds | `craft::Standing` | what the strip does |
|---|---|---|
| none of some ingredient | `Absent` | the recipe is not there at all |
| some of every ingredient, not enough of one | `Short` | shaded icon, dead button reading NOT ENOUGH |
| enough of everything | `Ready` | lit icon, amber BUILD |

The middle one is the point of the design. A recipe you have *started* is a recipe
worth showing, because the strip is an answer to "what is what I am carrying worth"
— and a wall of recipes needing materials that do not exist in your world yet is not
that answer. So the axe appears the moment you hold one stick and one cobble, and it
tells you it wants three and two.

**`craft::StandingOf` decides it and `Crafting::Draw` only draws it**, and the click
goes through `craft::Make`, which asks the same function again. A guard that lived
in the interface would be a guard a second caller could walk past — the same
argument §10.5 makes about `World::PlaceCell` refusing a cell rather than the hand
refusing to ask for one.

### 28.2 The strip is Don't Starve's, and that is why it is not a page of the pack

There, crafting is simply *there* while you play: you gather, and what you can now
make appears at the edge of your eye without being asked for. Minecraft's is the
opposite idea — a grid you go and stand in front of, which is a place rather than a
readout. This game already has the second thing, the pack behind Tab. Putting
recipes in it would mean a player only ever learns what they can make when they go
looking, which is exactly the discovery the strip exists to give away.

**What the wiki settled and what it did not**, since it was looked up and it is thin.
It gives the nineteen tabs, that a tab shows "what items need to be unlocked and
which resources are needed to unlock them", and that the button reads *Prototype*
when the ingredients are not there. Four pages of it — the single-player Crafting
article, the DST one, the Getting Started guide and a Klei thread — describe the
*organisation* of recipes and nowhere describe the shading of an unaffordable one,
the ingredient counts, or the state of the build button in prose. So the three states
above are the game as it is played rather than as it is documented, and they are the
three the mechanic was asked for. Anything claiming to be a wiki citation for the
*look* of it would be invented.

One deliberate departure: the card prints **held over wanted**, `1/3`, where Don't
Starve prints the required count alone. A cost alone answers "what does this take"
and leaves "how much more" to be worked out against a bag that is not on screen.

### 28.3 A recipe is a row, and it names the other tables by name

`src/craft/recipes/<name>.{h,cpp}`, registered by a registrar beside it, in §19's
arrangement. Nothing else is edited to add one — not a table, not an enum, not the
build.

`makes` and every `Need::what` are `const char *`, because a `constexpr` row cannot
hold an id that is not handed out until `content::Open` freezes the tables. §19.3's
rule, met from a third direction. What is new here is that a name may be a row of
**either** content table — "wood" is an item and "cobblestone" is a material, and a
player carries both in the same nine slots — so `craft::Named` walks the items and
then the elements.

That lookup's *order* would be a decision nobody wrote down if a name were ever in
both tables, so `craft::Ambiguous` exists and `recipe_checks.cpp` refuses to start on
one. The same file also refuses a recipe that names something missing, one that
yields nothing, one that asks for nothing, and one that asks for the same ingredient
twice — which would be counted twice and spent twice while reading as asking once.

It was checked the only way a guard can be: by breaking a row and watching it fire.

```
content: recipe 'stone axe' needs 'cobblestome', and there is no such row
content: 1 fault — refusing to start
```

### 28.4 The stick's arithmetic, which is the only number that had to be adapted

Minecraft: two planks make four sticks, and a log makes four planks — so a log is
worth eight sticks, and the stick recipe by itself consumes *half* a log.

This world has no plank item. `wood plank` is a material, laid by the cell, and
nothing crafts it. Half an item cannot be spent, so the chain is scaled up by two and
written as one row: **one wood, eight sticks.** That is Minecraft's rate exactly
rather than an invention, which is the sense in which the quantities are "the same"
once they are adapted to the items this game actually has.

The axe needed nothing done to it — three cobblestone and two sticks, unchanged,
because both ingredients already exist as things the player holds and both are
counted one to one. **It is only where a step of the chain is missing that the
arithmetic has to be redone**, and that is worth knowing before the next recipe.

### 28.5 What the axe does not do yet

Nothing. `ToolInHand()` still answers `Tool::Hand` and `ToolSpeed` still answers 1 —
§13.2b names those two lines as the whole of what changes when there is a real tool,
and changing them is a change to how *breaking* works rather than to how crafting
does. Doing it here would have meant the item could not be the one file an item
costs.

### 28.6 "The pointer is on a panel" is now one fact

`editor.cpp` asked `hotbar::Contains` itself, which was right exactly while the bar
was the only thing drawn over the world. It is not any more, and two files each
deciding what counts as the interface is two of them disagreeing the first time
either moves — §25.1's fault in miniature. `Editor::Update` takes `overUi` now, and
main works it out once from both panels.

Asked afresh at each use and never remembered from the top of the frame, because the
panels move *during* it: crafting something takes a recipe off the strip, and a
remembered answer is a hit test against a strip that is no longer that tall. Same
rule §14 gives every menu screen.

`src/ui/skin.h` came out of the same tidy. The amber a stack's count is drawn in was
written out in `hotbar.cpp` and again in `inventory.cpp`, with nothing saying they
had to agree; a third panel would have made three. One header of colours, and the
two existing panels were moved onto it — a change with one visible consequence,
which is that a tooltip's text went from `RAYWHITE` to the interface's own slightly
cooler off-white.

### 28.7 `--craft`, and the two things it asserts

```bash
./build/CppGame.exe --craft out.png [wide] [tall]
```

Three panels side by side, one per state, each drawn by `Crafting::Draw` itself
against an inventory made up on the spot. It exists because **no single screenshot
of a session shows the mechanic**: getting into all three states means gathering and
spending particular materials in a particular order, which is exactly the check
nobody runs.

It reports a verdict rather than only a picture, and both halves have been wrong in
this project under other names:

- **No two rows of the card land in the same pixels.** §25.1 is the long version —
  three things laid out separately in one column is how the hearts, the brush badge
  and the held item's name came to share twenty pixels. The gaps are printed as
  numbers, because "the information is too close together" is a claim about pixels
  and a picture cannot be measured with a ruler.
- **The strip lists exactly the recipes `craft::StandingOf` says are started.** A
  panel that drew beautifully while every recipe read as ready would pass a picture
  and fail a player.

Two faults it caught on its first two renders, both of which look like nothing in
code and are obvious in the sheet: a blurb one word longer than the card ran off the
right-hand border and over it, and the shading over an unaffordable recipe was heavy
enough that the silhouette it was supposed to be teaching had gone. The first is why
the card wraps rather than trusting the table to be brief; the second is why the
scrim is just over half and not two thirds.

---

## 29. A tool is a drawing, a lifetime and a pose

Fifteen tool sprites arrived at once — pickaxe, axe and shovel in wood, copper, iron,
gold and diamond — and they turned out to be three separate pieces of work wearing one
coat. Each is worth writing down on its own.

### 29.1 The file keeps its size; only the scale it is drawn at moves

`ItemDef::art` names a path under `assets/`, without the extension, and `item/icon.h`
is the one answer to "draw this stack". Every caller that used to reach for
`DrawPicture(PictureOf(stack), ...)` — the bar, the panel, the cursor carrying a stack,
the recipe card, a pickup lying in the grass — goes through `icon::Draw` now. Five
callers each deciding whether a row has a file is five places for one material to be
drawn one way in the bar and another on the ground: §25.1's complaint about the foot of
the screen, one layer in.

**Both kinds fill the same box.** The file is 64 pixels square and a `Picture` is six
texels, and neither number reaches a caller: what is drawn is `kPictureSide * pixel` on
a side either way. That is what makes the art a drop-in — a slot does not change size
because somebody drew a pickaxe.

**The whole canvas, never the drawing's own bounding box.** A pickaxe fills 48 pixels of
its 64, an axe 29 and a shovel 28, and fitting each to what it happens to occupy would
draw the shovel half again as wide as the pickaxe it hangs beside. One window for every
tool is §24.1's argument about one window per creature met from the other side, and it
is what keeps fifteen separate files reading as one set.

**It is the one thing in this project that is not point sampled**, and that is a
deliberate exception rather than an oversight. Everything else drawn from a file is
drawn at one texel per world pixel, where nearest sampling is not an approximation but
the exact answer (§5.5). This is the opposite case: 64 pixels have to become 36, so
better than half the source is not going to survive whatever is done to it. Nearest
picks which pixels live by where the grid falls, which eats the one-pixel black outline
the art is drawn with in some columns and keeps it in others. A mip chain averages,
which is what a smaller copy of a drawing *is*. **The assets are untouched** — nothing
is resaved smaller, and `GenTextureMipmaps` is called once on load.

**A row names a file, where a creature names a folder.** §24.2's rule is what it is
because a creature has three clips inside its folder; an item has exactly one picture,
so the file is the thing and a directory per pickaxe would say nothing. The price of a
path in a table is a path that can be misspelt, so `item_checks.cpp` opens every one at
startup and refuses to start on a file that is not there. Without that the failure is
silent in the worst way: the item is quietly drawn from the hand-drawn `picture` beside
it, which looks like a perfectly good picture (§16.2b).

### 29.2 A tier is a rate *and* a lifetime, and gold is the proof

`tool::Kit::lasts`, beside the speed rather than on a table of its own. Minecraft's own
durability figures — wood 59, stone 131, iron 250, gold 32, diamond 1561.

It is beside the speed because that is the one place the ladder stops being monotone:
gold is the fastest material in the game and lasts a third as long as wood. Read the
speeds alone and gold is simply the best tool there is, which is not the trade Minecraft
offers and not the one these rows describe. Two tables would be two things to keep in
step; on one row they cannot disagree, and `--tools` prints both.

**Copper is not Minecraft's**, because Minecraft has no copper tool. It is here because
there is copper in the ground already and there is now a head drawn for it, and the only
honest place to put it is between the two it sits between in the world: speed 5 against
stone's 4 and iron's 6, and 190 blows against 131 and 250. **Its damage is stone's
exactly**, and that is a decision rather than an oversight — a point of damage is half a
heart and there is no half step to give it, so what copper buys is the rate and the
lifetime.

**The ore itself and not an ingot.** Smelting is a furnace, a fuel and a fire, and there
is none of the three here yet, so a metal tool is made of what came out of the ground —
exactly as a stone one is made of the cobble that came out of the rock rather than of
anything done to it afterwards. The day there is a furnace, twelve recipe rows gain an
ingot and nothing else moves.

### 29.3 Wear is on the slot, and one function moves a stack

`Stack::wear`. On the slot and not on the row, and that is the whole difference between
a tier and a tool: `lasts` is shared by every diamond pickaxe there has ever been, and
this says what has happened to the one in this slot. It costs nothing — `holds` and
`what` are a byte each and `count` wants four, so there were already two bytes of
padding between them and `sizeof(Stack)` has not moved.

**It caught four call sites the moment it was added**, and this is the lesson. `Fill`,
`Take`, `Put` and the panel's put-one-down all rebuilt a stack field by field:

```cpp
into = {.holds = stack.holds, .what = stack.what, .count = fits};
```

Every one of them silently dropped the new field, and what that *is* is a worn pickaxe
repaired by being dropped on the ground and picked back up. They all go through
`Stack::Some(count)` now — one choke point, so the next field added to `Stack` is
impossible to forget.

**Only the inventory may empty a slot.** `Inventory::WearHeld` is where a tool is used
up and where the slot is cleared, for `World::PlaceCell`'s reason (§10.5): there are
three things that wear a tool — a block broken, a tree struck, a creature hit — and they
must not be three answers to what happens on the last blow.

**A tool that wears may not stack**, checked at startup. `wear` is one number per slot,
so two tools in one slot would share one lifetime: use one and the other is worn, break
the pair and both go. Nothing else would catch it — `Stack::Room` is what stops two
tools pouring into one another, and it stops it only because the limit is one.

**What is charged, and where.** One point per cell broken, which is Minecraft's rate — a
block broken costs a tool one of its lifetime whether or not that tool was the right one
for it. Per *cell* and not per stroke, so a wide brush wears a pickaxe as fast as it
digs: the same invariant §10.6 keeps about the economy, or `Editor::span_` becomes a
power setting rather than a choice. Charged where the block actually comes away and not
where the bite starts, because a bite is given back when the aim wanders off (§13.4) and
a tool worn by work that was handed back is a tool worn by nothing.

**And a swing has to land.** `Grove::Strike` returns whether it hit anything now, for
exactly this: a held button over a trunk that has already gone over is a blow every
frame, and an axe worn out by swinging at the air where a tree used to be is a fault
nobody would think to look for.

**Scale is deliberately uncorrected, the same way §13.1's hardness table is.** A cell is
about a quarter of a Minecraft block by area, so a wooden pickaxe's 59 blows clear about a
quarter of the volume Minecraft's would. If the hardness table is ever divided by four,
this is the second table that goes with it, and `--tools` prints both so they can be read
together.

**The bar is drawn even at full**, where Minecraft hides it until the tool is damaged.
The difference is what the two are for: there it is a warning, and here it is also the
answer to "does this thing run out", which a player holding their first pickaxe has no
other way to find. §25.2's lesson about the hearts — a readout that appears for the
first time during the emergency is one the player has to find while it matters. The
tooltip carries the number beside it, because a bar cannot be compared between two
pickaxes and a number is not readable while digging.

### 29.4 The tool in the hand is the arm's own arithmetic

`Player::DrawHeld`. The art is drawn upright — head at the top of the canvas, butt of
the haft at the bottom — so what has to happen is that the canvas's own up becomes the
direction the arm is pointing. Up is `(0,-1)` and a turn of `t` clockwise takes it to
`(sin t, -cos t)`, so the angle wanted is `atan2(arm.x, -arm.y)`; and it rotates about
the grip rather than the centre, so the hand stays on the haft through the whole swing.

**Mirrored across the haft and not across the screen.** An axe is all on one side of its
own handle; without the mirror the blade is above the shaft aiming one way and below it
aiming the other. Because the sprite is already rotated to lie along the arm, a flip in
sprite x is a reflection that keeps the head at the far end and only swaps which side
the blade is on — which is exactly what is wanted.

**`player_config::kHeldTool` is 22 and lives with the character**, not in `player.cpp`,
because it is half of how far the character reaches: `kAttackReach` puts the hand out
and this is what sticks out past it. Anything framing the character needs both, and
§29.5 is what happens when something has only one of them.

**The grip is 0.72, and it was settled by looking.** At 0.86 there were three pixels of
handle behind the fist and the whole tool read as floating off the end of the arm rather
than as being carried by it.

### 29.5 `--gear`, and the cell that was too small

```bash
./build/CppGame.exe --gear out.png [zoom]
```

Three bands: every item as a slot draws it, one pickaxe at six amounts of wear, and the
character holding a tool at six aims. Three verdicts, and each is about a fault a
picture of the other two would not show.

- **Every row that names a file has one that loaded.**
- **The bar is shorter at every step and never nothing.** Measured off the exported
  image rather than recomputed from `Stack::Whole`, because what is being checked is the
  *drawing*: arithmetic that is right and a bar drawn at a fixed width would pass any
  check of the arithmetic. Never nothing is the other half — a tool with one blow left
  and a bar rounded away is a tool the player believes is gone, and they put it down.
- **The tool is drawn in the character's hand.** §24.4's argument. Everything else on
  the sheet draws an icon directly, which proves the files are right without proving the
  game will ever show them — what puts a tool in the hand is `Player::Draw`, and a sheet
  that reproduced the pose would go on looking perfect while the game drew nothing at
  all. Measured as the pixels between two identical poses, one holding a pickaxe and one
  not, aimed through `Player::Update` so the mirroring is exercised rather than assumed.

**The fault it found on its first render was its own.** The pose cells were a fixed 120
pixels and the zoom was 4, so the arm ran off the edge of its own box and the tool was
drawn entirely outside it — which looks exactly like a tool that is not drawn at all,
and was read that way for a while. The cell is worked out from
`kAttackReach + kHeldTool` now. **A probe whose frame is chosen rather than derived is a
probe that can report the absence of something it merely cropped.**

`Inventory::PageOf` gained a `wanted` count in the same commit, and
`inventory_checks.cpp` counts pages rather than the whole item table. It used to weigh
every item against one page, which was a fair proxy at nineteen items; twelve tools took
the table to thirty-one and failed a build in which no page was three quarters full. **A
check that refuses a correct change is a check that gets deleted**, and the next silent
overflow after that goes unnoticed.

### 29.6 What has no art yet

Five rows still draw from the four tones on the row: `stone axe`, `stone pickaxe`,
`stone shovel`, `wood sword` and `stone sword`. Nothing is wrong with them and the
fallback is the design — but they are visibly a different hand beside the fifteen that
are drawn, and the fix is a file and one field each. `--gear` is where the difference is
seen in one glance.

---

## 30. A chest is a fixture that remembers

Seven rules were asked for: a chest keeps items, three of them join into one store,
each holds thirty-two, a button sorts, a second button says how, breaking one drops
only its own, and the right button opens it in front of everything else. They came out
as two fields on a table, one file of pouring rules moved, and one layout — which is the
sense in which this was a small change, and the sense in which it was not.

### 30.1 It is a fixture, and the table was already the right shape

A chest is put in a cell by a player. It stands on a surface. It comes down when that
surface is dug away. It is made from an item and taken back as one. It has a picture of
a *thing* rather than of a material's face. Every one of those sentences was already
written in `entity/fixture.h` for the torch, and §11.3 is the argument for why that
table exists at all.

So what a chest adds is two fields — `Def::slots` and `Def::joins` — and one member on
`Placed`, which is the contents. It is not a fourth table, it is not an entity, and
nothing in the world knows it is different from a torch except the two places that read
those fields.

**The one thing it did break was already broken.** `Fixtures::Remove` handed back a
`Kind`, and both callers then wrote `items::Torch()` into a `Scatter`. That was correct
for exactly as long as there was one row in the table, and a chest dug up would have
paid out a torch — §16.2b's fault, sitting in two files at once with the fuse lit.
`fixture::ItemOf` is the one answer now, and `Remove` hands back the whole record
because a store that is taken down has to pay out what was in it.

### 30.2 A bank is derived, and the fourth chest is refused

`Fixtures::Run` is the maximal horizontal run of same-kind neighbours through a cell,
capped by the row's `joins`. Nothing is written down: no group id, no membership, no
"which of these belongs to which".

That is the whole of why it can be trusted, and the alternative is where the bug would
have been. Stored membership has to answer *what happens when the middle of a run of
three is dug out*, and every answer to that is two chests sharing one bag across a hole
in the wall. A maximal run of neighbours has no such question in it: dig the middle one
and there are two stores of one, each still holding what it held.

**A placement that would make a run of four is refused**, with a notice, rather than
placed and quietly left out of the bank. Minecraft does the second — a chest beside a
double chest simply stays single — and it can afford to because its bank is two. At
three, "which three of these four are one store" would have exactly one honest answer,
which is *the order they happened to be built in*, and there is nowhere to write that
down that survives one of them being dug up. A run that cannot grow says so, and the
player leaves a gap.

**Horizontal, and that is the art's doing as much as the code's.** A chest is drawn
front-on (§30.10) and joins along its front. A stack of them going up a wall is a wall
of separate chests, which is how a store room is built anyway.

### 30.3 Thirty-two is four rows of eight, and the eight is load-bearing

`slots::kAcross` is 8 and it lives beside the pouring rules rather than in the panel
that draws the grid, which looks like the wrong home for a drawing constant and is not.

A sorting rule is about a **row**. A row has to belong to exactly one chest, or the rule
would have nowhere to live that survives the bank being taken apart — the same argument
§30.2 makes about membership, one level down. Thirty-two over eight is four rows to a
unit, so row `r` belongs to chest `r / 4` and `Fixtures::Rules` can hand out a pointer
into the very chest whose slots that row is drawn from. Dig up the middle chest of
three and the other two keep their rules, still about the rows they were always about.

`fixture_checks.cpp` refuses to start on a row whose `slots` is not a whole number of
rows of eight, and on one that joins more units than a `slots::Bank` has parts. Both are
silent failures otherwise: the first draws two slots past the end of the grid that
nothing can reach, and the second loses the last chest of a run with the player's things
in it.

Thirty-two rather than Minecraft's twenty-seven because a cell here is about a quarter
of a Minecraft block by area (§13.1) and a bag fills several times as fast for it. Three
joined is ninety-six, which is two and a half times what the player can carry — the
point at which a store stops being somewhere to put the overflow and starts being
somewhere to keep things.

### 30.4 The pouring rules left the inventory, because there are two containers now

`item/slots.h`. `Fill`, `Add`, `Room`, `Tally`, `Remove`, `Take` and `Put` were
`Inventory`'s and were the whole of what an inventory did: top up an alike stack before
filling an empty one, never merge two holes, take part of a stack away through
`Stack::Some` so its wear goes with it, empty a slot all the way back to nothing rather
than leaving it holding a row with a count of zero.

Every one of those is a rule somebody got wrong once before it was written down, and
§29.3 is the account of the last of them — four call sites rebuilding a `Stack` field by
field, every one of them repairing a worn pickaxe by dropping it on the ground. A second
copy of that list inside a chest is a second set of answers, and the two would part
company the first time either was touched.

**A bank is parts and not a span**, and that is rule six of the chest showing up in the
data structure. Three joined chests are one store to the player and three separate
arrays in memory, because breaking one has to drop *that one's* contents and leave the
rest. Pour them into a single buffer when they join and there is no honest way to take
one back out.

### 30.5 Two panels are one layout, and the slots shrink together

`ui/pack.h` is §25.1 met a second time, and the arithmetic is what forced it.

Three chests is twelve rows. At the bar's own forty-four-pixel slot that is six hundred
and fifty pixels of grid, beside a pack that is four hundred and fifty-six wide, in a
window that opens at 1000 x 600 and may be dragged to 640 x 400. There is no fixed slot
size at which that fits, and a store with a row off the bottom of the screen is things
the player put away and cannot get back — invisible in any screenshot taken at the
developer's window size.

So `pack::Of` derives the texel: the largest **whole** number of screen pixels at which
the whole arrangement fits the frame, walked down from six. Whole always, for
`hotbar.h`'s reason — a picture authored on a six-texel grid drawn at a fraction of a
pixel comes out with its columns alternating two pixels wide and three. A slot is
`pixel * kPictureSide + 8`, which at six is exactly the bar's forty-four, so **a pack
opened with no chest in front of it is laid out precisely as it always was.**

Measured: 1600 x 900 keeps texel 6 at every size; 1000 x 600 falls to 4 for three
chests; 640 x 400 falls to 3, and to 2 for three. Every slot is on screen at all of
them, which is what `--chest` asserts.

Two things are reserved whether or not they are showing — the rules column beside the
grid and the line of words under it — so turning the rules on does not shift the grid
out from under the player's hand. The second of those was found by the probe: the hint
was drawn wherever there happened to be room under the panel, and at 1000 x 600 it
landed on the frame's last pixel row. A line of text placed by the panel that draws it
is a line nothing else knows about.

### 30.6 The right button opens before it places

`Editor::Opening` is worked out every frame beside `buildable_` and `footing_`, so the
cursor promises it before the click — a cool blue outline where every other mark this
cursor makes is warm, because what it promises is not a change to the world at all.

It is tested **before the material in hand**, which is what "priority over everything,
over placing blocks" means and is also the only order that leaves a chest reachable: the
commonest thing to be holding while standing in a store room is a stack to put away, and
a stack to put away is very often a hand full of material. Under any other order,
opening a chest would mean emptying your hand first.

**On the press and never on the hold**, so a wall dragged out along a ledge does not
stop at every chest it passes.

The hand **reports** and the loop acts, which is the arrangement `Editor::Left` already
has with the axe. Opening a panel stops the world (§14), and a gate hidden inside the
thing it gates is a gate nobody finds.

**The lid runs outside that gate.** `Fixtures::Animate` is called beside `world.Update`,
on the frame clock, for the digging bar's reason (§13.4): a lid is a message to the
player about a panel that is open, not a thing happening in the world. Inside the gate
it would be a chest that never finishes opening, because opening it is what stopped the
clock.

### 30.7 Sorting is one order, and a row set aside is a promise

`slots::Sort` pours alike stacks together and lays the rest out by `(holds, what)` —
materials before items, alphabetical within each. Nobody chose alphabetical:
`content::Open` hands out ids by sorting on each row's own name (§19.1), so sorting on
the id *is* sorting by name and there is no second ordering written down anywhere to
fall out of step with the first.

Stacks are **lifted whole and put back**, never rebuilt from a kind and a total. That is
the one subtlety: a stack carries `wear`, and two pickaxes worn differently are two
things. Rebuilding from `(kind, count)` hands the player a pair of brand new ones, which
is §29.3's repair-by-being-dropped in another costume.

A row with a rule holds that kind and nothing else, and keeps its slack — an unruled
material poured into the gap at the end of the cobblestone row is exactly what the row
was reserved to stop. Everything else fills what is left, and takes the slack **only**
where there is nowhere else to go, because the alternative is losing it. That branch is
unreachable in a bank that was not already full to the brim, and "cannot happen" is not
a place to put something the player dug for.

**Rows and not columns**, and the reason is §30.3: a row belongs to one chest and can be
written into it, while a column crosses every unit in the bank and could not be stored
anywhere that survives one of them being dug up.

### 30.8 The search dims and never filters

These squares *are* the storage. Filtering the view would make a click land in a
different slot from the one it appears to be over, which is a whole indirection between
the grid and the chest for the sake of hiding some squares. What a search does here is
say which of them to look at.

**An empty slot is never dimmed.** It cannot answer a search — there is nothing in it to
be the wrong thing — and with ninety-six slots mostly empty, shading them turns the
whole panel dark and buries the handful of squares the search was asked to point at.
That was visible in the first `--chest` sheet and invisible in the code.

### 30.9 The bin, and the one place a tip is shown over a full hand

A click on it cannot be taken back, so it is a dark red well with a paler lid rather
than a slot — a slot that is always empty reads as a slot the player has failed to fill.
The left button destroys the whole stack and the right button one at a time, which makes
it usable for trimming a stack rather than only for losing one.

It is the **one** place this panel breaks its own rule about tooltips. Everywhere else a
tip is hidden while something is on the cursor, because the hand is already over the
slot it is about to go into. Over the bin, that is the only moment the question matters:
a red square is not self-evident, and what the player needs to know is whether letting go
here destroys what they are holding.

### 30.10 The art is front-on, and switching it on is one field

`fixture::Def::art` names a folder under `assets/fixtures/`, and the strips inside are
always called `alone`, `left`, `middle` and `right` — §24.2's rule, so there is no
filename in the table to misspell. Each strip's **frames are the lid opening**: nought
shut, the last one wide open, and the lid is a play-head between them. A one-frame strip
is a chest that never appears to open and is still correct; a missing `left` falls back
to `alone`, so a bank drawn before the ends exist is three identical chests.

**It is drawn front-on and never mirrored.** A fixture has no facing at all —
`sheet::Draw` is always handed `+1` — and that is not a simplification: a chest seen from
the side could not show which of its neighbours it had joined with, which is the whole of
what a bank has to say for itself.

`assets/fixtures/chest/README.md` is the contract, and the row's `art` is still
`nullptr` today so that nothing warns about a file nobody has drawn yet. The hand-drawn
`picture` on the row is what it falls back to, and §24.2's reason for keeping it holds
here too: it is the one description of the chest that cannot go out of date, being in the
same file as everything else about it.

### 30.10b The fallback joins too, and that is `Def::joined`

Reported from play: *combine the chest sprites.* Three chests standing side by side were
drawn as three boxes with two seams down them, and what the player had built was one
store. The piece machinery was already there — `Piece`, `Wardrobe::For` — and it was
being asked for **only after the art check**, so a bank with no authored art fell back to
three copies of one face.

`Def::joined` is three more pictures on the row: the left end, the middle, and the right
end. The units that continue into a neighbour drop the border on that edge, and the lid,
the iron band and the body run straight through the join. `FaceOf` is the one answer to
which face a unit shows, asked by the draw and by the contact sheet alike.

Written out by hand rather than derived from `picture` by a rule, on `Picture::art`'s own
argument: at six texels a side the whole face is thirty-six characters, which is easier to
change by eye than any rule that could have produced it — and a rule that opened one edge
would be wrong the first time a fixture had a shape it did not expect.

**Nothing dark may stand at a seam**, and that is the rule the rest follows from. These
pictures are bordered in their darkest tone, so *any* dark texel where two units meet
reads as a border — which is the fault being fixed. Two things came out of measuring it:

- **The first draft put half a clasp on each inner edge.** It was centred for a run of two
  and symmetrical for a run of three, and it looked exactly like a seam. `--chest` counts
  the border texels left at a join and said four. The clasp moved to the middle of the
  *middle* face, which puts it dead centre of a run of three.
- **And the check was wrong before the art was.** Its first version counted dark texels
  touching at the join, which flagged the iron band — a band that is *meant* to run
  straight through. A border is dark at the edge with something lighter beside it: a
  vertical line rather than part of a horizontal one, and that is what it measures now.

A third thing the sheet showed and no code could: at six texels the single chest's latch,
notched into the body in the darkest tone, read as a **doorway**. It is a pale lock on the
dark band now, which is Minecraft's arrangement and reads as metal on wood.

### 30.11 `--chest`, and the four things it asserts

```bash
./build/CppGame.exe --chest out.png [wide] [tall]
```

It exists for `--craft`'s reason (§28.7): **no single screenshot of a session shows the
mechanic.** Getting a bank of three on screen means felling a wood, making twenty-four
planks, crafting three chests and standing them in a row — and the sizes that break are
exactly the ones nobody reaches by accident. It draws through `Inventory::Draw` and
`Store::Draw` themselves and lays out through `pack::Of`, never a copy of any of them.

| verdict | the fault it is about |
|---|---|
| every slot is inside the frame | a row of a chest off the bottom of the screen, at a window size the developer never used |
| no two parts of the layout touch | §25.1, in a layout with two panels, a bin, a field, three buttons and a column of headers |
| sorting keeps every count | counted **per kind**, because a total is blind to two kinds swapped |
| breaking one takes its own | rule six, through `Place`, `Run`, `Store` and `Remove` rather than by reasoning about them |

`Store::Ruling` and `Store::Seeking` are settable from outside for `Crafting::Open`'s
reason: the panel reaches those states from a click, a probe has no mouse, and a probe
that reproduced them would be photographing the reproduction.

**Both faults it found on its first two renders were its own, and both were worth it.**
The per-unit fill wrote through `Fixtures::Run`, which answers with the *whole* joined
run for any cell of it — so three chests were filled into one, and the break test read
nine where it should have read twenty-seven. And writing straight into a slot goes round
the stack limit `Fill` keeps, so a stack of seven wood shovels went in, the sort
correctly split it back out into sevens of one, and the sheet came out a wall of
shovels. A probe that cannot be read is not an instrument.

---

## 31. A save is the difference between the seed and the world

Five rules were asked for: the player can save, and what is saved is every change to
the world, the pack, every chest, and every creature. They came out as one file format
and six `Save`/`Load` pairs, each beside the fields it writes — plus a saves screen
that had been drawn and refused since the menu was written.

### 31.1 What is written is what cannot be worked out again

The generator is a pure function of position and seed. That is the whole design of the
world (§5.5, §8, §21.2), and it is what makes a save small: a country is its seed plus
the journal of what the player changed; a wood is its scatter plus the trees that have
been touched; a county's animals are a function of the cell plus the record of which
cells have been asked and what became of what was in them.

So the file holds `World::edits_`, `Grove::remembered_`, every fixture and its
contents, the thirty-six slots, the character's position and health, and the warren's
patches. Nothing else. Chunks, silhouettes, the light, the chunk pictures, the water —
every one of those is derived, and **derived state is not history**. Writing it would
make the file large, make it brittle against every change to how a chunk is built, and
make it possible for a save to hold two answers to one question.

**And what is not written is state a fraction of a second old.** A swing halfway
through, a creature mid-leap, a hurt flash, a stack on the cursor, the timers on
`life::Health`. A world you load is a world you *arrive* in, standing still. Every one
of those fields would be the game telling you about a moment you did not see — and it
is the same argument `mob::Life` already makes about a creature (§21.2): what is kept
is history, not description.

Two things are deliberately dropped that look like history and are not: `World::mown_`
and `World::sown_`, which are a tuft growing back and turned earth greening over. They
are timers on the weather clock. A world you come back to has settled, which is what a
world you come back to looks like.

### 31.2 Names, never indices — and the format follows from that

§19.1 is the reason and it says so outright: *a row's id ends up in `Stack::what` and
will end up in save data, so it has to be the same number on every build of the same
content.* Ids are a function of the **set** of rows, so they move the day a material is
added. A binary save keyed on them is a world that quietly turns to stone where the
diamond was.

Every reference in the file is the row's own name — materials, items, fixtures,
species, creatures. From there the rest follows:

- **A record is a line**: a tag, then its fields, with anything that could hold a space
  in quotes ("wood plank" is one). `save::Writer` and `save::Reader` are the whole of
  the syntax and live in `save/record.h`.
- **The contents live with the module that holds them.** `World::Save` writes the
  journal, `Grove::Save` the wood, `Fixtures::Save` what is standing and what is in it,
  `Warren::Save` the patches. Adding a field to `TreeState` means editing the file you
  were already editing. The alternative is one enormous writer that reaches into six
  others' privates, and the first thing it does is make every one of those fields
  public.
- **Sections are self-delimiting**, through one record of pushback (`Reader::Again`),
  and carry no counts. A count on a header is a number that has to agree with the lines
  under it, and the first hand-edited save has it disagree.
- **A record with no case for it is skipped**, which is what makes the format additive:
  a save written by a later build with a section this one has never heard of loads as
  the world without that section. The version number on line one is what says when that
  stops being safe.

**Everything is sorted before it is written** — edits by row then column, patches and
fixtures by cell, trees by key. Not tidiness: a save has to be a function of the
*world* and not of a hash table's iteration order, or two saves of the same country are
two different files and the round trip below has nothing to compare.

**A name that resolves to nothing stops the read.** Not for the settled bits and not
for a creature — those are dropped, deliberately, because §21.1 removed three creature
rows in one commit and refusing there would break every save ever written by a build
that had them. Everywhere else it is a refusal: a hillside missing the one material the
reader did not recognise is a world that is quietly not the saved one.

### 31.3 The round trip is §2's bar, pointed at the one file a player cannot remake

`--saves` writes a world, reads it back, writes it again, and compares the two files
byte for byte. Anything dropped on the way in comes out as a shorter second file;
anything mangled comes out as a different line, and the check names the line.

That is CLAUDE.md §2's discipline applied where it matters most, and it matters most
here for a reason the rest of the project does not have: **a save's faults are
invisible until the player has already lost something.** A chest whose contents are
written but not read looks perfect for the whole session it was filled in.

The `written` record is the one line skipped, because it is seconds since the epoch at
the moment of writing — a fact about the room rather than about the world, and a check
that demanded two writes match on it would fail on being correct.

Four more verdicts beside it, and each is about something the file check cannot see:

| verdict | what it catches |
|---|---|
| the world read back is the world written | a reader that parses every line perfectly and throws the result away |
| a name this build does not know is refused | §19.2's rule — a guard nobody has watched fire is not a guard |
| renaming changes the name and nothing else | which is what makes it safe on a world played for a week |
| both worlds are found by the listing | a head `Read` understands and `Peek` does not — a save that loads and cannot be found |

**It found a real bug on its first run**, and the bug is the shape this section is
about. `Warren::Save` wrote the patches and, for each, the live creatures standing in
it. A creature put down by hand — the console's `/spawn`, or a probe — is in the herd
immediately and its cell has no patch record until the view has left it, so it was
filed under a patch that did not exist and dropped. The counts said `0 patches, 1
creature` on the way in and would have said nothing at all on the way out.

The fix is one merged sorted list of cells rather than two passes, and the merge is
load-bearing: written in two passes, the *second* save would have those cells as real
records and would put them in a different order — so the round trip would have caught
the fix as a new fault.

### 31.4 A write never destroys what it replaces

`save::Write` writes to `world.txt.part` and moves it into place. A save is the one
thing in this program a player cannot make again, and a write that stops halfway — the
window closed, the disk full, the machine off — must not be allowed to leave the old
save destroyed and the new one truncated. The only way to be sure is never to open the
real file until there is a whole one to put in its place. `save::Rename` goes the same
way round for the same reason.

The preview picture is the one part that may fail without the save failing: a world
written without a thumbnail lists perfectly well, and losing a session over one would
be absurd.

### 31.5 Three moments write, and only one of them is a button

- **When a world is made.** Not a convenience: until it is written it is not in the
  list, so a player who makes a world and closes the window has made nothing, and there
  is no moment at which that is what they meant.
- **On the pause screen**, which is the button.
- **On the way out**, which is Minecraft's rule and is there for its reason: there is no
  such thing as quitting a world without saving it, because the alternative is somebody
  who closed the window after an hour and finds an hour ago.

The explicit button is not made redundant by the last of those. That one is *when you
choose*; the exit write is the one nobody has to remember.

**Leaving a world drops both copies of which save is being played** — the menu's and
the loop's — and the second one is not tidiness. Without it, the write on the way out
would put back a world the player had left and then deleted from the list, which is a
save that comes back from the dead.

### 31.6 The preview is the last thing the player looked at

Taken with `LoadImageFromScreen` after `EndDrawing`, on the frame the game is paused,
and never on a timer. The world is still drawn on that frame — the gate that stops it
was tested at the top — so it is the last frame there is anything to photograph, and
the only way to reach "save" is through the pause screen, so it is always the picture
they were looking at when they stopped.

After `EndDrawing` and not inside it, because what is wanted is the whole frame as the
player saw it — the world, the character, the bar, the hearts. Shrunk to 480 px on the
way in rather than on the way out: it is drawn at a third of a menu's width and read on
every listing, and a full-screen PNG per world is several megabytes to hold a thumbnail.

### 31.7 Loading is making a world with one stage in the middle of it

The staged rebuild §14.1b describes gained one stage: after the ground is measured and
everything that remembers the last country is cleared, and *before* the character is
stood up. That ordering is the only one that works — the journal has to land in a world
whose own edits have just been dropped, and the save is what says where the player was
standing, so a loaded world skips the "find you somewhere to stand" stage entirely.

Making and loading are one path because every other stage is the same work in the same
order: calibrate, measure, clear, stand up, stream, paint. Two copies would drift the
first time one gained a stage.

**A save this build cannot read is played anyway**, on the country the seed describes,
with a line in the console saying so. The ground is still the player's; a silent empty
world would look like the save was never written.

### 31.8 The saves screen, and the one place this module breaks its own rule

The screen was drawn and refused — `load`, `edit` and `delete` were there and dead,
because "the store is a system of its own and is deliberately not being invented here".
It is invented now: a list of what is on disk, newest first, with the name on top and
the mode, the seed and how long ago it was played under it; a preview; and four buttons
that all work.

Three things about it are decisions rather than layout:

- **Renaming happens in the row**, not on a screen of its own. What is being renamed is
  the row the player is looking at, and moving them somewhere else to type would take it
  off the screen.
- **Deleting asks first, and the question is a field rather than a screen.** That is the
  exception to §14's stack: a confirm is not a place the player has *gone* — it is a
  question about the screen they are on — and pushing it would make the back arrow mean
  "cancel" on one screen and "go up" on every other.
- **The pause screen is the title screen.** The same four buttons meaning four
  different things, because it *is* the same screen: the title standing on a world
  rather than on nothing, which is exactly what the stack already says (`Paused`). Two
  layouts would be two things to keep the same size and a second place to add a button
  to.

The list is selected **by id and never by position**. A save that was deleted moves
every row under it up one, and a remembered position would quietly select whatever slid
into it — which is the row the next click deletes.

**The picture box is the shape of the screen the picture was taken of.** It was a fixed
share of the panel's height, and a screenshot is the shape of the window — so a preview
fitted into it came out as a strip floating in a hole, with a band of empty panel above
and below. Taken from *this* window rather than from the picture, so the layout stays a
pure function of the frame and does not move when the selection does; a save written at
another shape is still fitted inside and centred, which is the rare case rather than
every case. `--menu` measures the share of the box a picture of the window's shape
covers, and it is a hundred per cent or the check fails.

### 31.9 `--menu`, and why the interesting states cannot be played to

```bash
./build/CppGame.exe --menu out.png [wide] [tall]
```

Six screens on one sheet: the title, the saves list with a world picked, a world being
renamed in place, the delete question standing over the list, the new-world form, and
the pause screen. It exists for `--craft`'s reason (§28.7) and `--hud`'s (§25.5): **an
empty saves list is what a developer sees on every run.** A list with six worlds in it,
one of them mid-rename, takes a session of play to reach and is exactly where a layout
goes wrong.

The saves it lists are **real files**, written and swept up afterwards — a mock list
would be a picture of the mock, and the one thing worth checking is that what
`save::List` hands over is what gets drawn. They hold a head and nothing else, because
the head is all this screen ever reads.

`Menu::Renaming` and `Menu::Asking` are settable from outside for `Crafting::Open`'s
reason: those states are reached by a click, a probe has no mouse, and a probe that
reproduced them would be photographing the reproduction.

Two faults it caught on its first render, both invisible in code: the title screen wore
a back arrow pointing at itself (the probe's own doing — it pushed a screen that was
already on top, which is worth knowing because the game does not), and every row of the
list said "1009 days ago" because the fixture used a fixed timestamp. The second is the
better lesson: a probe whose data is constant photographs the probe rather than the
screen. The rows now spread through the past so all four of the phrasings appear at
once — which is how "1 hours ago" was found and made singular.

A third came from running it beside a world somebody was actually playing: the count read
**"7 of 6"**, because `Menu::Listed` counts everything on the disk and the disk had a real
save on it. The probe counts only its own prefix now. A check that counts other people's
things is a check that fails for the wrong reason — and the two sweeps in this project
that remove save folders are written against their own prefixes for the same reason, so
that neither can ever take a player's world with it.
