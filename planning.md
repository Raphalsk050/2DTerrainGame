# Terrain planning

Working notes for world generation. Part reference (how Minecraft does it, and
what its biome set actually is), part design (how this project is structured and
what is still missing).

---

## 1. Why the old generator had to go

The first generator was a single fractal Brownian motion field faded out towards
the sky:

```
density(x, y) = fbm(x, y) * fade(y)
solid         = density > 0.45
```

One field decided everything, and that is where its problems come from.

- **The surface was a slice through a cave field.** The ground and the caves
  were the same isoline of the same noise, so the fade band came out as swiss
  cheese: holes in open ground, disconnected lumps of rock floating over them,
  no line that reads as *the surface*.
- **The caves were not caves.** Perlin noise is isotropic, so the empty space it
  leaves is round and evenly scattered. Isolated pockets, no corridors, no
  reason for one pocket to connect to the next. There is nothing to explore in a
  field of sealed bubbles.
- **Every knob moved everything.** Raising `octaves` to roughen the hills also
  shattered the caves; lowering `frequency` to widen the caves also flattened
  the world. There was no way to express "flatter here, more caves there".
- **No place had an identity.** One parameter set covered the whole world, so
  every stretch of it looked the same and biomes had nowhere to attach.

The fix is not a better noise function. It is **more than one field, each doing
one job**, composed in a fixed order.

---

## 2. Reference: how Minecraft generates a world

Worth writing down because Minecraft solved the same problems, and its
vocabulary is useful even though our world is 2D.

### 2.1 Terrain shape comes from stacked density functions

Since 1.18 the terrain is not a heightmap and not a raw 3D noise threshold. It
is a **noise router**: a graph of *density functions*, each a value per block
position, combined into one `final_density`. Where `final_density > 0` the block
is stone, otherwise air (which aquifers may then fill).

The parts that matter to us:

| Function | What it does |
| --- | --- |
| `continents` (continentalness) | Rises inland. Low values become ocean, high values inland highland. |
| `erosion` | How flat the land is. High erosion flattens; low erosion allows mountains. |
| `ridges` → PV (peaks and valleys) | Folded from `weirdness`; picks ridge lines and valley floors. |
| `depth` | A vertical offset: how far the column's target height is from sea level. |
| `initial_density_without_jaggedness` | The base terrain before the fine ridge detail is added. |
| `final_density` | Everything combined, caves subtracted. The only thing that decides solid vs air. |
| `preliminary_surface_level` | A cheap 2D estimate of the surface height, used by aquifers and surface rules. |

The important structural idea: `continentalness`, `erosion` and PV are passed
through **splines** into a target height and a "squash" factor, and the 3D
density is then derived from that target height. So the world has a
heightmap-like backbone — the surface is continuous by construction — and the 3D
noise only perturbs it, which is what allows overhangs without shattering the
ground. Our generator uses the same trick.

Also worth noting: in the noise router, `temperature`, `vegetation`,
`continents`, `erosion`, `depth` and `ridges` **only choose the biome**. They do
not shape the terrain; the terrain is entirely `final_density`. Biome and shape
are decoupled, which is why a biome can be swapped without breaking the land.

### 2.2 Caves are a separate subtraction, in three flavours

This is the part the old generator was missing completely. Minecraft carves
caves out of already-solid rock, with distinct layers that have distinct jobs:

- **Cheese caves** — the large open chambers. A low-frequency 3D field, air where
  the field is inside a band. Named for the holes it leaves in the rock. These
  are the rooms, and they are what makes a cave system feel like a place. Often
  contain **noise pillars**, columns of stone left standing floor to ceiling.
- **Spaghetti caves** — long winding tunnels, carved as a band around the
  *zero set* of a noise field rather than around its extremes. The zero set of a
  noise field is a family of long curves, so the result is corridors, not
  pockets. Roughly 85–112 blocks from root to branch tip.
- **Noodle caves** — the same construction, thinner: tunnels 1–5 blocks wide.
  The crawlways that connect the larger galleries.
- **Carver caves** — the older, pre-1.18 system, still present. Procedural
  tunnels stamped along a path: a circular main room (25% chance) 1–14 blocks
  high and 5–15 across, with 1–4 branching trunks. Y −56 to 180, denser below
  Y 47. Merging carvers produce the messiest and most spacious caves.
- **Ravines / canyons** — long narrow vertical gashes, often breaking the
  surface. Visually the most dramatic thing underground and the cheapest way to
  give the player a way in.
- **Aquifers** — local water tables with their own surface level per region, so
  a cave can be flooded to a height that is not the global sea level. Below
  Y 0 they can be lava instead; from Y −55 to −63 always lava.

The three-layer split (rooms + wide tunnels + narrow tunnels) is the single most
valuable idea here. Rooms alone are disconnected. Tunnels alone are monotonous.
Together, tunnels stitch the rooms into a network, and the difference in width
between galleries and crawlways is what gives a cave system a sense of scale.

### 2.3 Cave biomes

Caves also get their own biomes, chosen from the same climate fields plus depth:
**Lush Caves** (below high-humidity surface), **Dripstone Caves**,
**Deep Dark** (deep, in the deepslate layer), **Sulfur Caves**. Note that Lush
Caves are placed by the *surface* humidity above them — the underground inherits
identity from the land overhead, which is a cheap way to make digging down
somewhere feel connected to where you dug down from.

---

## 3. Reference: the Minecraft biome set

Java Edition 1.21+: 66 biome types — 55 Overworld (54 reachable plus the void),
5 Nether, 5 End, plus one superflat-only.

### 3.1 Overworld

**Offshore**
Ocean · Deep Ocean · Warm Ocean · Lukewarm Ocean · Deep Lukewarm Ocean ·
Cold Ocean · Deep Cold Ocean · Frozen Ocean · Deep Frozen Ocean ·
Mushroom Fields (mycelium; never borders another biome)

**Highland**
Jagged Peaks (tall pointed peaks, snow) · Frozen Peaks (smoother, packed ice) ·
Stony Peaks (warm, no snow, stone and calcite) · Meadow (elevated grassy
plateau, flowers) · Cherry Grove (pink petals, cherry trees) · Grove (spruce
under the peaks) · Snowy Slopes (snow and powder snow) · Windswept Hills (steep,
sparse trees) · Windswept Gravelly Hills · Windswept Forest

**Woodland**
Forest (oak and birch) · Flower Forest · Taiga (spruce, ferns) ·
Old Growth Pine Taiga (2×2 spruce, mossy boulders) · Old Growth Spruce Taiga ·
Snowy Taiga · Birch Forest · Old Growth Birch Forest (birches up to 13 blocks) ·
Dark Forest (closed leaf roof) · Pale Garden (desaturated grey, pale oak,
creaking) · Jungle (dense, mega trees, vines) · Sparse Jungle ·
Bamboo Jungle (podzol floor)

**Wetland**
River · Frozen River · Swamp (flat with shallow pools, lily pads) ·
Mangrove Swamp (mud, mangroves) · Beach · Snowy Beach · Stony Shore

**Flatland**
Plains (rolling hills, villages) · Sunflower Plains · Snowy Plains ·
Ice Spikes (packed-ice spires)

**Arid**
Desert (dunes, cacti, fossils) · Savanna (flat and dry, acacia) ·
Savanna Plateau · Windswept Savanna (chaotic terrain, giant mountains) ·
Badlands (terracotta layers, red sand, exposed mineshafts) · Wooded Badlands ·
Eroded Badlands (narrow terracotta spires)

**Cave**
Deep Dark (sculk, ancient cities, warden) · Dripstone Caves (speleothems,
copper) · Lush Caves (moss, glow berries, axolotls) · Sulfur Caves (sulfur,
cinnabar, cave spiders)

**Other**
The Void

### 3.2 Nether

Nether Wastes (the common one, netherrack) · Soul Sand Valley (blue fog,
fossils) · Crimson Forest (crimson fungi, weeping vines) · Warped Forest (warped
fungi, endermen) · Basalt Deltas (chaotic basalt and blackstone, magma cubes)

### 3.3 End

The End (central island, dragon) · Small End Islands · End Midlands (End
cities) · End Highlands (chorus trees, End cities) · End Barrens (steep cliffs)

### 3.4 Sample climate parameters

Biomes carry a temperature and a downfall figure. A few, to show the scale:

