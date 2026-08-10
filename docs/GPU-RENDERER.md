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

## Known limitations

* **Sub-pixel sampling offset.** The software rasteriser starts each scanline
  with the attribute value of the *unrounded* span edge and then steps in whole
  pixels, so its sample sits `frac(xEdge) * dA/dx` away from the exact value —
  a per scanline offset that vertex data cannot express. Only emulating the DDA
  per scanline would remove it. Adding its nominal expected value (half a
  horizontal step) once per triangle was tried and measured *worse*.
* Explosions and particles still use the sprite path rather than real alpha
  blending.
* Minimap markers are not scaled with the UI.

## Development notes

Two things cost real time during development and are worth knowing:

* **A/B dumps are contaminated by mouse movement.** `--dump_world_frame N`
  renders a specific simulation tick, but yaw and pitch stay mouse controlled,
  and D3D11 needs a real window even with `--hide_graphics`. Moving the mouse
  during a comparison run changes the view direction and therefore the whole
  image. Repeat any surprising result before suspecting the code.
* **Renderer regressions only pass with a 640x480 configuration.** The harness
  tolerates one differing pixel per frame, and the HD and original software
  renderers diverge more than that at higher resolutions.

### Useful switches

| Switch | Purpose |
|---|---|
| `--dump_world_frame <tick>` | writes the world frame at a simulation tick, plus raw palette indices and the shading table |
| `--force_roll <0..2047>` | pins the camera roll so rolled frames can be compared deterministically |
| `--cull_mode <0..2>` | triangle winding rejection; 2 is the default and matches the software rasteriser |
| `--profile_renderer` | per stage timings and frame rate to the log |
