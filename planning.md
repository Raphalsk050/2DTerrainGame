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
      chambers   isotropic, low frequency        the rooms
      galleries  stretched horizontally          long walkable corridors
      crawlways  narrow, tighter                 links between galleries
      shafts     stretched vertically            the way in from the surface

  Solidity(x, y) = min(Depth, -chambers, -galleries, -crawlways, -shafts)
  Density(x, y)  = kSurfaceLevel + Solidity / kDensitySpan      -> [0, 1]
```

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

**Coverage figures are measured, not guessed.** `regionCoverage` and
`chamberCoverage` are shares in [0, 1]. `terrain::Calibrate` samples the field
and takes the quantile that achieves them, so reshaping the noise does not
silently change how much of the world is hollow. Same approach the element table
already uses for ore veins in `World::CalibrateSpawn`.

### 4.3 What it measures

Numbers from the offline probe, over 24 000 px of surface and a 6000 × 4200 px
flood fill, with the settings as authored in `main.cpp`:

| | |
| --- | --- |
| Relief | 271 px, about 90 px of swing per screen of travel |
| Mean surface slope | 10°, steepest single step 14 px (a terrace riser) |
| Open ground | 5.7% of columns; one cave mouth every ~680 px, ~38 px wide |
| Cave volume | 15% of the rock just under the crust, rising to ~27% at 3500 px; 21% overall |
| Reachable from the sky | 97.8% of all open space, without digging |
| Sealed pockets | 2.2% of open space, largest 531 lattice cells |
| Vertical clearance | median 36 px; 67% of runs walkable upright, 96% crouchable |

The last two rows are the ones worth watching when the settings change.
Connectivity is what makes the caves explorable, and clearance is what makes them
walkable — the character is 26 px tall and 14 px crouched, so a median of 36 px
means most passages are walked and the crawlways are genuinely crawled.

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

---

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

Lives in `src/weather.h` / `src/weather.cpp`, with the climate half of it in
`terrain::ClimateAt`.

### 6.1 One field, three consequences

Clouds are not scenery painted behind the world. There is one cloud field, and
everything else is read off it:

```
                  ┌──────────────────────┐
  front (drifts) ─┤                      ├─→ what is drawn in the sky
  climate  ───────┤   cloud field        ├─→ light::Medium::cover  → shadow on the ground
  elevation ──────┤                      ├─→ rain, and how hard
                  └──────────────────────┘
                             ↑                        │
                             └────────────────────────┘
                          a raining column hangs lower and darker