| Biome | Temperature | Downfall | Precipitation |
| --- | --- | --- | --- |
| Desert | 2.0 | 0.0 | none |
| Jungle | 0.95 | 0.9 | rain |
| Plains | 0.8 | 0.4 | rain |
| Ocean | 0.5 | 0.5 | rain |
| Taiga | 0.25 | 0.8 | rain |
| Snowy Plains | 0.0 | 0.5 | snow |

Placement uses six values per position — temperature, humidity (vegetation),
continentalness, erosion, weirdness/PV and depth — and picks the biome whose
declared range is nearest. That is a **table lookup in a 6D parameter space**,
not a Voronoi map of regions, which is why Minecraft's biomes blend into each
other along plausible lines instead of tiling.

### 3.5 What this project should take from it

Most of that list is decoration: block palettes, trees, mobs. Only the
structural half is relevant to a 2D side-scroller, and it collapses to a small
set of axes.

| Axis | Range | Drives |
| --- | --- | --- |
| Elevation | lowland ↔ highland | surface `relief` amplitude |
| Erosion | flat ↔ rugged | hill amplitude, terracing |
| Temperature | frozen ↔ scorching | palette, surface cover, ice/lava |
| Humidity | arid ↔ lush | vegetation, water table height |
| Cave density | solid ↔ honeycombed | cave region mask |

A first biome set for this game, defined as points in that space rather than as
copies of Minecraft's:

| Biome | Elevation | Erosion | Temp | Humidity | Caves |
| --- | --- | --- | --- | --- | --- |
| Plains | low | flat | temperate | mid | few |
| Rolling hills | mid | mid | temperate | mid | mid |
| Highland / peaks | high | rugged | cold | low | many |
| Badlands / mesa | mid | terraced | hot | arid | mid |
| Desert | low | very flat | hot | arid | few |
| Frozen waste | low | flat | frozen | low | mid |
| Swamp | very low | very flat | warm | high | few, flooded |
| Karst / cave country | mid | rugged | temperate | mid | very many |

And underground, chosen from the surface climate above plus depth, following
Minecraft's Lush Caves trick:

| Cave biome | Condition |
| --- | --- |
| Dry caverns | default |
| Flooded caves | high humidity above, mid depth |
| Dripstone | mid depth, any humidity, high cave density |
| Deep dark | deepest band, regardless of surface |

---

## 4. This project: the generator as it stands

Terrain lives in `src/terrain.h` / `src/terrain.cpp` and is a **pure function of
world position**. Nothing is cached, no chunk talks to its neighbour, and a
region regenerated later is identical to the one released. Everything below has
to preserve that.

### 4.1 The pipeline

Layers are applied in a fixed order. Each one owns exactly one decision, and the
value passed between them is a **signed distance in pixels**: positive inside
rock, negative in open air, zero on the surface. Working in pixels rather than
in raw noise units is what makes every knob readable — a corridor is 40 px tall
because the number says 40.

```
                    ┌─ relief    (very low frequency)  where the land is high or low
  Height(x) ────────┼─ hills     (mid frequency)       the shape you walk over
   (1D, so it       ├─ detail    (high frequency)      texture underfoot
    cannot          ├─ erosion   (very low frequency)  scales hills+detail: plains vs mountains
    have holes)     └─ terrace                         snaps heights into walkable ledges

  Depth(x, y) = y - Height(warp(x, y))                 warp folds the surface into overhangs

  Caves, each a signed band, subtracted from Depth:
      shafts     stretched vertically      the way in, and the only way down
      crawlways  narrow, carries `floor`   links, and the one layer in dead rock
      galleries  stretched horizontally    long walkable halls, wholly regional
      chambers   isotropic, low frequency  the widenings, with rubble floors
      caverns    very low frequency        the great voids, grown in by depth

  Each corridor's width varies along it (girth) and closes where it runs low
  (pinch); each room is intersected with itself lifted by `rubble` to give it a
  floor; the union is a smooth minimum, so junctions flare instead of creasing.

  Solidity(x, y) = smin(Depth, -shafts, -crawlways, -galleries, -rooms) + roughness
  Density(x, y)  = kSurfaceLevel + Solidity / kDensitySpan      -> [0, 1]
  Ground carries Solidity unclamped, since the density saturates 13 px in.

  Water(x, y)    = the open space below terrain::TableAt(x), compressed  (§4.6)
```

How much of any of it exists at a position is decided by three gates, all in
[0,1] and all multiplied into the layer's width: the **crust**, which keeps
everything but the shafts well under the ground; the **region**, whose coverage
runs from a tenth just under the crust to half far below; and, for the shafts
alone over their first hundred pixels, a **mouth** gate that asks whether there is
cave country a long way *underneath* the opening.

### 4.2 The decisions that matter

**The surface is a 1D heightmap.** `Height` takes only `x`. A function of one
variable has exactly one value per column, so the ground cannot have a hole in
it — the swiss-cheese surface is not fixed, it is *unrepresentable*. Everything
that wants to vary vertically has to earn it by being a separate layer.

**Caves are carved, not left over.** Rock is solid by default and cave layers
remove from it. The alternative — calling low-density noise a cave — is what gave
us sealed bubbles.

**Tunnels are bands around a zero set, not thresholded noise.** Thresholding
`noise > t` gives blobs. Carving `|noise| < t` gives the *contour lines* of the
field, which are long connected curves. Two tunnel layers with different shapes
cross each other, and the crossings turn corridors into a network with loops in
it. Loops matter for exploration: a loop means you can come back a different way
instead of retracing the corridor you arrived through.

**The band width is divided by the local slope, not by an average of it.** A band
is `|f| / |∇f| < halfWidth`, with `∇f` read by difference at the point. This
costs two extra samples per tunnel layer and it is not optional: with an average
slope, the band balloons wherever the field happens to run flat near zero, since
a fixed number of field units then spans a great deal of ground. The first
version of this generator used an average, and it put a four-hundred-pixel pit in
open ground. Dividing locally also makes a corridor the width it was asked for
along its whole length.

**Every number in the settings is in pixels, and that has to be earned.** Two
constants in `terrain.cpp` do it. `kFbmPeak` is the practical peak of a sum of
Perlin octaves divided by their total amplitude — about 0.45, not 1 — so without
it every amplitude delivers under half the pixels it asks for. Both were measured
over a wide area rather than derived, because the analytic bounds are several
times what the field actually does.

**Anisotropy decides whether a cave is walkable.** `NoiseShape::aspect`
stretches features horizontally (`> 1`) or vertically (`< 1`). This is the
single most important gameplay knob down there: galleries with a high aspect run
sideways in long walkable halls, and the same field at a low aspect drops
vertical shafts. In a side-scroller, an isotropic cave field is unwalkable by
construction — you cannot climb a round hole.

**A crust keeps caves underground.** No cave layer may open within `crust`
pixels of the surface, ramping in over `crustFade`. This is the direct fix for
holes in open ground.

**Entrances are the exception, and they are deliberate.** The shaft layer is the
only one allowed through the crust, and it reaches down past the crust into the
gallery layer, so an opening at the surface always leads somewhere. Rare by
design: an entrance should be a find.

**The gallery layer is global; everything else is regional.** A very low
frequency `region` field decides how honeycombed the underground is, and it
scales chambers and crawlways but *not* galleries. So the world varies between
near-solid rock and cave country, while a connected backbone always exists to
travel through. Regional pinch-outs also read as natural cave ends, which a flat
wall does not.

**The position every cave layer is read at is warped, and this is the one that
mattered most.** The zero set of a smooth field is a smooth curve, so a band
around one is a ribbon — and a network of ribbons still reads as clean arcs
meeting at clean angles however its width is modulated and however its junctions
are blended, because every irregularity a cave has comes from the *path* being
irregular and the path had none. Reading the field at a position pushed around by
another field fixes it at the source: the contour meanders, hooks and doubles
back. Inigo Quilez's fbm-of-fbm, and it costs two samples for the whole cave stack
rather than two per layer, since every layer shares the displacement — warping
them separately would pull the junctions apart. Note that warping breaks the
eikonal property the band width relies on, so a warped corridor is narrower than
it asks for wherever the warp compresses space; the widths are authored against
the warp rather than against the setting.

