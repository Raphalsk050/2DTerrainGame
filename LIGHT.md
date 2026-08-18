# The light — where it stands

The light solver was replaced. This is what it is now, what was measured, and what
is still open. Written to be read at the start of a session, before touching
anything.

**`CLAUDE.md` is stale about the light.** §4 (where the frame goes), §5.2 (what is
parallel) and §5.4 (the caches) describe a solver that no longer exists. Everything
in them about `spread`, `Skyline`, the light medium fill and the cascade stack is
wrong now. The rest of that file — building, the build grid, the materials, the
water, the drops — is untouched and still holds.

---

## 1. What it is

**Holographic Radiance Cascades**, Freeman, Sannikov & Margel 2025
(arXiv:2505.02041), running entirely on the GPU as OpenGL 4.3 compute shaders.

| file | what |
|---|---|
| `src/light.h` | `Medium`, `Sky`, `Settings`, `Field`. Namespace kept as `light`, so every caller compiled unchanged |
| `src/light.cpp` | host side: SSBO pyramid, dispatch order, readback |
| `src/light_shaders.h` | the seven compute kernels, as string literals |
| `tools/light_scenes.cpp` | the test bank |
| `tools/lightscenes.bat` / `.sh` | build and run it |

`CMakeLists.txt` forces `OPENGL_VERSION "4.3"` before `FetchContent_MakeAvailable`.
Without it rlgl compiles the compute and SSBO calls to empty stubs that return zero —
no link error, just a solver that silently does nothing.

**Deleted, and not replaced by anything:** `light_layer.cpp/h`, and with the old
solver went `Spread`, `surfaceReach`, `surfaceLip`, `sunDepth`, `sunBlend`,
`coarseOcclusion`, `skyline`, `ReachesSky` and the mip pyramid. Each of those existed
to compensate for a premise the new architecture does not have.

`Medium::cover` went later and for a different reason — it was the canopy's per-column
share of held-back sky, and it was the flicker. See §4b.

### Three departures from the paper, each deliberate

1. **Twice the directions per level.** The paper's `v_n(k) = (2^n, 2k − 2^n)` has an
   always-even y component for n ≥ 1, and then §5.0.3 observes that this is exactly
   why probes of odd and even row never interact — the checkerboard it then treats
   with a blur. The authors' own Rust reference (`entropylost/amitabha`) carries
   `2 << level` directions instead, which couples the parities at the source. Followed
   the reference. Measured: no checkerboard (see §3).

2. **`R_N` computed, not zeroed.** The paper says treat the top cascade as uniformly
   zero. That is right about lights and wrong about the boundary: at level N−1 the
   even-column case reads `R_N` at its own position, which is inside the grid, and
   zeroing halves that estimate along the near edge of every quadrant. `kBoundary`
   computes it from the ray `T_N` already holds.