```

Because it is one field, the couplings cannot come apart. There is no cloud
appearance to keep in step with a separate rain flag and a separate shadow map;
there is one value and three readings of it.

**The three inputs are one term each.**

| Input | Term | Why |
| --- | --- | --- |
| The front | `frontInfluence` | A broad slow field drifting past, so weather arrives, sits, and passes. Without it the sky holds the same cloud for ever and rain is a property of the map instead of an event. |
| The climate | `humidityInfluence` | Wet ground is cloudier than dry ground. This is the biome half. |
| The land | `terrain::ClimateSettings::humidityLift` | Air pushed up over a rise cools, cool air holds less water, so it condenses. High ground stands in cloud on a day the plains are clear. |

They are **added, not multiplied**. Multiplying would let a dry region hold the sky
clear through any front that crossed it, and a wet one keep cloud through every
clear spell; adding lets each shift the answer without being able to overrule it.

**Rain is not rolled for.** `rain = smoothstep(rainAt, rainFull, cover)` — it is
what a thick enough sky does, which is the "cloud formation favours rain" rule
stated once and in one place. And it is read from the *regional* cover rather than
from the cloud directly overhead: rain that switched off in the gap between two
clouds would read as a fault.

### 6.2 The shadow

`light::Medium` gained a `cover` array beside its `skyline`, and it is the same
kind of fact: both are properties of what stands over a column, found once and read
by every ray that asks for the sky. The skyline says whether the sky can be seen at
all; the cover says how much of it is getting through. `Field::SkyAt` applies it at
the end of a ray that reached the sky, so a cloud shades the ground beneath itself
and nothing else.

It could not have gone in `Sky::radiance`: that is one number for the whole world,
and the whole point of a cloud is that it shades the ground under itself and not
the field beside it.

Two things were wrong on the first attempt, and both were only visible once the
shadow was drawn next to the cloud casting it:

- **Cover was read at one height.** Whether a cloud happened to cross that exact
  line had nothing to do with whether it was there, so the ground had shadows under
  clear sky and clouds with no shadow beneath them. It now takes the thickest cloud
  anywhere down the column. The maximum rather than a sum, because the band is thin
  enough that any cloud in it is opaque — a second layer behind the first does not
  darken the ground further.
- **The band profile scaled the field down.** A sky asked for a third full came out
  at 8%. The profile is now a penalty on the *cutoff* instead, so the middle of the
  band is exactly as full as it was asked to be and only the edges taper.

### 6.3 The knobs, and what they cost

`cover` is a genuine share of sky, on the same measured basis as ore probability and
cave region coverage: `Sky::Configure` samples the cloud field and stores a table of
quantiles, so the number means what it says. Perlin noise crowds hard around its
midpoint, and a cutoff of `1 - cover` leaves a sky asked for a third full reading as
very nearly empty — the same trap the ores fell into, and the samples have to be an
irrational multiple of one feature apart for the same reason.

Note that the share of *ground in shadow* runs about half again the `cover` figure,
because a column counts as shaded if cloud stands anywhere in the band above it.

Measured over 60 000 px of world at one instant, with the settings as authored:

| Coupling | Correlation |
| --- | --- |
| humidity → overcast | +0.48 |
| elevation → humidity | +0.42 |
| elevation → overcast | +0.31 |
| overcast → rain | +0.43 |
| overcast → shade | +0.42 |

At `cover = 0.28`: 29% of the sky is cloud, 48% of columns have cloud somewhere
above them, and 8% of the world is raining at any moment. Over one spot the front
carries the sky from 13% to 58% overcast in ten minutes, so rain arrives and passes.

Cost is about 25 000 noise samples a frame — the cloud grid over the view plus the
shade for each lit column. `Column` exists to make that affordable: everything that
depends on the horizontal position alone is computed once per column rather than
once per sample, which for a grid of tens of thousands of samples is the difference
between the sky being free and it being the most expensive thing in the frame.

### 6.4 Rain, as it falls

Drops are hashed out of their own index rather than stored, so nothing survives
between frames and two views of the same world at the same moment agree.

One bug worth remembering: the fall was originally measured from the cloud base to
the ground. The cloud base hangs lower the harder it rains, so a drop's cycle
stretched as a shower passed and its speed fell away with the rain. It now falls a
fixed `rainSpan` at a fixed `rainSpeed`, and a drop that reaches the ground simply
stops being drawn — which also means the number of drops in the air over a column
follows how far they have to fall, so a deep valley in the rain is full of them.

### 6.5 What weather does not do yet

- **Rain does not feed the water table.** The obvious next coupling, and the one to
  be careful with: the liquid automaton is mass-based, so rain that added mass
  without a sink would eventually flood the world. It needs evaporation, or a cap
  per surface cell, before it is turned on.
- **Nothing gets wet.** Rain is a number `World::RainAt` already answers; what is
  missing is anything reading it. Torches that gutter, ground that turns slick,
  crops that grow.
- **No snow.** `Climate::temperature` exists and falls with elevation; rain above a
  snow line should fall as snow and lie on the ground.
- **One sky.** No day and night, so `SkyLight` is constant and the shade is the only
  thing that changes. A time of day would multiply into the same place.

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

**Aquifers.** Water currently spawns from one global band with a fixed top. A
per-region water table — its own low-frequency field giving a local surface
level — is what makes one cave flooded and the next one dry, and it is how
Minecraft avoids a world where every cave below y is full.

**Ravines.** Long narrow vertical gashes breaking the surface. Cheap: the shaft
layer with a much lower frequency, much greater reach and a width that grows
downward instead of shrinking.

**Ore districts, and ore that follows the caves.** Veins know their depth band and
nothing else. Minecraft's `vein_toggle` ties them to a second low-frequency field
so ore arrives in rich districts rather than evenly. Better still for gameplay:
bias veins *towards* cave walls, so exploring pays more than tunnelling blind.
Both are one more term in `World::GeneratedValue`.

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

**Inspecting it from inside the game.** There is an offline probe (statistics and
one-pixel-per-world-pixel renders) that was written to tune this and is not in the
repository. The two things the game itself now has are `F` for free flight, which
suspends gravity and collision so a cave system can be followed through the rock,
and `F6` to skip the light multiply so the world can be read unlit. A live
settings panel would be the next step; raygui is already a dependency.

---

## Sources

- [Biome – Minecraft Wiki](https://minecraft.wiki/w/Biome)
- [Cave – Minecraft Wiki](https://minecraft.wiki/w/Cave)
- [Noise router – Minecraft Wiki](https://minecraft.wiki/w/Noise_router)
- [World generation – Minecraft Wiki](https://minecraft.wiki/w/World_generation)
- [Custom world generation/noise settings – Minecraft Wiki](https://minecraft.fandom.com/wiki/Custom_world_generation/noise_settings)
- [The World Generation of Minecraft – Alan Zucconi](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/)
- [Minecraft devs explain the new cheese and spaghetti cave generation – PCGamesN](https://www.pcgamesn.com/minecraft/caves-and-cliffs-snapshot-21w06a)
