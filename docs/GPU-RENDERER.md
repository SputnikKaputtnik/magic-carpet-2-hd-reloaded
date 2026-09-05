# The D3D11 GPU renderer

Magic Carpet 2 draws its world with a software rasteriser: every terrain
triangle, every sprite and the sky are written pixel by pixel into a single
8-bit palette index buffer. This fork moves that work to the GPU while keeping
the original output — the goal was never "looks close enough", it was
"reproduces the software renderer, pixel for pixel, and is much faster".

Everything is behind feature flags. With all of them off you get exactly the
original software path.

## What runs on the GPU

| Stage | Flag | Notes |
|---|---|---|
| Palette resolve and upscale | `gpuPalettePresentation` | 8-bit indices to RGBA on the GPU |
| Terrain and world polygons | `gpuWorldGeometry` | one draw call per world frame |
| World sprites and billboards | `gpuSprites` | same vertex stream as the terrain |
| Exact destination blends | `gpuExactBlend` | rasterizer ordered views, default on |
| Sky | `gpuSky` | requires `gpuExactBlend` |
| Exit warp blur | with `gpuWorldGeometry` | see "The exit warp" below |

Each flag can be switched off independently, which makes it possible to compare
the GPU and software paths directly and to bisect a visual difference to the
stage that caused it.

## Measured results

1280x720, uncapped, on a GeForce RTX 4070 Ti. "World CPU time" is the time the
game spends producing one world frame.

| | Software | GPU |
|---|---|---|
| Level 1 | 11.85 ms / 71.7 FPS | **0.40 ms / 407 FPS** |
| Level 8 | 15.86 ms / 55.6 FPS | **0.59 ms / 385 FPS** |

Image accuracy against the software renderer, same simulation tick:

| | differing pixels | clearly differing (>24/255) |
|---|---|---|
| Level 1 | 1.44 % | **16 pixels** |
| Level 8 | 6.35 % | 0.78 % |

The remaining difference is a sub-pixel sampling offset, not a computation
error: for 89 % of the differing pixels the GPU value is what the software
renderer produced at a *neighbouring* pixel. See "Known limitations" below.

## How it fits together

The renderer keeps the original painter's algorithm. There is no depth buffer
and no sorting: terrain triangles, sprites and the sky all go into **one**
vertex buffer in submission order, and D3D11 guarantees primitive order in the
output merger. That is what makes the draw order between terrain and sprites
exact — it is the same order the software rasteriser used.

* `GpuRenderDevice` — device, swap chain and back buffer.
* `GpuPalettePresenter` — resolves palette indices to RGBA and scales the frame.
* `GpuWorldRenderer` — the world itself: terrain, sprites, sky, blend tables.

The pixel shader reproduces the rasteriser's shading modes literally, including
the lookup tables the original indexes into (`x_BYTE_F6EE0_tablesx`: 64 shade
ramps, then a 256x256 destination blend table).

### Destination reads

Several pixel modes read the pixel they are about to overwrite — translucent
water (terrain mode 0x1A) and the sprite shadow modes. A normal render target
cannot do that in primitive order, so the index target is written through a
**rasterizer ordered view** (ps_5_0). Without ROV support the renderer falls
back to blending against the background layer, which is what earlier builds
did, and the destination-reading sprite modes stay on the CPU.

Note for anyone touching device creation: drivers only report ROV support on a
feature level 11_1 device, so `GpuRenderDevice` asks for 11_1 first and falls
back if the runtime rejects it.

### Sprites

The eight blit cases in `DrawSprite_41BD3` are not eight sprite variants — they
are the octants of the *camera roll*, identical for every sprite in a frame.
On the GPU they collapse into a single rotation. With `S` and `C` being sine
and cosine of the roll:

```
column(u,v) = screenY + u*C + v*S
row   (u,v) = screenX - u*S + v*C
```

Note that the blit is transposed: `screenX` is the destination row and
`screenY` the column.

Sprite pixels live in a per-frame R8 atlas (shelf packer, one texel replicated
border per entry, cached by source pointer) and are marked by a bit in the
packed vertex field.

### The exit warp

At the end of a level the game warps the player to the exit and puts a blur over
the world. Its structure is easy to misread, and both misreadings cost time:

1. The world is rendered **into the blur buffer**, not onto the screen.
2. The buffer is blended against the screen, which still carries the *previous*
   frame, through the same 256x256 table the translucent pixel modes use.
3. The blend loop then leaves `DrawWorld` through the jump at its end — which
   sits *past* the normal world pass. So during the warp the world is drawn
   **once**, and the blended image is the frame.

The cost follows from step 1: because that pass targets the blur buffer rather
than the screen, the GPU path declines it and the whole world falls back to the
software rasteriser for as long as the warp lasts. At 3840x2160 that is 60 fps
against 3.5.

The GPU version draws the world normally — which is what keeps the frame rate —
and applies the effect afterwards, **in the palette presenter, behind the
palette resolve**. That placement is the result of a failed attempt worth
recording: in index space there is no fractional blending. The 256x256 table is
one fixed mix, and it pulls weakly — measured against the game palette, a lookup
lands only about a fifth of the way between its operands, because the result has
to be an index the palette actually contains. Dithering around that limitation
(update only a fraction of the pixels per frame) turns the missing fractions
into visible grain, an ordered matrix into a standing grid. In RGB, after the
palette resolve, `lerp` simply exists.

