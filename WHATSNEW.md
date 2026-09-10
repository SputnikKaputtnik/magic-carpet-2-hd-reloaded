# What's new

## Unreleased

* **Merged upstream `development` (52e0c1b, 48 commits)** - the network rework,
  multiplayer scores, popup messages and "fix load game" from the base project.
  Upstream's renames (`ColourLookupTable_F6EE0`, `ProjectionVertex`,
  `ptrMemoryBuffer_E9C3C`) are applied to this fork's files. The GPU world frame
  is byte-identical to 0.8.2 after the merge (0 of 921600 indices differ).
* **`--test_renderers` switches the GPU stages off by itself.** The renderer
  regressions compare the HD software renderer against the original one on the
  CPU; since the stages default to on (0.8.1) the HD image lived on the GPU and
  every pixel differed - the suite had been failing silently, including against
  the 0.8.2 release. The mode now forces the stages off whatever the
  configuration says. 16/16 with the unchanged test configuration.

## 0.8.2 (2026-09-06)

Fixes and changes since 0.8.1. Findings from the external code review of the
D3D11 renderer are referenced as F1..F8.

### GPU renderer

* **F1** – The terrain atlas upload no longer reads past the end of the
  texture buffer with high-res (128 px) textures. The upload is clamped to
  the buffer that owns the atlas; an unknown buffer uploads nothing instead
  of reading out of bounds.
* **F4** – The terrain atlas on the GPU is invalidated whenever the block
  graphics are reloaded (day / night / cave sets), so a graphics change no
  longer leaves the previous set on screen.
* **F5** – The 1024 texel sky no longer repeats every 256 texels: the
  column steps are accumulated modulo the texture size, as the software
  renderer does. Sky rows are bit-identical to the software renderer at
  roll 0 / 40 / 2000; the 256 texel sky is unchanged.
* **F3** – Sprite atlas lookups are clamped to the sprite's own atlas entry.
  A strongly minified, rolled sprite could read texels of the neighbouring
  entry.
* The sprite fog shade is computed in 64 bits. With the wider fog band it
  overflowed at view distance 3 and 4; the GPU rejected those sprites and
  the CPU drew them with a garbage shade against the cleared viewport -
  beige creatures showing through terrain.
* Every sprite the GPU hooks hand back to the CPU blit is logged
  (`SpriteReject ...`, first 50); such a fallback is a visible defect.
* The shadow pass of big animated sprites is consumed as a no-op on the GPU
  path, matching the original (whose big-sprite blit never drew it), instead
  of being handed back to the CPU blit.

### Rendering

* The distance fog band scales with the view distance. The original fades
  from 15 to 19 tiles of a 20 tile range; at view distance 4 the band had
  shrunk to one tile right at the cut-off and the landscape popped in.
  Configurable via `gameDetail.fogStartFraction` (0.75) and
  `gameDetail.fogEndFraction` (0.95); bit-identical to the original
  constants at view distance 1.

### Simulation

* Mana spheres show the colour of their real owner. The sprite index that
  encodes owner colour and size was only refreshed while a sphere moved and
  while an inherited flag was clear, so a resting sphere kept its old colour
  after an owner change while balloons of the real owner collected it. The
  refresh now runs once per tick.

### Features

* **F12** switches the terrain and sky texture set (CD 32 px / high-res
  128 px) at run time. Sprites keep the set they were loaded with.

### Diagnostics

* `--dump_world_frame <tick>` also writes the raw palette indices
  (`WorldFrame-<backend>.idx`) and the shading tables (`Tables.bin`).
* `--toggle_hd_textures_at <tick>` for deterministic texture-set dumps.

### Known open issues

* Fogged sprites can render too dark on the GPU (shade row ~30 instead of
  ~12 for the same texel); cause not yet isolated.
* Gouraud shaded terrain (raster mode 5) deviates from the software renderer
  in some levels (Level 8: ~22 % of pixels).
* Review findings F2 (CPU fallback sprites lose painter order), F6 (colour
  index 0 treated as "not drawn by the CPU"), F7 (transparent UI blends
  against the cleared viewport) and F8 (8-bit warp history quantisation)
  are open. F6 and F7 share a fix: read the world indices back before the
  UI is drawn.
