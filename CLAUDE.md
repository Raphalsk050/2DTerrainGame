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
| `--wind`, `--caves`, `--ore`, `--covers`, `--settle`, `--column`, `--tones` | generator reports |

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
| light solve | 9.5 | 47% of the frame; already parallel |
| `DrawScene` itself | 2.8 | sky, clouds, rain, stars, player |
| `lights.Update` | 1.6 | texture upload |
| `world.Update` | 1.0 | streaming and the grass band |
| `StepWater` | 1.0 | |
| `DrawHud` | 0.7 | |
| `DrawTerrain` | 0.6 | a blit |
| **frame** | **20.4** | 49 fps, from 131 ms / 8 fps |

What is left is the light. Porting the cascades to fragment shaders is the next
real step; the two things that make it awkward are that `spread` is a distance
transform (on the GPU it becomes jump flooding, which is an *approximation* and
would be the first thing to leave the byte-identical bar) and that
`World::LightLevelAt` is read on the CPU by plant growth (`grove.cpp`) and the
HUD, which would need a readback.

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

### 5.7 Split parallel work by cell, not by column

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
