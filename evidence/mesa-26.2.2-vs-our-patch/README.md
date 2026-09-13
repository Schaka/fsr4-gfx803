# Our patch against stock Mesa 26.2.2

Stock Mesa 26.2.2 contains upstream merge request 41178, which lowers NIR's software `sdot_4x8`
expansion to `imul24_relaxed`. This directory holds the measurement of that driver against
`patches/mesa-26.2.2-nir-imul24-int8.patch`.

Measured 2026-09-13 on an AMD RX 470 (Polaris10, GCN4), in Pragmata.

## Result

Real gameplay, loaded from a save, upscaler on every frame. One identical scene, 100-second window,
1280x720 upscaled to 1920x1080, `FSR4_DOT_MODE=i32`, FSR4 4.0.2 INT8 upscaler DLL.

| run | driver | frames | mean ms | median ms | fps |
|---|---|---:|---:|---:|---:|
| A | Mesa 26.1.6 + this patch | 3322 | 30.27 | 30.16 | 33.0 |
| B | Mesa 26.2.2 + this patch | 3316 | 30.38 | 30.27 | 32.9 |
| C | Mesa 26.2.2 stock | 1907 | 52.93 | 52.93 | 18.9 |

The patch makes frames 1.75 times faster, 22.5 ms per frame. Mesa 26.2.2 and 26.1.6 perform the same
with the patch: A and B differ by 0.36 percent, which is run-to-run noise.

With FSR4 4.1.1 the patch makes frames 2.28 times faster. See `../fsr-4.0.2-vs-4.1.1/`.

## Why upstream 26.2.2 does not reach the patch

Upstream rewrites the lowering of `nir_op_sdot_4x8_iadd`, which exists only if a shader contains
`OpSDot`. These shaders contain none. In `i32` mode, vkd3d-proton's `emit_i8_dot_instruction` emits
the decomposition itself as `bfe` plus `OpIMul` plus `OpIAdd`, so NIR does not build that node. This
patch matches the expanded form.

Count the `OpSDot` instructions:

```bash
for f in data/spirv/*.spv; do spirv-dis --no-color "$f"; done | grep -cE "Op(SDot|UDot|SUDot)"
```

The count is zero.

`FSR4_DOT_MODE=dot` emits `OpSDotKHR` and uses the upstream path. In the SDK sample, stock Mesa
26.2.2 with `dot` comes within about 7 percent of this patch with `i32`.

## Log evidence

`logs/log_excerpts.txt` carries, for each run:

* the upscaler path, `FSR31FeatureDx12`, with no `FSR2FeatureDx12_212`;
* `FSR4ModelSelection`, which shows the FSR4 model was loaded;
* the render resolution, 1280x720 in every run;
* `SyncInterval: 0`, which shows vsync was off;
* the total frametime line count.

All three runs used the same save and the same scripted gamepad input, so they measure the same
scene.

## Reproduce it

See `../../REPRODUCE.md`, and `../../notes/HEADLESS_GAME.md` for the game rig. Quote the mean over a
long window.