3. **The sky is the boundary condition**, read in the direction a ray leaves by
   (the paper's footnote 1). There is no skyline and no per-column daylight depth: a
   ray that starts underground is stopped by the rock it is in. `SkyRadiance` takes a
   direction and nothing else — it took a position too, for the cover, and §4b is why
   it no longer does.

### Multi-bounce

`Q = emitted + sigma · albedo · F_prev`, fed back temporally, one bounce per frame.
Albedo comes from each material's own `ElementPaint::tone[2]` — nothing was authored
into `kElements`.

Two traps already paid for, both documented at the code:

- **No second division by 2π.** `Restrict()` already normalises every cone by the
  turn, so the solver's output is a *mean radiance*, not an integral. Dividing again
  made every surface 2π too dark — a sunlit hillside came out black under a correct
  sky.
- **A dense cell takes its incident light from its open neighbours.** A probe inside
  a solid cannot see out of its own cell (at σ=32 the ray dies in half a cell), so a
  surface read zero incident light and reflected nothing. One neighbourhood, no
  tuning — that is the difference between it and the `Spread` sweep it replaces.

---

## 2. The tools

```bash
tools\lightscenes.bat                 # closure checks, 12 pictures, measurements
tools\lightscenes.bat --checks        # the numbers alone, fast
tools\lightscenes.bat --only 08       # one scene
```

Pictures land in `build/scenes/`, contact sheet at `00-contact-sheet.png`.
Needs a real GPU — it opens a window and ignores it.

**In game:** `F5` draws the raw light field flat in grey over everything, sky
included — white lit, grey partly shadowed, black unlit, orange rectangle marking the
solved region. This is the view to screenshot when a shadow looks wrong. `L` draws
the solve's *edges* over the world instead of covering it — see §4b. `B` toggles the
bounce, `C` takes the cloud out of the light while leaving it drawn, `F6` is unlit as
before.

---

## 3. What has been measured

Closure, in `--checks`. All three pass exactly:

```
empty world, sky all round    got 1.00000   want 1.00000   ok
sealed in rock                got 0.00000   want 0.00000   ok
emission doubles              got 2.00000   want 2.00000   ok
```

The first one is the strong one: a uniform sky closes at exactly 1 only if all four
quadrants, every cone arc at every level, the boundary cascade and the reconstruction
are simultaneously right. It is what found the 2π.

**Checkerboard: not present.** Mean second difference across rows vs across columns,
in a region that should be smooth:

```
cross blur   across rows   across cols   ratio
off             0.024829      0.024420   1.017
on              0.003149      0.003471   0.907
```

The two axes agree to 2% with the blur off. The doubled direction count removed it at
the source. The blur stays, but as an **anti-aliasing filter** for the isotropic moiré
the paper names separately — it takes out about eight ninths of it. Anything proposing
to replace it has to be judged against isotropic noise, not against a checkerboard.

**Stability under region movement**, converged field, world held still, region jumps
one stride, light compared at the same world positions:

```
what is on              stride    of level
bounce + sky                32       2.41%
sky only  (bounce off)      32       0.07%
bounce only                 32       2.43%
lamps alone (bounce off)    32       0.04%
```

Direct transport does not care where the region stands. All the movement is the
bounce, and only because the strip a jump newly exposes has no history. That is why
`kSnap` in `world.cpp` is **2** — the smallest it can be, since the two interleaved
probe grids are one cell apart and an odd origin swaps them. Walking the same distance
costs the same total either way; a coarse snap saves it up and spends it in one flash.

**Frame**, `--profile 300`, full screen, flying:

| | before | now |
|---|---|---|
| light solve | 13.1 ms | ~4.2 ms |
| light upload to screen | 1.7 ms | 0.003 ms |
| cloud pass | — | 0.67 ms |
| frame | 24.1 ms (42 fps) | ~20.6 ms (49 fps) |

Two zones nobody touched read ~27% high in these runs, which by `CLAUDE.md` §4's own
rule means the machine was warm. Remeasure cold before writing any of this down as
final.

---

## 4. THE SKY BUG — done, and what it turned into

**The drawn sky was being multiplied by the light field, and it must not be.**

`world.Sky().DrawAtmosphere(view)` was the first thing `DrawScene` drew, so
`ComposeLight` multiplied it along with everything else. Harmless under the old
solver, which had no cloud shadow in open air. With clouds as real matter casting
real shadows *through* the air — and the visible band of sky sitting **below** the
deck — the cloud's shadow fell across the backdrop and dimmed the whole of it,
flickering as the player walked from under one cloud to the next.

Wrong by physics and not only by taste: the backdrop is the sky *above* the clouds,
and it is the source. Under a cloud you see the cloud grey and, beside it, ordinary
blue. You never see darkened blue.

### What was done

`src/lit_layer.h` / `.cpp`. The world goes into a screen-sized target of its own,
the multiply happens inside it, and the atmosphere is drawn straight into the frame
with the layer composited over it. `DrawScene` split in two: `DrawLitWorld` is the
layer's contents and runs beside the other captures, before `BeginDrawing`.

```
lit.Capture()                 // cleared to BLANK, layer blend set
  DrawUnderground             // the country behind the ground -- see below
  DrawClouds
  terrain, plants, player, rain, mist, liquids
  ComposeLight                // multiplies only what is in this target
lit.Finish()

DrawAtmosphere                // straight into the frame, unmultiplied
lit.Compose()                 // over it, premultiplied
stars, overlays, cursor
```

The drawn clouds stay **inside** the layer on purpose: they are objects in the world
and should darken at dusk and glow at noon. Only the blue behind them comes out.

### Four traps, all paid for

1. **The alpha channel is now half the picture, and raylib's `BLEND_ALPHA` gets it
   wrong.** It sets one factor pair for both channels, so alpha comes out
   `src.a² + dst.a(1 − src.a)` — squared on the first thing drawn over an empty
   target. Nobody could ever see that over an opaque background, and the two layers
   this codebase already had are both drawn over one. Here the alpha is what says
   how much cloud, rain or fog stands in front of the blue: squared, a cloud at a
   half lets three quarters of the sky through. `LitLayer::Blend()` sets
   `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` for colour and `(ONE, ONE_MINUS_SRC_ALPHA)`
   for alpha, which leaves the target **premultiplied** — hence
   `BLEND_ALPHA_PREMULTIPLY` in `Compose`, not the ordinary alpha blend.

2. **`ComposeLight` must not touch alpha.** Spelled out as
   `(RL_DST_COLOR, RL_ZERO)` / `(RL_ZERO, RL_ONE)` rather than left to
   `BLEND_MULTIPLIED`. That mode happens to leave alpha alone too — it is
   `(RL_DST_COLOR, RL_ONE_MINUS_SRC_ALPHA)` and the field's own alpha is 1 — but
   only because of a constant in `light_shaders.h`. `--probe` is byte-identical
   across the change, which is what says the two are equivalent today.

3. **The sky came through the caves.** This is the one that only shows once the
   fix is in, and it is the whole reason `World::DrawUnderground` exists. The
   atmosphere was never only the sky: it also filled the frame *underground*, where
   it was multiplied down to black and nobody ever saw it. Taken out of the layer,
   an unlit cavern came out the pale blue of a bright afternoon.

   The first fix was to lay the same air down a second time inside the layer and
   erase it everywhere the land was not standing behind it. It worked and it read
   wrong, and the reason is worth keeping: what it left behind a hillside was the
   *sky*, dimmed. Below the horizon `AirAt` gives the thickest air there is, which
   is the pale wash of the horizon itself — so every hole dug at the surface had a
   flat panel of grey in it that matched nothing around it, and at midday, when the
   light multiply barely darkens anything, it matched nothing most of all.

   What is behind a cave is the rock the cave was cut out of, and it is now drawn as
   that. `World::PaintBackdrop` paints the generator's own country — the rock and
   the covers over it, and nothing else — into the chunk texture, first, so it is
   behind the walls and behind the ground and the frame pays one blit that was
   happening anyway. Measured at **+0.1 ms** on `--profile`, all of it in
   `PaintChunks` and all of it paid once per chunk.

   Three things are deliberately left out of it, and they are what separates a
   backdrop from a second copy of the world: no ore, because a seam is a thing to
   find by digging and a backdrop full of them is a map of where to dig; no water,
   because a level is a thing that moves and this is a picture that does not; and no
   caves, because a cave drawn behind a cave is a hole with a hole behind it.

   It is drawn at `kBehindShare`, a little over half its own brightness — Minecraft's
   figure, and settled by the same argument. Seamless is not the aim and cannot be:
   the backdrop is lit by the same daylight as the hillside in front of it, so at the
   same tone a pit dug at noon would be a hole nobody could see. Darkening is also
   the house rule for reading as *behind* — it is what separates the wood wall from
   the planks it is made of — and it is not a fade, because §5.5 of CLAUDE.md needs
   every colour the ground is drawn in to be opaque.

   `DrawUnderground` is what is left of the old pass: one flat rectangle per lattice
   column in the deep rock's own dark, over the whole view below the skyline. It is
   the floor under the backdrop, for the stretches no chunk has a picture of yet — so
   a chunk arriving late shows as a patch of plain rock rather than as a window onto
   the sky. Opaque, because the alpha in this layer is how much of the sky a pixel
   covers and below the land the answer is all of it.

   The land both of them measure against is `terrain::Height` — the *generator's*
   country, on §8's rule for the covers. Not `Skyline`, which follows the sky down
   into a shaft and would have let daylight into the cave the shaft opens onto; and
   not `SurfaceProfile`, which lets what has been built raise the surface and would
   turn the whole sky under a mid-air platform into cave. A hole somebody dug is a
   hole into rock.

   `kBehindSink` puts both four pixels below that line. The contour crosses half a
   lattice step out from the last filled vertex, so the two descriptions of the
   ground do not agree to the pixel, and a backdrop that started a pixel high would
   read as a comb of dark teeth along every hilltop. What sinking costs is four
   pixels at the lip of a fresh surface cut where neither the ground nor the backdrop
   is drawn and the sky shows through — which is the sky it is continuous with, four
   pixels lower than it should be, and the far cheaper of the two faults.

   And the backdrop is painted over the chunk's **own span**, never into its margin.
   The ground gets away with painting into the margin because its squares are
   disjoint — a square belongs to whichever chunk its middle falls in, so a neighbour
   drawing over the same strip has nothing there to draw. A backdrop fills every
   square it is given, so a margin painted by both is the second chunk laying plain
   rock over the first one's ground: a dark band down every chunk border, the width
   of two margins, which is exactly what the first build of it did.

4. **The night went with it.** `Sky::AirAt` says in its own comment that it gives
   *colour only* — "how bright the sky is belongs to the light layer the whole scene
   is multiplied by, and it is already carrying the day". True, and the whole day/
   night cycle of the drawn sky was that multiply. Out of it, midnight came out the
   blue of a bright afternoon with black trees standing against it.

   `Atmosphere::night` in `weather.h` is the knob now, and it is the only one — a
   share of full day, floored so a clear night is dark blue and not a void for the
   stars to hang in. Applied in `DrawAtmosphere` and **not** in `AirAt`, which is
   right to stay colour-only: its other three callers are the rain, the snow and the
   fog, all drawn inside the light, and dimming there would darken them twice. It is
   applied *after* the overcast wash, or a closed sky at midnight sits at the full
   brightness of `Atmosphere::overcast` — brighter than the clear sky it replaced.

   Nothing here changes how much light the world *receives* at night. That is
   `light::Sky::radiance`.

### What it costs

Interleaved off/on/off/on, `--profile 300`, full screen and flying: **21.6 / 24.4 /
24.3 / 26.0 ms**. So the layer is somewhere near two milliseconds — but `StepLight`,
which nothing in this change goes near, drifted 8.6 → 11.3 ms across the same four
runs, which by §4 of `CLAUDE.md` is the machine heating up and is as large as the
effect. **Remeasure cold before writing a number down.**

---

## 4b. THE FLICKER — found, and it was the trees

**Symptom:** the daylight stepped as the player walked, in the parts of the world
that were lit. Survived turning the bounce off, which is what finally placed it —
§3's stability numbers were right all along and were measuring something else.

**Cause:** `Medium::cover`, the one part of the daylight that was a *field* rather
than a direction. A canopy could not be stamped into the medium as extinction the
way a cloud is — extinction is fog, a material's own picture hides its cell, and a
canopy stands in open air where nothing is drawn, so the fog was the only thing on
screen and every tree wore a grey blob in the sky above it. So it was held back as
a share per column instead, applied to the sky a ray reached at the end of its
march.

What that could not survive is that the share was offered by whatever the grove was
holding that frame, and the grove's set turns over as the player walks. A tree
entering or leaving it added or removed a **whole band of held-back sky at once**,
and the ground under that band stepped. Nothing about it was gradual and nothing
about it was transport: a set membership changing, expressed as daylight.

**Fix: the canopy shade is gone, and the whole `cover` mechanism with it.** It was
the field's only client once the cloud became matter, so what came out is
`Medium::cover`, `coverBuf_` (binding 8), the `Cover` SSBO and the `(1 - bCover[c])`
term in `SkyRadiance`, `World::AddCover` and `covers_`, `Grove::Shade` and
`kCanopyShade`. `SkyRadiance` now takes a direction alone, which is what the paper's
footnote 1 says it is. The three closure checks still pass exactly.

**A wood therefore has no dark floor.** That is a real loss and it is item 1 below.

**Two false trails, both cleared and both worth not walking again:**

- *The sprite atlas.* `Grove::Shade` skipped any plant the sheet was not holding, so
  the daylight was partly a function of a drawing cache with a per-frame bake budget
  and LRU eviction. It looked like the answer. It was instrumented — a count of
  casting against skipped, and a mark at every canopy whose shade was missing — and
  the skipped count never moved off zero. Not it.
- *The bounce.* `B` toggles it in game now. The flicker survives it.

**What was built to find it, and is worth keeping:**

| key | what |
|---|---|
| `L` | every edge the light solve has — region, its snapped origin in cells, the three coarsest probe lattices, the cloud deck against how much of it the region actually stamped |
| `B` | the multi-bounce on and off |
| `C` | the cloud out of the light while it stays drawn (it used to gate the tree shade too, which made it useless for clearing either suspect alone) |

`L` also carries a finding that has **not** been chased: the deck's stamp is clipped
to the region, `max(top, 0)` and `min(bottom, rows - 1)`, so the deck is only as
thick as the part of it that fell inside. The overlay says `whole` or `CLIPPED`. If
it says `CLIPPED` while walking, every cloud shadow on screen is changing depth as
the region's top edge crosses the deck's.

---

## 5. Also open, in the order I would take them

1. **Canopy into the medium as leaves, which now means giving a wood its floor
   back.** The per-column share is gone (see §4b) and nothing replaced it, so a wood
   is currently as bright as the field beside it. `weather.h` records why the leaves
   cannot simply become extinction stamped into open air: they are drawn and the fog
   is not, so every tree wore a grey blob in the sky above it. Putting the *leaves
   themselves* into the medium is what resolves that — the same road the cloud took
   — and it is the only version that cannot bring the flicker back, because then a
   canopy occludes by being there rather than by being in a set.

2. **The region is 88 MB and 4× larger than intended.** `RADIANCE: 512x512 cells,
   256x256 probes, 9 cascades, 88.0 MB` for a 1920×1080 screen. `side()` rounds the
   height to 512 because the `active` rectangle reaching `StepLight` has a vertical
   span around 1248 px rather than the ~180 cells the window implies. Find out why
   `active` is that tall. The solve is doing four times the necessary work.

3. **`--profile` has no zoom argument.** It takes `[frames] [still]`. The requested
   test — maximum zoom, close to the player, to catch spikes — needs one.

3a. **`--probe` cannot see the sky, and now that is a gap.** It clears to a flat
   colour and draws no atmosphere, so the whole of §4 — the backdrop, the erase, the
   cave that came out blue — has no still picture and was judged by eye. Giving the
   probe a `LitLayer` and the two atmosphere passes would make it the instrument for
   this, at the cost of every stored reference image. Worth it; the class of bug it
   would catch is the class that took the longest here.

4. **Cloud opacity constants**, if they need taste rather than fixing:
   `world.cpp:1982` `kCloudSigma = 0.02f` (extinction per cell; the deck is ~70 cells
   so full cloud passes ~25% of the light — exponential, so thickness gives partial
   opacity for free) and `world.cpp:1987` `kCloudAlbedo` (how bright the cloud is,
   not how dark its shadow; keep well below 1 or multiple scattering washes the
   shadow out from the inside).

5. **Rewrite `CLAUDE.md` §4, §5.2, §5.4** once the above settles.

---

## 6. Method, since it cost real time to learn here

I got three diagnoses wrong in a row on the flicker — blamed the plants, then cascade
alignment, then my own guesses — and each time the measurement contradicted me. Two of
the "bugs" were faults in the instrument, not the solver:

- `Field::At` indexed the readback (a frame behind) through `origin_` (already
  advanced), so every reading was displaced by however far the region had walked.
  Fixed with `readOrigin_`; it was a real gameplay bug too.
- The stability test solved once after moving, so what it read back was still the old
  region — it compared the settled field against itself.

Both showed up as a large, plausible, entirely fictitious "50% instability". **Build
the instrument, then distrust the instrument.** The closure checks are cheap and they
are what caught the 2π; run `--checks` after anything that touches the solver.