The presenter keeps a trail buffer at game resolution and moves it towards the
current world each frame by `1 - exp(-dt / tau)` — an exponential decay whose
constant is a **duration**, so the trail keeps its length whatever frame rate
the renderer reaches. The naive alternative, blending against the previous
frame, fails exactly there: the software warp looked forceful because at 3.5 fps
its "previous frame" stood 285 ms of camera travel apart, and at 60 fps that
distance collapses to nothing. The displayed image is `lerp(world, trail,
strength)`; CPU-drawn pixels (HUD, text) stay out of the mix, which is the same
order the software warp has — it blends before the HUD is drawn.

Because the effect blends against earlier frames, **it is invisible while the
camera stands still** — the trail converges to the current image. Any test of it
has to move the camera.

The defaults (strength 85%, decay 1400 ms) were tuned by eye against the
original at 4K. A derivation from the software warp's arithmetic (20% of an
~350 ms old frame) is far too subtle at a smooth frame rate; matching how the
warp *felt* takes far more than matching what its code computed.

The engine requests the effect through the world renderer
(`SetWarpBlurEnabled`), which only carries the parameters; the presenter picks
them up at present time. Both gates of the software warp stay in charge, so the
effect fires only during the actual exit warp sequence. `--blur_on_cpu` forces
the software path for comparisons; the availability flag flows the other way,
from the presenter's `Initialize` into `SupportsWarpBlur()`.

## Known limitations

* **Sub-pixel sampling offset.** The software rasteriser starts each scanline
  with the attribute value of the *unrounded* span edge and then steps in whole
  pixels, so its sample sits `frac(xEdge) * dA/dx` away from the exact value —
  a per scanline offset that vertex data cannot express. Only emulating the DDA
  per scanline would remove it. Adding its nominal expected value (half a
  horizontal step) once per triangle was tried and measured *worse*.
* **The exit warp trail lives in RGB, not in the palette.** During the warp the
  world's colours leave the palette; the original's coarse colour snapping in
  the blur is smoothed out. `--warp_strength` / `--warp_decay_ms` tune it.
* Explosions and particles still use the sprite path rather than real alpha
  blending.
* Minimap markers are not scaled with the UI.

## Development notes

Two things cost real time during development and are worth knowing:

* **A/B dumps are contaminated by mouse movement.** `--dump_world_frame N`
  renders a specific simulation tick, but yaw and pitch stay mouse controlled,
  and D3D11 needs a real window even with `--hide_graphics`. Moving the mouse
  during a comparison run changes the view direction and therefore the whole
  image. Starting the process minimised keeps the pointer out of the window
  (`Start-Process -WindowStyle Minimized`); a reference that differs from an
  earlier run of the same build and configuration is contaminated, not a
  regression. Repeat any surprising result before suspecting the code.
* **Renderer regressions only pass with a 640x480 configuration.** The harness
  tolerates one differing pixel per frame, and the HD and original software
  renderers diverge more than that at higher resolutions.
* **`--hide_graphics` switches the world output off**, and with it
  `--dump_world_frame`. A run that produces no dump usually has this switch, not
  a level that was never reached. `--set_level` skips the menus on its own;
  `--play_file` alone does not and waits in them.
* **Frame dumps are not reproducible pixel by pixel when the camera moves.** The
  dump fires on a simulation tick, but the camera interpolates per *rendered*
  frame, and how many of those have elapsed depends on machine load. Repeat runs
  of a moving camera differ by around 20 % of their pixels. Comparisons that need
  motion have to use an aggregate — the mean palette index separates conditions
  well, spreading by at most 0.6 within one.

### In-game keys

The port adds a few keys next to the original ones (F1-F10 are the spell keys
in the original, with and without Alt):

| Key | Purpose |
|---|---|
| `F12` | switches the terrain/sky texture set between the CD set (32 px blocks, 256 px sky) and the high-res set (128 px blocks, 1024 px sky) at run time. Sprites keep the set they were loaded with. Needs the high-res folder from `highResGraphicsFolder`. |
| `Shift+F11` | cycles the view distance |
| `Shift+F12` | toggles the fps counter |

### Useful switches

| Switch | Purpose |
|---|---|
| `--dump_world_frame <tick>` | writes the world frame at a simulation tick, plus raw palette indices and the shading table |
| `--force_roll <0..2047>` | pins the camera roll so rolled frames can be compared deterministically |
| `--cull_mode <0..2>` | triangle winding rejection; 2 is the default and matches the software rasteriser |
| `--profile_renderer` | per stage timings and frame rate to the log |
| `--force_blur` | turns the exit warp on during ordinary flight, without reaching the exit |
| `--skip_blur` | leaves the warp out entirely |
| `--blur_on_cpu` | keeps the warp on the software path even when the GPU could take it |
| `--warp_strength <0..100>` | share of the trail in the displayed image during the warp (default 85) |
| `--warp_decay_ms <ms>` | trail length as a duration, frame rate independent (default 1400) |
| `--force_level_end <tick>` | completes the level at a simulation tick |
| `--toggle_hd_textures_at <tick>` | performs the F12 texture set switch at a simulation tick, so the switch can be verified by frame dumps |

A moving, repeatable camera — which the warp needs — comes from playing back a
recording into a level:

```
remc2 --set_level 0 --play_file remc2-regression-test/Levels-1-5-Recording.bin \
      --dump_world_frame 120 --profile_renderer
```