**A corridor of constant width is a pipe.** `Tunnel` carves a band of exactly the
half-width it is handed — the local-slope division above is what guarantees it —
and what that control costs is character, because nothing underground has a
constant cross-section. So the width handed in varies, out of a low-frequency
field of its own (`TunnelLayer::girth`), and a share of it is subtracted
everywhere (`pinch`) so that the corridor closes *entirely* where the swing runs
low. The closures are as valuable as the widenings: a network in which every
passage is open is a graph with no dead ends, and a cave with no dead ends is one
nobody has to learn. This is Minecraft's `spaghetti_2d_thickness`.

**The pinch is a share and the region floor is a factor, and they have to
commute.** Written in pixels the two compounded: a layer held to a fraction of its
width through dead rock had that fraction taken off an already-pinched number,
came out at two pixels, and the entire underground below the entrance layer's
reach was sealed off from the sky. Measured, not reasoned about — it looked
perfectly plausible in the settings.

**The wall is roughened after the layers are unioned, not before.** One folded
high-frequency field added to the finished distance, faded out a few pixels either
side of a surface. It has to be bounded: applied everywhere it leaves bubbles of
air deep in the rock and pillars of rock in mid-air. This is
`spaghetti_roughness`.

**Layers are unioned with a smooth minimum.** The exact union is `min`, and `min`
creases: two corridors crossing leave a wedge of rock with a knife edge in each
quadrant. Nothing in rock erodes to a point. Blending over ten pixels is both the
better picture and the truer one — and it carves, by up to a quarter of the blend,
so coverage has to be measured with it on.

**A room needs a floor, and a field cannot give it one.** A void carved from a
threshold is symmetric about its own middle, which reads as a bubble. Intersecting
the room with itself lifted by `rubble` pixels shaves a band off the *bottom* and
leaves the ceiling alone — one extra sample, and rooms get flat debris floors and
domed roofs.

**The connective tissue is the narrowest layer, not the widest.** Exactly one
layer needs `floor` above zero, or a region border is a wall. Carrying it on the
halls put a fifth of all rock into corridors nobody was meant to find; the same
guarantee on the crawlways cost a quarter of that. What the player crosses dead
rock through should be a squeeze, and a squeeze is cheap.

**Rarity is a fact about depth, not about chance.** `regionCoverage` is two
figures: a tenth of the ground just under the crust and half of it far below.
Rarity is only ever *felt* at the surface, and depth is where the volume belongs
and where the network has to be dense enough to join up at all. One number cannot
say both. This is Terraria's split between the Underground and the Cavern layer.

**The entrance layer has two jobs that want opposite densities.** Above, a hole in
the ground should be a find and should lead somewhere. Below, it is the only thing
in the world carrying a route from one depth to the next, since every other layer
runs sideways — measured, stopping the shafts just under the crust sealed
everything below 900 px off from the sky. So it is gated only over the first
`mouthDepth` pixels, gated on the region *well below the opening* rather than at
it, and gated through the pinch rather than through an allowance, so a shaft the
gate turns down is closed outright instead of reduced to a slit that still lets
the daylight in.

**Water is a level, not a scattering.** See §4.6.

**Ore is pulled onto the cave walls.** One term added to the vein's field before it
meets its cutoff, from `terrain::Ground::solid` — which is why `Ground` carries the
unclamped distance at all, the density having saturated thirteen pixels in. The
cutoff is measured *with* the term included, so the ore is moved rather than
added and `probability` still means what it says.

**Coverage figures are measured, not guessed.** `regionCoverage` and
`chamberCoverage` are shares in [0, 1]. `terrain::Calibrate` samples the field
and takes the quantile that achieves them, so reshaping the noise does not
silently change how much of the world is hollow. Same approach the element table
already uses for ore veins in `World::CalibrateSpawn`.

### 4.3 What it measures

`--caves x y w h` is the probe, and it is in the binary rather than beside it, so
what it measures is the world these settings describe and not a copy of them kept
in step by hand. It takes `name=value` overrides for every cave setting, so a
sweep is a shell loop. `--ore` and `--settle` answer the other two questions
below. Over 8000 × 4200 px with the settings as authored:

| | |
| --- | --- |
| Relief | 271 px, about 90 px of swing per screen of travel |
| Mean surface slope | 10°, steepest single step 14 px (a terrace riser) |
| Cave volume | 13% of the rock just under the crust, 24% at 2000 px; 19.2% overall |
| Reachable from the sky | 85.2% of all open space, without digging |
| Caves lost | 9.2% of the void, in 26 sealed voids over 200 cells |
| Vertical clearance | median 30 px; 88% of the *space* is walkable upright, 95% crouchable |
| Cave mouths | one every 530 px, 67 px wide |
| Water | 24% of the void under the table, which moves 12 px at worst across a cave |
| Ore at a cave wall | 1.7× to 3.0× what the blind rock holds, and 9× for gold, per `--ore` |
| Liquid movement after generating | 0.9–1.2% of the water present, over 600 steps |

Four of those want reading carefully.

**Reachable is 83% and not 98%, and lower is the price of rarer.** A cave network
in two dimensions cannot be both sparse and wholly connected: corridors only join
where they cross, and thinning them thins the crossings faster than the corridors
themselves. Three dimensions hide this — Minecraft's caves can pass over and under
each other — and a side-scroller cannot. So the figure to hold is not how much of
the void the sky reaches but how much of it is *worth* reaching and does not: a
hundred cells of blind crack behind a wall is rock with a hole in it, and two
thousand is a hall nobody will see. That is the **caves lost** row, and it is the
one to watch.

**Clearance is quoted by space and not by passage.** Under half the *passages* are
tall enough to stand in, which sounds like a world of crawlways and is not: a
passage is any unbroken vertical run, and the wall roughness leaves a great many
slivers a texel deep in the side of a hall. Nine tenths of the space the player is
actually in is stand-up height.

**Ore is measured as a ratio, not a share.** The claim `wallBias` makes is that
walking a passage pays better than tunnelling past it, and that is two numbers.
Between two and three is the band that was aimed at: below it the caves are
scenery, above it the rock between them is not worth a pickaxe.

**Liquid movement is the direct test of the water.** Generated water that is not
already at rest collapses the moment it is stepped and does it again every time
the chunk is rebuilt, which is what "the water moves when I walk back" is. Under
one per cent is the automaton's own noise against the region border.

### 4.4 Cost

`Density` went from one 4-octave field to about twenty-six octaves across
thirteen samples, five times the arithmetic. Chunk generation nevertheless came
out *faster* than before, at 0.44 ms against roughly 0.5, because the expensive
part was never the noise:

- **The cheap gates run first.** A position above the surface returns having
  sampled the surface alone, and one inside the crust only ever reaches the
  entrance layer.
- **`World::Emplace` samples the terrain once per vertex, not once per material
  per vertex.** Every material bounded against the ground — three ore veins and
  the water table — used to ask `terrain::Density` the same question at the same
  position. It is now sampled once and passed down through `GeneratedValue`.
  That one change is worth five times more than anything in the noise.

At 0.44 ms a 60 fps frame affords roughly 38 chunks, against the handful that
stream in while walking.

### 4.5 An edit outlives the chunk it was made in

The generator is a pure function of position, so a chunk can be thrown away and
rebuilt identically — **except for what was done to it by hand**, which by
definition no function of position can produce. That was held by keeping the
chunk resident: `edited` pinned it, and `kDropMargin` dropped it anyway at twelve
chunks so that memory grew with the size of the world held rather than with the
distance walked. So walking ~2300 px from something built and coming back found
the noise had taken it back. **A pin is not a memory.**

What is stored instead is the edit, not the chunk:

- **A brush leaves a vertex in one of two states**: holding exactly one material
  at full, or holding nothing. It never leaves a gradient — placing pins to 1.0
  and digging clears — so the *last* stroke over a vertex is the whole truth
  about it, and an edit is one `optional<Element>`. Editing the same vertex a
  hundred times stores one record.
- **Replayed at the end of `Emplace`**, after the exclusion pass, because that is
  where a live edit lands too: a brush writes over the finished field, not into
  the contest that produced it. So a rebuilt chunk *is* the chunk that was
  dropped.
- **Filed by chunk**, so generating one is four map lookups rather than a walk of
  every edit ever made — four because a border vertex belongs to four chunks and
  is filed under one, the same asymmetry `WriteVertex` has, read backwards.
- **Only liquid is still lost past the drop margin**, and deliberately: where a
  pool has crept to is simulation, not intent, and it is what made pinning grow
  without bound in the first place.

The cost moved from per *chunk touched* to per *vertex touched*. A chunk somebody
set one block down in used to hold nine full grids — some 40 KB — for the sake of
one changed number. Reported in the HUD as `edits kept`, since it is now the one
structure that never shrinks.

Two things fell out of it:

- **`MarkEdited` was over-marking.** It set the flag on all four chunks around a
  vertex without checking which actually hold a copy of it — unlike `WriteVertex`,
  whose identical walk has always bounds-checked. One block set down pinned the
  three chunks beside it as well, quadrupling the very memory `kDropMargin` exists
  to bound. Caught because a rebuilt world reported 4 pinned chunks where the
  original reported 10.
- **The replay has to re-set `edited`.** Not for the pin, which is now only a
  performance nicety, but because `SurfaceProfile` opens only hand-touched
  chunks — a rebuilt roof that forgot the flag would stop stopping the rain.

Verified: a wall of 84 solid vertices and a dug pit, walked 7680 px away until
zero chunks remain pinned, walked back — 84 vertices and the same pit, the same
4 pinned chunks, and the rain still landing on the wall at the same height. `R`
still clears everything.

---

### 4.6 Water is a level, not a scattering

The old water was a share of the cave vertices in a depth band, filled to full
mass wherever the noise cleared its cutoff. Two things were wrong with it and only
one of them was the amount.

**A blob of mass is not a shape water can hold.** The liquid automaton settles a
column into a *gradient* — two stacked cells come to rest with the lower holding
`kMaxCompress` more than the upper — so anything laid down flat is out of
equilibrium by construction and falls the moment it is stepped. And because
liquid is deliberately not journalled, it falls again every time the chunk is
rebuilt. That is the whole of "the water moves when I walk back into a place": not
a chunk bug, a statement about the world that was never true.

So the water is now the open space below a **water table** — `terrain::TableAt`,
a function of the horizontal position alone, exactly like the surface and the
climate. Everything follows from that:

- **The table is snapped to a multiple of `step`.** A continuous field cannot be
  both varying and flat: to move `swing` pixels it has to slope somewhere, and any
  slope at all is a surface out of equilibrium along its whole length. Snapping
  removes the slope; the frequency being very low — one feature spans some 170 000
  px — is what makes the steps rare. Measured: a 12 px step every 730 px, so the
  worst a cave sees is a two-cell ledge that settles in a few frames.
- **There is no field deciding *whether* a stretch has water.** Anything that
  switches the table off puts a boundary somewhere, and a boundary falling inside a
  cave is a vertical wall of water with nothing holding it up. A table always
  present and merely sometimes deeper than the deepest cave has no boundary to
  fall anywhere. Rarity comes from the depth instead.
- **The column is generated compressed.** `min(under, 1) * kMaxMass + max(under -
  1, 0) * kMaxCompress`. Filling at full mass all the way down looks right and is
  not — it is a column with too little at the bottom and too much at the top.
  Measured with `--settle`: a third of the water in a deep region moved before this
  and under one per cent after it.

Where the table crosses a cave is the case worth having, and it is common because
the table sweeps 2400 px across the world: a chamber with its floor under water and
its roof in the air.

Two chunk-lifecycle faults went with it. `StepWater` cleared `holdsLiquid` over a
range a chunk wider than the one it wrote back, so chunks along the edge of the
simulated band lost their pin and were dropped at 384 px instead of 2304. And
`kSimulationMargin` was 128 px against a 192 px chunk, so a chunk could be created
and scroll into view without a single vertex of it ever having been stepped.

## 5. Ores

### 5.1 Reference: how Minecraft places them

Each ore is a *configured feature* (what one vein looks like) placed by a *placed
feature* (how many, and at what heights). The numbers, for the overworld set:

| Ore | Vein size | Veins/chunk | Height range | Distribution | Discard on air |
| --- | --- | --- | --- | --- | --- |
| Coal | 17 | 20 | Y 0…192 | triangle | 0.5 |
| Copper | 10 | 16 | Y −16…112 | triangle | 0 |
| Iron | 4 | 10 | Y −64…72 | uniform | 0 |
| Gold | 9 | 4 | Y −64…32 | triangle | 0.5 |
| Diamond | 4 | 7 | Y −64…16 | triangle | 0.5 |
| Emerald | 3 | 100 | Y −16…480 | triangle, mountains only | 0 |

Three ideas are worth taking, and they are all about *shape* rather than amount:

- **A triangle, not a slab.** An ore is densest at one height and thins away from
  it. Iron's uniform band is the exception and it is the one ore that is meant to
  be everywhere.
- **Size and count are separate knobs.** Coal is 17 blocks × 20 veins; iron is
  4 × 10. Two ores can hold the same share of the stone and feel completely
  different — one as seams worth following, the other as scattered specks.
- **Emerald is a biome ore.** Mountains only, denser with altitude. It is the one
  ore that rewards climbing instead of digging.

Redstone and lapis lazuli are deliberately not included.

### 5.2 This project: how an ore is written

One row in `kElements`, and four numbers decide everything about where it is:

| Field | Question it answers |
| --- | --- |
| `probability` | How likely is it *where it is densest*, as a share of eligible rock |
| `veinCells` | How many lattice cells across is one vein |
| `band.peak` | Which layer is it densest in |
| `band.top` / `band.bottom` | Where does it thin out to nothing |
| `band.scarcity` | How rare is it *outside* that band |

**`probability` is measured, not declared.** `World::CalibrateSpawn` samples the
ore's own noise and takes the quantile that achieves the share asked for, so
changing a vein's size does not silently change how much ore the world holds.
Two things make that measurement meaningful, and both were learned the hard way
because getting either wrong is *silent* — the ore simply is not there:

- Samples must be **more than one noise feature apart**, and by an *irrational*
  multiple of it. Perlin noise is exactly zero at every corner of its lattice, so
  a grid stepping a whole number of features reads the same corner over and over;
  every sample comes back at the midpoint of the field, the cutoff lands in the
  middle of the distribution instead of its tail, and the world fills with ore.
  This is what produced the screen-sized emerald blobs at the surface.
- The **tail has to be populated**. A quantile at one in a thousand rests entirely
  on the samples above it, so the grid is sized from the probability: rarer ores
  are measured over a proportionally larger stretch of world.

**`veinCells` is the size knob and it is separate from the amount on purpose.**
Lowering the probability alone leaves the veins exactly where they were and merely
shaves them down, which is how an ore can be made rare and still feel like it is
everywhere. The generator works the noise frequency back out of it, via
`kVeinFeatureSpan` — a vein is the cap of one noise feature standing above the
cutoff, so its width is a fixed fraction of that feature's. The constant was
measured across the whole table and held to within 8% over ores from three to
eight cells wide and a 25-fold range of probability. It is not a hard ceiling: a
true cap would need the generator to know the extent of a shape it evaluates one
vertex at a time.

**`band.scarcity` is a floor, not a boundary.** It raises the cutoff away from the
peak rather than cutting the field off, so an ore thins out with distance from its
level and survives beyond its band wherever its noise ran high — the occasional
pocket a long way from home. Set too high it stops being a floor at all: at 0.3 an
ore outside its band is ninety times rarer, which reads as *absent*, and that is
what left everything below y 2000 barren. Around 0.16–0.24 is the working range.

### 5.3 What it measures

At each ore's own peak, over a 24 000 × 900 px strip. A screen is 1000 × 600 px,
which is the unit to judge scarcity in, and the character is 26 px tall:

| Ore | Peak y | Cells | Share of rock | Veins per screen | Mean vein |
| --- | --- | --- | --- | --- | --- |
| Coal | 480 | 8.0 | 1.37% | 5.0 | 46 px |
| Copper | 560 | 6.5 | 0.59% | 3.0 | 39 px |
| Iron | 1500 | 6.0 | 0.57% | 3.5 | 36 px |
| Gold | 1800 | 3.5 | 0.043% | 0.9 | 19 px |
| Diamond | 2400 | 3.5 | 0.027% | 0.5 | 20 px |
| Emerald | 320 | 3.0 | 0.009% | 0.35 | 14 px |

And by depth, the share of rock that is ore *of any kind* — the row to watch,
because a band reading zero is a stretch of world with nothing in it:

```
   y  200- 600   2.05%     y 2200-2600   0.60%
   y  600-1000   2.48%     y 2600-3000   0.33%
   y 1000-1400   1.50%     y 3000-3400   0.37%
   y 1400-1800   1.26%     y 3400-3800   0.46%
   y 1800-2200   0.62%     y 3800-4200   0.31%
```

Diamond's band has no bottom, because this world has no floor either: below its
peak it stays at full abundance for ever, so descending always has a reward. Every
other ore persists past its band on its scarcity floor alone.

---

## 6. Weather

Lives in `src/weather.h` / `src/weather.cpp`, with the climate half in
`terrain::ClimateAt` and the noise in `terrain::Worley` / `terrain::Sample`.

### 6.1 One direction of dependency

```
  THE WEATHER      a state of the whole world, on a timer
  clear / fair / overcast / storm
        |
        +--> how much of the sky is filled
        +--> whether it is raining, and how hard
        +--> the palette the cloud is lit and shaded with
                |
  THE CLOUD FIELD  procedural, a pure function of position
        |
        +--> what is drawn in the sky
        +--> how much daylight reaches the ground   (light::Medium::cover)
        +--> where the drops fall from
```

Nothing asks downwards. That single rule is what the whole rework was for.

### 6.2 Rain is a state of the world, not of a cloud

This is the one structural decision. It was the other way round and it was wrong:
rain read off each column's own cloud thickness, so a small cluster rained while
the cluster beside it, in the same weather, stayed dry.

Every game that has solved this solved it the same way, and it is worth writing
down because it stops the question being reopened:

- **Minecraft** — "weather is always global: one sky for the entire dimension. When
  rain arrives, every loaded biome transitions to overcast." States with duration
  counters; on expiry the next is drawn at random.
- **Terraria** — rain affects all surface biomes for its duration; 24-minute events,
  17.4% chance, timer-driven.
- **Stardew Valley** — one weather per day for the whole map, probabilistic by
  season.

So `MoodDef` is a table with one row per kind of weather, and the row is the whole
definition: cover, rain, likelihood, palette and shade move together because they
are one fact about one afternoon. The storm's `cover = 0.94` is *why* a rainy sky is
overcast — not a second rule written somewhere else.

The clock is not a state machine. Time is cut into spells of `spellMinutes`, each
spell's mood drawn from a hash of its index and the seed, and the last
`crossMinutes` blended into the next. So the weather is a pure function of the
clock: nothing to store, nothing to save, and two views of the same world at the
same moment agree — the property the rest of the module already had.

The front survives, demoted to rippling the cover from place to place so the sky is
not a flat sheet. `frontInfluence` and `humidityInfluence` are deliberately small
(0.14 and 0.10): they texture the weather, they do not overrule it, or a storm
would have clear patches in it.

Measured over 60 000 px: in a storm **100% of columns are raining** and cover runs
0.85–1.00; in fair weather 0% and 0.22–0.48. Never in between.

### 6.3 Shape: Perlin-Worley with edge erosion

The recipe is Nubis's (Andrew Schneider / Guerrilla, Horizon Zero Dawn), and it is
three fields:

| Field | Job |
| --- | --- |
| `shape` — Perlin | The drift and the large shape: where there is cloud at all. |
| `lobes` — Worley, ~3× the frequency, inverted | The bumps. Inverting turns cell walls into cell centres, so adding it puts a rounded bulge on the outline. This is the cauliflower. |
| `detail` — Worley, ~3× again | The erosion, eating the silhouette into a rim of small lobes. |

The lobes field **has to be finer than the base** — cells the size of the cloud only
move the outline about. That was the first attempt and it produced flat blobs.

The erosion is sampled **only within `erosionBand` of the cutoff**. Deep inside a
cloud it cannot change the outcome and outside it there is no outline to erode, so
the band that matters is narrow and skipping the rest is most of the cost of the
field. Nubis's own optimisation.

**The three fields do not travel together, and that is what makes them weather.**
They used to: one sample position, `x - t·wind`, shared by all three. Every layer
moved at exactly the wind, so nothing about the silhouette ever changed and the sky
read as a cutout on a conveyor. What each one does on top of the drift now:

| Field | On top of the drift | Why |
| --- | --- | --- |
| `shape` | `evolve` px/s along the noise's **third axis** | `stb_perlin` is 3D and `Fbm` was passing `z = 0`. There is no depth in a side-scroller, so travelling along it does not *scroll* the field, it *changes* it — the difference between a cloud that arrives and a cloud that forms. Free: the noise interpolates the corners of a cube either way. |
| `lobes` | `lobeCrawl`, mostly vertical | Worley is 2D and has no depth to move through, so a relative velocity is its share of the same job. |
| `detail` | `detailCrawl`, about twice as fast | Small features change faster than large ones: the rim boils while the body only rolls. |

Two things worth keeping:

- **Vertical, deliberately.** Sideways is the direction the eye has already been told
  means wind, so a large horizontal difference between layers reads as texture
  sliding under a stencil. Nothing else in the sky moves up, so the eye reads that as
  convection instead — which is what a cumulus does.
- **None of it can upset the calibration.** The three fields carry different seeds
  and are independent, so their joint distribution is the product of their own
  whatever the offsets are, and every plane through a Perlin field is distributed
  like every other. `cutoff_` measures the same field at any moment. Worth knowing
  before touching the numbers.

Measured over 20 s **in the wind's own frame**, so pure drift reads as zero and what
is left is the shape itself re-forming:

| | drift alone | with `evolve` + crawl |
| --- | ---: | ---: |
| mean change in density, cover 0.35 | 0.017 | **0.208** |
| silhouette flipped, cover 0.35 | 1.5% | **21.1%** |
| mean change in density, cover 0.94 | 0.020 | **0.270** |
| silhouette flipped, cover 0.94 | 2.0% | **27.3%** |

`NoiseShape::offsetZ` is on the shared noise type, so **every terrain layer had to
stay untouched**: the per-octave depth is `z · frequency · (1 + 0.31·o)`, a
*multiply*, which is exactly zero at `z = 0`. Verified against 32 800 samples of
`Height` and `Density` — bit for bit identical. (The non-integer ratio between
octaves is not decoration: Perlin loses a little variance on the integer planes of
its own lattice, and at a plain frequency the octaves sit at 1z, 2z, 4z, so their
flat spots coincide and the sky's contrast would dip on a fixed period.)

### 6.4 Light: Beer-Lambert and the powder term

```
beer   = exp(-absorption · depthToSun)      the path to the sun
powder = 1 - exp(-2 · powderScale · here)   the density at this point
lit    = beer · powder
```

Beer alone is monotone, and a cloud shaded by it reads as a smear with a gradient
on it. Powder models the light that scatters back out of a thin edge and runs the
other way — zero where there is almost no cloud, rising as it thickens. So the very
fringe is dark, the body just inside it bright, and the far side dark again because
Beer has taken the light. **That double curve is what the eye reads as cloud.**

Two traps, both hit and both worth recording:

- **They are measured on different things.** Powder applied to the path is zero at
  zero depth, so a point with a clear line to the sun — the brightest thing in the
  sky — came out in the darkest band.
- **`absorption` is against a field margin, not a distance.** Margins run to about a
  third, so at 2.0 the darkest Beer term was 0.5 and every cloud sat in the top of
  the bands, uniformly bright. It wants ~7.

The result is quantised into `layers` bands at the very end. The model is
continuous; the flat bands are the pixel-art step over it.

**And it is computed per cell.** Nothing in the chain takes the camera as an input,
which is the fault it replaced: a single column at the centre of the screen used to
decide the shading of every cloud in view, so walking six pixels re-tinted the sky.

### 6.5 The band has to thin by cover, not by a penalty

`bandTaper` scales the *cover* across the band, not the cutoff. A fixed penalty is a
fixed number of field units, so a heavily covered sky — where the cutoff is already
low — walks straight through it: the deck fills the band to its boundary and ends on
a ruled horizontal line at the top and bottom. Thinning the cover means the edge
always runs out to nothing, because a cover of zero has no cloud in it by
definition.

### 6.6 Rain, as it falls

- **Lighter than the air behind it, always.** The drop takes `AirAt` at its own
  height and is lifted towards `rainLine`, so it reads against noon and against a
  storm alike — and against a night, once there is one, with nothing here touched.
  Measured: luma 199–225 against a sky of 157–174. It was a fixed colour multiplied
  down to 30%, which came out very nearly black.
- **Slanted by the wind.** The fall direction is `(wind · rainDrift, rainSpeed)`
  normalised, so it leans into a gale and stands up in still air. `rainDrift` is
  there because a drop is far lighter than a cloud and is in faster air; without it
  the slant is a degree and a half. Currently 17.7° off vertical.
- **Three gauges.** Length, opacity and fall speed from one hash of the drop's index.
  Rain of a single gauge is a comb. **Not width** — that was the fourth gauge and it
  was the wrong axis to spend weight on: at the size of a square, one step wider is
  *twice* as wide, and the heavy drop came out 10 px across against a length of 15.
  A brick, not a drop.
- **Drawn as squares on the world's lattice, not as a line.** Everything else in the
  scene is, and an unantialiased 5 px quad covers four to six screen pixels depending
  on where between two squares it falls — which is most of why some drops looked
  thicker than their neighbours even at one width. Rounding the *ends* of the line
  instead would fix the width and wreck the slant: a drop is 2–7 squares long, so
  moving an end half a square swings the angle by tens of degrees, and the swing
  changes every frame. Quantising where each square is *drawn* leaves the direction
  exactly as it was. Same row-centre idiom as `DrawShaded`; at 17.7° the streak
  advances a third of a square per row, so the staircase is always joined.
- **A drop leaves the cloud, not the band.** `from` was `base + rainDrop · rain` —
  one height for the whole world. The band's underside is ~100 px below where the
  cloud actually stops, so rain began in open sky on a ruled horizontal line, under
  clouds that plainly were not producing it. `Sky::UndersideAt` marches *up* from the
  bottom of the band and stops at the first edge — the **lowest** cloud, not the
  thickest, because a drop leaves the first underside there is — and root-finds the
  margin between the two straddling samples, exactly as the terrain contour is drawn.
  Without the root-find every drop in the world starts at one of nine heights and the
  top of the rain is a staircase. Measured over 600 columns in a storm: **−433 to
  −220, mean −294**, against the old flat −220 everywhere — 74 px higher on average,
  and varying instead of ruled. Under 2% of columns have no cloud and fall back to the
  band; that share rises through the minute a shower is arriving, when `cover` is
  still climbing towards the storm's 0.94.
- **Sampled every 50 px and interpolated, and floored to that step.** Per drop is the
  12-step march that came out of `ColumnAt` for costing four fifths of it. Anchored
  to the *view* instead of the world, every sample point would move as the player
  walks and the base would ripple in step with their footsteps — the same failure
  `StepLight` records, and the reason `DrawAtmosphere` and `DrawClouds` floor theirs.
- **A drop stops at what is actually there.** It was culled against `terrain::Height`,
  which is a function of the column alone and cannot see a chunk, so rain fell
  straight through anything built. `World::SurfaceProfile` starts from the skyline and
  lets the edits overrule it, and only opens chunks marked `edited` — an untouched one
  holds the field the skyline already read, vertex for vertex, so a world nobody has
  built in pays one walk of the chunk map. The streak is **cut off** at the surface
  rather than dropped whole, so it ends *on* what it hits; testing only the head made
  the rain stop a whole streak-length above the ground. Verified: a slab placed at
  y −76 moves the surface from 146 to −84 while `terrain::Height` stays at 144.
  Lowered only — a column *dug* below the surface still answers with the noise, which
  is what the light solve does with the same skyline and for the same reason.
- **Fixed span.** A drop falls `rainSpan` at `rainSpeed` and is skipped once it passes
  the ground. Tying the cycle to the cloud-to-ground gap tied it to the cloud height,
  which is tied to the weather, so a shower's drops slowed as it passed. The *span* is
  still fixed; only the *start* now follows the cloud.

### 6.7 Cost

| | before | after |
| --- | ---: | ---: |
| noise evaluations, surface, fair | ~75 000 | **17 976** |
| noise evaluations, surface, storm | ~75 000 | **20 688** |
| whole cloud band in view | up to 127 000 | **20 349** |
| **underground, nothing visible** | **39 560** | **~0** |
| grid allocations per frame | 8 | 1 |
| grid copies per frame | 6 (265–452 KB) | **0** |
| rasteriser passes | 6 | **1** |

Where it went:

- **The 12-step weight march is gone from `ColumnAt`** — 62 noise calls back to 14.
  It existed only to decide rain, and rain is the weather's now. Much the largest
  single win, and it came from getting the model right rather than from optimising.
- **One pass instead of six.** `Sky::DrawShaded` walks the grid once and shades each
  cell, which is also what makes the lighting per-cloud. The six nested passes each
  needed their own shifted copy of the same numbers.
- **The field is sampled at `fieldStep` × the world lattice.** A cloud is ~400 px
  across, so 12 px still gives ~33 samples over one, and the rasteriser interpolates
  between them: the shape does not coarsen, only the sampling of it.
- **`StepLight` skips a column whose ground stands above the lit region.** Cloud
  shade is only read at the end of a ray that reached the sky, so a column with no
  sky in it never reads its own. This is the whole cost of the weather while
  underground, which is most of the time.

`DrawRain` returns immediately when it is not raining, and `DrawClouds` when the
band is out of view.

Tying the rain to the cloud paid for itself, and then some:

- **The ground test ran before the view test.** `DrawRain` walks a stretch half again
  as wide as the screen, because a slanted drop starting off the side still crosses it
  further down — so most drops in the loop are off-screen, and `terrain::Height` under
  one costs 8 noise calls. About **4 200 calls a frame** on drops nobody could see,
  against a storm budget of 20 688. Both are plain tests on the same position, so
  which goes first changes nothing but the bill.
- **What it bought instead**: ~46 cloud-underside marches (one per 50 px) at 9 field
  reads each, ~2 000 calls, and a surface profile that is a memoised lookup per column
  plus a scan of whatever chunks have been built in — nothing, in a world nobody has
  built in. Net, the rain is cheaper than it was and lands on the right things.

### 6.8 The day, and what it turns

One angle drives all of it. The sun's elevation is a sine of the phase, and the
light, its colour, its direction and how fast the ground dries are every one of them
a function of that single number. There is no keyframe table: a dawn is not four
authored colours faded between.

```
phase     = frac((time + dayOffset) / dayMinutes·60)   0 = midnight, 0.5 = noon
elevation = sin(phase·2π − π/2)                        −1 midnight, +1 noon
light     = smoothstep(darkAt, litAt, elevation)       [0,1], everything else
```

Both edges sit **below** the horizon's own zero (−0.45 and 0.05), which is what makes
the day longer than the night: light arrives before the sun does and outlasts it.
Measured: **11.7 min of full day, 8.5 min of full night, 114 s per turning.**

`dayMinutes = 24` deliberately is not a whole multiple of `spellMinutes = 5`. At four
spells to a day every dawn would fall at the same point of a spell for ever and the
weather would never once break differently over a sunrise.

**`light` is pinned at exactly 1.0 for 49% of the cycle**, and `travel` at 0, so the
beam is white and high noon is bit-for-bit the sky this world had before there was a
clock in it. Verified. That is a real regression gate and it is why the edges are
where they are.

#### The sunset is in the coefficients already, but not where you would look

The obvious move — thicken the air in the line of sight as the sun drops — is wrong,
and it is worth writing down so nobody retries it. `1 − exp(−rayleigh·airmass)` is
monotonically increasing in every channel, so **more air in the view path makes
white, not red**: airmass 3.5 gives `[214,250,255]`, airmass 14 gives white.

What was missing is extinction of the *incoming* beam. A sunset is red because the
light crossed a long path before it arrived. Same coefficients, read the other way:

```
beam_c = exp(−rayleigh_c · travel)         normalised so the strongest channel is 1
lit_c  = beam_c^(airmass/thickness) · (1 − exp(−rayleigh_c · airmass))
```

Raising the beam to the view's own airmass is what keeps the zenith blue while the
horizon burns — the reddening shows up where the light had furthest to come.

Two things about it that were got wrong first and are easy to get wrong again:

- **`travel` keys to `elevation`, not to `light`.** Golden hour is when the sun is low
  and it is *still broad day*. Keyed to the daylight, the colour peaks at half
  brightness and reads brown.
- **The beam has to fade back to white as the light goes.** Below the horizon
  `travel` is at its longest and stays there, so without the fade the sky keeps its
  sunset all night — and since the whole scene is *also* multiplied by a blue
  moonlight, what comes out is brown. This is the one place two tints meet.

Measured through the whole pipeline, horizon and zenith:

| phase | elevation | light | horizon | zenith |
| ---: | ---: | ---: | --- | --- |
| 0.25 | +0.00 | 0.97 | `[209,172,77]` | `[17,36,71]` |
| 0.30 | +0.31 | 1.00 | `[209,191,106]` | `[17,36,72]` |
| 0.50 | +1.00 | 1.00 | `[209,249,255]` | `[17,36,75]` |
| 0.00 | −1.00 | 0.00 | `[209,249,255]` | `[17,36,75]` |

Midnight and noon share a *colour* and differ entirely in what multiplies it.

#### The daylight must be mixed in exposed space

The single arithmetic decision in the cycle, and the one that makes it look broken if
it is got wrong. Light reaches the screen through `1 − exp(−value·exposure)`, and
daylight sits so far up that curve that it is saturated: **halving the radiance
leaves the screen five per cent darker.** A linear mix gives an afternoon where
nothing happens for hours and then a cliff into night.

So the two ends are exposed first, mixed as *brightnesses*, and put back through the
curve to find the radiance that produces them:

```
lit    = E(night) + (E(day) − E(night)) · light
wanted = lit · Mix(white, beam, 0.35)
value  = −log1p(−wanted) / exposure
```

| light | radiance | on screen |
| ---: | --- | --- |
| 1.00 | `{2.600, 2.800, 3.100}` | `[254,254,255]` |
| 0.75 | `{0.675, 0.564, 0.464}` | `[197,181,163]` |
| 0.50 | `{0.363, 0.341, 0.327}` | `[140,135,131]` |
| 0.25 | `{0.180, 0.184, 0.203}` | `[83,85,92]` |
| 0.00 | `{0.050, 0.060, 0.090}` | `[27,32,46]` |

Two ends rather than one dimmed, because a night is **blue** where the day is
near-neutral. That is what makes a torch read as warm after dark, and it costs
nothing: emission is added to the sky term rather than scaled by it, and `Expose` is
per channel, so a torch stops washing out the moment daylight stops saturating the
curve. No torch value changed.

It lives in `World::StepWeather` and not in the caller, because the inversion has to
use the same `exposure` the light field will read it back through.

#### What the sky module gets is hue and direction, never brightness

Everything drawn before the `lights.Compose()` multiply — the atmosphere, the clouds,
the terrain, the player, the rain, the liquids — is already carrying the day. Scaling
`AirAt` or `CloudTint` by the daylight *as well* squares it: `0.15 × 0.15 = 0.02`, a
black frame at dusk. The four landing sites are all colour or geometry:

| Where | What it takes |
| --- | --- |
| `AirAt` | the beam, raised to the view's own airmass. Colour only. Also colours the rain, which needed no change of its own — as its comment predicted. |
| `CloudTint` | the beam on the sunlight, then the two lights run together as the light goes, so a cloud at night is a flat silhouette. |
| `DrawClouds` | `today_.sun` in place of the deleted `Shading::sun`. It points *below* the horizon after sunset, so the deck lights from underneath at dusk — that falls out, nothing asked for it. |
| `ShadeAt` | eased to `nightShade` as the light goes. |

**`settings_` is never written at runtime.** Only `Configure` may touch it, and that
measures the field over 32 768 samples. The sun's direction comes from `today_.sun`;
moving it by re-configuring would cost more than the rest of the frame together.

`ShadeAt` needs the taper because the shade is a *share* of the daylight held back,
and holding back three quarters of a moonlit sky leaves nothing. Measured: a storm at
midnight holds back 11.7% instead of 78%, and the ground under it exposes to 23
against 26 in the open — darker, and still readable. At noon the taper is exactly 1
and a daytime storm is untouched.

### 6.9 Stars, and why they are the one thing drawn outside the light

Scattered, not placed: one to a cell of a coarse lattice, each one's position,
brightness, colour and twinkle hashed out of the cell it is in — the same trick the
rain uses on its drops. Nothing is stored.

**They are drawn *after* `lights.Compose()`, and they are the only part of the world
that is.** The first version had them under it, between the air and the cloud, which
is where they belong by layering. It does not work, and the arithmetic says exactly
why: the multiply caps everything at the light's own value, and at midnight that is
`[27,32,46]`. So the brightest possible star was `[27,32,46]` on a `[2,5,14]` sky —
and worse, its *colour* went with its brightness. An amber star came out `[22,20,21]`,
which is grey. **A star is a light, not a lit surface**, and the multiply cannot say
that. Outside it the same star is `[217,159,111]`.

The price is that the two things that should hide a star no longer do it by being
drawn on top of it, so both are asked instead:

- **The ground** comes in as the same `weather::Ground` the rain lands on — the
  world's real surface, edits and all. `World::GroundUnder` now serves both.
- **The cloud** is read out of the field at the star's own position, which is exactly
  what being covered by it would have meant. Only for stars inside the band, and the
  `ColumnAt` is hoisted per lattice column, so most stars cost nothing.

Not paid for: a sprite drawn before the multiply and standing against the sky — the
player in mid-air — can have a star in front of it. One square, and about one star in
sixteen frames by area, so it is left.

#### Two fades, and the mistake of having both do the same job

- **The cover fade is read through a curve, not straight.** With a straight
  `1 − cover`, a fair sky at 0.34 cover dimmed *every* star by a third — on top of the
  per-star occlusion already hiding the ones actually behind a cloud. Counted twice,
  and a fair night read as an overcast one. Now `smoothstep(0.55, 0.95, cover)`: clear
  and fair nights keep **100%** of their stars, overcast 39%, a storm 0.2%. The
  per-star test does the real work; this is only for the sky a closed deck seals over,
  where there is no cloud at that point to ask.
- **The ground fade is a height, not an airmass.** Airmass only varies about fivefold
  across the whole visible sky, so a coefficient strong enough to clear the horizon
  takes the top of the screen down with it — the first attempt left 35% of a star's
  brightness sitting on the treeline, which reads as holes punched in the picture. A
  `smoothstep(0, rise, altitude)` saturates: 0% at the ground, 9% at 60 px, full above
  320 px.
- **The cloud edge is faded across, not cut at.** The cloud on screen is rasterised
  from a lattice twelve pixels across and interpolated between, so its drawn edge and
  the field's exact edge disagree — measured, over **2.2% of the cloud area**. Every
  one of those is a star left burning on the rim of a cloud, which is exactly where
  the eye goes. `smoothstep(−cloudEdge, +cloudEdge, margin)` swallows the
  disagreement, and a star dimming as it passes behind the thin edge of a cloud is
  what it should do anyway.

#### Small, and nearly all the same brightness

Two things that had to be tuned against what they look like rather than derived:

- **Three world pixels, not five.** A star is the only thing in the world drawn off
  `config::kPixelSize`. Everything else is a surface standing in the world and
  belongs to its grid; a star is a point at an unreachable distance, and at the size
  of a terrain tile it reads as a tile. It keeps its own grain so it still sits still
  as the view moves.
- **A fifth between the brightest and the faintest**, down from a half. With the full
  range the field reads as noise rather than as a sky — the eye finds the scatter
  before it finds the pattern. The variety moved into the colour instead, and even
  there the two ends are pulled 30% back towards each other.

### 6.10 Humidity, with a memory and no state

`Climate::humidity` was a static function of X with one consumer, and
`Climate::temperature` had none at all. Both are now read by a number a game rule can
be written against:

```
humidity(x) = clamp(climate.humidity + wet·wetGain − drought·dryGain·(0.5 + temperature))
```

**`wet` and `drought` are read backwards out of the clock, not accumulated forwards
through it.** That is only possible because the weather is a pure function of time,
and it is worth saying what it buys: the ground remembers the last shower without the
world keeping any state, so two views of it at the same moment agree and there is
nothing to save. An exponential kernel *is* the leaky integrator `w += (rain−w)·k·dt`
with the accumulator taken out.

- **24 samples over 15 minutes**, half-life 4 min. Not 8: at 1.9-minute spacing
  against a 1.2-minute crossfade on a 5-minute square wave, a coarse comb beats
  against the rain and the reading jitters as the clock moves.
- **Once a frame**, in `Advance`. Both are functions of the moment alone, so
  recomputing them per query would be waste; what varies from place to place is the
  climate they are added to, and that is read at the query.
- Costs a couple of microseconds — the weather at a moment is a table lookup and a
  hash, with no noise in it at all. `HumidityAt` itself costs one `ClimateAt`, which
  is ~10 noise samples, so nothing may walk every column with it.

Measured, a storm passing over `x = 0` (climate humidity 0.50): 0.47 → **0.73** at the
height of it → 0.66, 0.47, 0.35, 0.28 over the twelve minutes after the rain stops →
and back up as night falls and the drying stops.

**Deliberately not fed back into `ColumnAt`.** Rain → humidity → cover → rain is a
loop, and it would make `WeatherAt` — pure by contract, so a forecast agrees with what
is drawn — disagree with the screen. The cloud cover still reads the *static* climate
humidity, and that is the first thing someone will try to change.

### 6.10 The controls, and what nothing does yet

`F8` runs the day on to its next quarter. Two decisions in it worth keeping:

- **It moves an offset, not the clock.** `Sky::Field` reads `time_` in five places, so
  scrubbing it six hours would slide the cloud field several screens sideways and land
  in a different spell of weather. `dayOffset_` shifts the day alone, wrapped to one
  day so a long session cannot walk it out of float precision.
- **It eases in over about three seconds rather than jumping.** What is usually being
  checked *is* the transition, and a jump lands on the far side of it. Asking again
  while one is running queues another quarter.

`F7` already runs the day, since the day is keyed to the same clock the weather is —
40× puts a whole day in 36 seconds.

The query surface is three things: `Sky::Today()` for the whole `Daylight` row,
`Sky::HumidityAt`, and `World::HumidityAt` forwarding it beside `RainAt`.
Deliberately absent: a `DaylightAt(Vector2)`, because daylight will never vary with
position and what a mob spawner actually wants is `LightLevelAt`, which exists; and an
`IsNight()`, because that is a threshold and thresholds belong to the rule that needs
one — mobs want `< 0.2`, crops want `> 0.5`, and baking one number in forces every
rule to share it.

**No sun or moon is drawn.** The sun is a direction the clouds are shaded from and a
colour the light has, nothing more. When a body is wanted, `today_.sun` and
`today_.phase` already say where it is.

### 6.11 What weather still does not do

- **Rain does not feed the water table.** The obvious next coupling and the one to be
  careful with: the liquid automaton is mass-based, so rain adding mass without a
  sink floods the world. It needs evaporation, or a cap per surface cell.
- **Nothing gets wet.** `World::RainAt` answers; nothing reads it. Torches that
  gutter, ground that turns slick, crops that grow.
- **No snow.** `Climate::temperature` exists and falls with elevation; rain above a
  snow line should fall as snow and lie. Half of it now has a consumer — the drying
  of the ground reads it — so the field is at least exercised.
- **Rain does not splash.** A drop's fall ends exactly on the surface it hits, and
  nothing is drawn at the contact. A couple of squares thrown up from the impact point
  is the cheapest thing left that would make the landing read as a landing.
- **A dug column still reports the surface the noise describes.** `SurfaceProfile`
  only lets edits *lower* it, so rain stops a few pixels above the floor of a fresh
  pit. Fixing it properly means fixing `Skyline`, which the light solve shares — so it
  is one change that would straighten both.
- **A cloud's shadow stays directly beneath it** while the cloud itself is lit from
  the side. Ground shade is one figure per column and marches straight down, so at a
  low sun the two visibly disagree; at midday the sun is near vertical and they
  agree. The fix is an offset of `(ground − cloudBase) · sun.x / −sun.y` inside
  `ShadeAt`, capped as the sun nears the horizon — but it touches the light path and
  widens the column range the solve must cover, so it is its own change.
- **The cloud shape is not domain-warped.** The best-looking thing left undone, and
  rejected on cost: displacing the sample position by a second slowly-drifting field
  is two extra fbm evaluations per sample, ~+40% on a field the light path reads
  through `ShadeAt`. It also cannot be applied in `DrawClouds` alone — there is one
  `Field` so the calibration and the drawing cannot disagree, and a warped cloud with
  an unwarped shadow is worse than a still one.

---

## 7. What is not done yet

Roughly in the order the value arrives.

**Biomes.** Everything in §3.5. Half the substrate now exists — `terrain::ClimateAt`
answers temperature and humidity per column, elevation included, and the sky already
reads it. What is missing is the table and the selection. The shape the code should take: a `Biome` table
in the manner of `kElements`, each row a set of `SurfaceSettings` and
`CaveSettings` overrides, selected by climate fields and blended between
neighbours over a border band. Blending is the hard part — a hard switch puts a
cliff on every biome border. Blend the *parameters*, not the resulting heights.

**Ravines.** Long narrow vertical gashes breaking the surface. Cheap: the shaft
layer with a much lower frequency, much greater reach and a width that grows
downward instead of shrinking.

**A wide cave mouth is not ground.** `World::Skyline` follows the sky *down* into
an opening, which is right for lighting and wrong for planting: what it reports
inside a wide mouth is a ledge halfway down a shaft, level, with level ground
either side, so every footing test `flora` has passes and a tree grows on the
wall of a cave. `flora::Ground` now carries how far each column's top lies below
`terrain::Height` and `LayerRules::rootLimit` rejects the rest. Grass is not yet
covered by it.

**Ore districts.** Veins know their depth band and their distance from a cave wall,
and nothing else. Minecraft's `vein_toggle` ties them to a second low-frequency
field so ore arrives in rich districts rather than evenly. One more term in
`World::GeneratedValue`, on the model of the wall bias already there.

**Emerald needs biomes to mean anything.** It is in as the shallowest and rarest
ore, which keeps the shape of the idea, but the rule behind it — mountains only,
denser with altitude — needs `band.peak` to be relative to the surface rather than
an absolute height. That is the same thing a soil layer needs (see surface cover
below).

**Surface cover.** A soil and grass layer over the rock, thickness from the
biome. Structurally this is a second element with an `InsideGround` spawn and a
band measured from `Height(x)` rather than from an absolute Y — which the
element table cannot currently express, since bands are absolute.

**Structures.** Anything placed rather than sampled — a hut, a mineshaft, a
chest. This breaks the pure-function property, so it needs its own pass with a
deterministic per-chunk seed, run after the density field is built.

**Per-column height cache.** `Height` is 1D but is re-evaluated per vertex.
`World::Skyline` already memoises per column; the same treatment for `Height`
would remove the eight octaves of the surface from every sample. Only worth doing
if chunk streaming starts to show — it does not today (§4.4) — and it has to live
in `World`, not in `terrain`, to keep the generator stateless. Note that it is
only valid while `warpAmplitude` is zero, since the fold makes `Height` depend on
the height it is read at.

**Inspecting it from inside the game.** The offline probes are now in the binary —
`--probe`, `--caves`, `--ore`, `--settle`, `--covers`, `--column`, `--surface`,
`--sun`, `--tones` — so they measure the world these settings describe rather than
a copy kept in step by hand. What the game itself has: `F` for free flight, which suspends
gravity and collision so a cave system can be followed through the rock; `F6` to
skip the light multiply so the world can be read unlit; `F7` to run the weather and
the day forty times over; and `F8` to run the day on to its next quarter, eased in
rather than jumped, because what is usually being checked *is* the transition. A
live settings panel would be the next step; raygui is already a dependency.

---

## Sources

- [Biome – Minecraft Wiki](https://minecraft.wiki/w/Biome)
- [Cave – Minecraft Wiki](https://minecraft.wiki/w/Cave)
- [Noise router – Minecraft Wiki](https://minecraft.wiki/w/Noise_router)
- [World generation – Minecraft Wiki](https://minecraft.wiki/w/World_generation)
- [Custom world generation/noise settings – Minecraft Wiki](https://minecraft.fandom.com/wiki/Custom_world_generation/noise_settings)
- [The World Generation of Minecraft – Alan Zucconi](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/)
- [Minecraft devs explain the new cheese and spaghetti cave generation – PCGamesN](https://www.pcgamesn.com/minecraft/caves-and-cliffs-snapshot-21w06a)
