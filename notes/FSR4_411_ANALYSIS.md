# FSR4 4.1.1 on the RX 470: where the upscaler time goes

Working notes behind `tools/fsr4_layer` and `tools/fsr4_tune`. Measurements come from an RX 470 and
an RX 570 (Polaris) and from a Vega 56, in Pragmata at 1280x720 to 1920x1080 with FSR 4.1.1b.

The shipping tools are in `tools/fsr4_tune/` and `tools/fsr4_layer/`. The raw session data stays
local in `archive_local/fsr4_411_re/`: the captured weight block, the per-pass error sweeps, the
profiles, and the scratch benchmarks.

## DLL structure

The 4.1.1b upscaler DLL (md5 `15103d8c121636b1c3ef1184357b677e`) contains 1,124 DXIL compute
shaders. They implement one network, `fsr4_model_v07_fp8_no_scale`:

1. `prepass`: resamples the render to output resolution, reprojects history with a 9-tap
   Catmull-Rom filter, and runs a 2x2 stride-2 int8 layer into a 16-channel tensor at half output
   resolution.
2. `pass1` to `pass12`, each with a `_post` shader: a U-Net at 1/2, 1/4 and 1/8 of output resolution.
3. The output head (no model name string): a stack of int8 layers at 960x540 with weights baked as
   constants. It predicts sigmoid blend weights for each 2x2 output block, filters the color input
   with 3x3 taps, and writes `rw_mlsr_output_color` and `rw_history_color`.
4. `fsr_rcas_pass`: RCAS sharpening.

Each pass exists in 9 builds. Three builds call `AmdExtD3DShaderIntrinsicsUAV` and do not run on
Polaris. The other six are three tensor-size buckets, each stored twice. The bucket for 1920x1080
output uses tensors of 960x540, 480x270 and 240x135. The network passes read their weights from
`InitializerBuffer` at run time.

The FSR4-patched dxil-spirv command-line tool builds from
`vkd3d-proton-build/subprojects/dxil-spirv` with `-DDXIL_SPIRV_CLI=ON`.

## Measured cost in Pragmata

Setup: RX 470, Mesa 26.2.2 with the patch, FSR 4.1.1b INT8, 1280x720 to 1920x1080, one dispatch per
pass per frame. Source: vkd3d-proton timestamp profiler over a 60 s window (1,836 frames). The
profiling build adds about 2.3 ms per frame, so the shares are more reliable than the absolute times.

| stage | tensor | ms per frame | share of FSR4 |
|---|---|---:|---:|
| output head | 960x540 | 2.91 | 20% |
| pass9 | 240x135 | 1.32 | 9.0% |
| pass1 | 960x540 | 0.98 | 6.7% |
| pass2 | 960x540 | 0.97 | 6.7% |
| pass12 | 960x540 | 0.96 | 6.6% |
| pass5 | 480x270 | 0.94 | 6.4% |
| prepass | 1920x1080 | 0.94 | 6.4% |
| pass4 | 480x270 | 0.93 | 6.4% |
| pass10 | 480x270 | 0.90 | 6.2% |
| pass11 | 480x270 | 0.89 | 6.1% |
| pass8 | 240x135 | 0.88 | 6.0% |
| pass7 | 240x135 | 0.88 | 6.0% |
| pass6 | 240x135 | 0.71 | 4.9% |
| pass3 | 480x270 | 0.20 | 1.4% |
| RCAS | 1920x1080 | 0.15 | 1.0% |
| 13 post shaders | | 0.04 | 0.3% |
| total FSR4 | | 14.59 | |

The 12 network passes take 10.56 ms: 2.92 ms at 960x540, 3.86 ms at 480x270 and 3.79 ms at 240x135.

## Layer rewrites at the same size

`bench2/bench_layer.c` times one compute shader at a real dispatch size on the RX 470. Each kernel
below is a 3x3 convolution with 16 input and 16 output channels at 240x135. The int8 and float32
outputs match a CPU reference (`bench2/check.py`): 0 mismatches for int8, max error 9e-7 for float32.

| kernel | driver | ms |
|---|---|---:|
| int8, FSR4 packing, `v_mad_i32_i24` | patched | 0.22 to 0.26 |
| int8, same | stock | 0.36 |
| float32, mat4 on packed vec4 channels | patched | 0.56 |
| Winograd F(2x2,3x3), float32 | patched | 0.93 |
| Winograd F(2x2,3x3), integer, exact | patched | 0.40 |
| FSR4 pass7 at 240x135, one dispatch | patched | 1.73 |

Findings:

1. int8 with the 24-bit multiply-add is the fastest representation. float32 costs 2.2 times more.
2. Integer Winograd is exact for int8 layers (checked in NumPy), but its operands exceed 8 bits,
   so it leaves the `v_mad_i32_i24` path and runs 1.85 times slower than the direct convolution.
3. FSR4's own pass code runs at about the same throughput per multiply as the clean kernel. The
   time follows the number of multiplies, not the code quality.

## Resolution lever

OptiScaler `[OutputScaling]` with `Enabled = true`, `Multiplier = 0.75`, `Downscaler = 0` makes FSR4
output 1440x810, and FSR1 scales that to 1920x1080. Every tensor then has 56% of the pixels.

| configuration | frames | mean ms | fps |
|---|---:|---:|---:|
| FSR 4.1.1b, 1920x1080 output | 2,195 | 27.78 | 36.0 |
| FSR 4.1.1b, OutputScaling 0.75 | 2,703 | 22.28 | 44.9 |

Both runs log `FSR31FeatureDx12` and `FSR4ModelSelection`, with no `FSR2FeatureDx12_212`.

## Flat shaders with baked weights

`archive_local/fsr4_411_re/tracer/` holds a tracing interpreter. It runs one invocation of a pass,
keeps tensor values symbolic, folds the weights to constants, and writes a flat GLSL shader. If a
weight byte has a magnitude at or below the threshold, the tracer drops that multiply.

1. Weights: `InitializerBuffer` is static data in the DLL. The used range (bytes 864 to 130975) is
   stored 5 times, identical, from file offset 0xc04a00. 4.1.1 stock and 4.1.1b share it, and 4.0.2
   does not contain it. A bake depends on the DLL build and the tensor bucket, not on the game.
2. Exactness: all 12 flat passes at threshold 0 produce byte-identical scratch buffers against
   FSR4's own shaders on the RX 470. The vkd3d-proton form (for `VKD3D_SHADER_OVERRIDE`) matches the
   standalone form on the CPU for passes 1, 4, 5, 10 and 11.
3. Multiplies removed with real weights: 21 to 53% per pass at |w| <= 8, 38 to 73% at |w| <= 16,
   52 to 82% at |w| <= 24. Weight energy kept: 99.1%, 94.6% and 86.3%.

Pragmata, 60 s windows, FSR 4.1.1b, all 12 passes replaced:

| override set | frames | mean ms | fps |
|---|---:|---:|---:|
| none | 2,164 | 27.79 | 36.0 |
| flat, exact | 1,880 | 32.10 | 31.2 |
| flat, abs(w) <= 8 | 2,339 | 25.90 | 38.6 |
| flat, abs(w) <= 16 | 2,675 | 22.74 | 44.0 |
| flat, abs(w) <= 24 | 2,882 | 21.17 | 47.2 |

All runs log `FSR31FeatureDx12` and `FSR4ModelSelection`. Picture quality of the pruned sets is not
checked yet.

Per-pass GPU time in Pragmata, ms per frame, from the profiling build (60 s each):

| pass | original | flat exact | flat abs(w) <= 16 | flat abs(w) <= 24 |
|---|---:|---:|---:|---:|
| output head | 2.91 | 3.04 | 3.05 | 3.04 |
| pass9 | 1.32 | 2.95 | 1.27 | 0.90 |
| pass11 | 0.89 | 1.33 | 0.92 | 0.72 |
| pass1 | 0.98 | 0.98 | 0.39 | 0.30 |
| pass2 | 0.97 | 0.97 | 0.42 | 0.32 |
| pass12 | 0.96 | 0.99 | 0.32 | 0.25 |
| pass4 | 0.93 | 0.90 | 0.31 | 0.24 |
| pass5 | 0.94 | 0.90 | 0.43 | 0.25 |
| pass10 | 0.90 | 0.89 | 0.31 | 0.20 |
| pass7 | 0.88 | 2.01 | 0.43 | 0.28 |
| pass8 | 0.88 | 2.14 | 0.48 | 0.32 |
| pass6 | 0.71 | 0.41 | 0.13 | 0.09 |
| pass3 | 0.20 | 0.16 | 0.10 | 0.08 |
| 12 passes | 10.56 | 14.63 | 5.51 | 4.15 |

The output head was not replaced in these runs, so its column stays near 3 ms.

1. The exact bake helps only pass6 (0.30 ms saved) and pass3 (0.05 ms saved). A set with only those
   two passes keeps the output bit-identical. Measured in the game, twice in one session:
   27.80 and 27.78 ms without the override, 27.44 and 27.45 ms with it, so it saves 0.35 ms.

### Why the exact bake loses on the other passes

ACO statistics and IR for pass7, from `RADV_DEBUG=shaderstats,shaders`:

| | original | flat exact | flat abs(w) <= 24 |
|---|---:|---:|---:|
| SPIR-V bytes | 98,828 | 887,912 | 263,824 |
| instructions | 5,136 | 29,926 | 7,488 |
| branches | 33 | 1 | 1 |
| VMEM clauses | 82 | 24 | 5 |
| copies | 394 | 5,556 | 310 |

1. The original extracts its weight bytes on the scalar unit (672 `s_bfe_i32`) and gives
   `v_mad_i32_i24` an SGPR operand. All 2,688 MAC instructions have a scalar first operand. The
   scalar unit runs in parallel with the vector unit, so this costs no vector instructions.
2. A GCN VOP3 instruction takes no 32-bit literal, and inline constants cover only -16 to 64. Of the
   baked MACs, about 11,000 get an inline constant and about 28,000 need the weight in an SGPR
   first, which adds 5,313 `s_movk_i32`. The vector instruction count per MAC stays near 1, so the
   bake saves nothing on the vector unit. It only removes buffer loads that were already cheap.
3. The bake replaces the loop with straight-line code, 5,136 instructions to 29,926. Polaris has a
   32 KB instruction cache shared by 4 compute units, so the unrolled body streams from L2.
4. pass6 is the exception because its original is already unrolled: 1 branch, 28,803 instructions
   and 597 VMEM clauses. The bake removes 575 of those clauses and cuts it to 11,188 instructions.

So the exact bake pays only where the original is already unrolled. Elsewhere it needs pruning to
remove enough multiplies to cover the code growth.
2. The output head bakes its weights as dword literals. `tracer/prune_const.py` prunes them with a
   text rewrite: 102 of 3,840 multiplies have weight 0, 39% have abs(w) <= 8, 60% have abs(w) <= 16,
   and 72% have abs(w) <= 24.

With the output head included, Pragmata, 60 s windows, one session:

| override set | frames | mean ms | fps |
|---|---:|---:|---:|
| none | 2,187 | 27.80 | 36.0 |
| exact bake of pass3, pass6 and the head | 2,220 | 27.48 | 36.4 |
| 12 passes and head, abs(w) <= 16 | 2,894 | 20.82 | 48.0 |
| 12 passes and head, abs(w) <= 24 | 3,094 | 19.57 | 51.1 |

Pruning the head is worth about 1.9 ms on top of the 12 passes at the same threshold. The exact set is slower in the game, so the choice between original and flat must be
made per pass from an in-game profile. `bench_layer` times do not predict in-game pass cost.

## Picture quality, judged on a Vega 56

The RX 570 has no video output in this box, so a Vega 56 replaced it for a look at the image. The
pruning changes only integer int8 math, and that is bit-identical on any GCN card. The picture on the
Vega is therefore the picture the RX 570 produces. Frame times from that card do not transfer.

The overrides do reach the game on the Vega. Of the 13 shaders, the 10 that were not replaced have
the same hashes as on Polaris. The 3 replaced ones are absent from the shader dump, because vkd3d
loads our SPIR-V instead of converting the DXIL.

Upscaler time as reported in the game, and the observer's verdict:

| set | upscaler ms | picture |
|---|---|---|
| exact bake of pass3, pass6 and the head | 7.5 | the control, bit-identical to stock |
| all passes and head, abs(w) <= 16 | about 6 | difference barely noticeable |
| all passes and head, abs(w) <= 24 | 4 to 5 | noticeably worse, still ahead of FSR 3.1.5 |

The same ordering holds in isolation on the Vega: pass7 takes 2.48 ms at threshold 8, 1.16 ms at 16
and 0.51 ms at 24.

At threshold 16 on Polaris, with the profiling build, the 12 passes and the head come to 7.66 ms per
frame. The head falls from 3.04 to 1.18 ms. What is left is led by pass9 at 1.28 ms, the head at
1.18 ms, the prepass at 0.94 ms and pass11 at 0.93 ms. The prepass holds almost no weights, so
pruning does not reach it.

## Packing two output channels into one multiply

Pruning drops terms. Packing keeps them all and halves the multiplies instead. Two output channels of
a layer read the same activations. Their weights are quantized to N bits and packed into one operand
as `q_a + (q_b << 16)`. One `v_mad_i32_i24` then adds `q_a * x` to the low half of the accumulator and
`q_b * x` to the high half. A bias of 32768 keeps the low half non-negative, so the halves never mix.
The accumulator is flushed every few steps to stay inside 32 bits. The packing itself is exact,
checked over 300 random activation vectors, so the only error is the quantization.

Error on pass7, measured against the exact pass on the captured tensor:

| mode | relative error | compare |
|---|---:|---|
| pack6 | 5% | |
| pack5 | 11% | prune16 is 51% |
| pack4 | 21% | prune24 is 71% |
| pack3 | 44% | |

Every pass beats FSR4's own shader on the Vega with pack5, from pass4 at 1.35 to 0.83 ms up to
pass11 at 3.10 to 2.16 ms.

In the game, with the output head untouched:

| set | upscaler ms | picture |
|---|---|---|
| exact bake of pass3, pass6 and the head | 7.5 | the control |
| 12 passes, pack5 | 6 | no visible difference |
| 12 passes, pack6 | 6 | no visible difference |
| 12 passes and head, pack5 | 5 | flicker, and hair artifacts |
| 12 passes and head, pack6 | 5 | better than pack5, still flickers |

**Leave the output head alone.** Every rewrite of it, pruned or packed, costs temporal stability. The
head writes `rw_history_color`, and the next frame reads that back through the prepass. An error
there therefore returns frame after frame, instead of staying inside one frame. The network passes
have no such feedback, and rewriting all twelve is free of visible cost.

## The gain depends strongly on the card

The same shaders were timed on a Vega 56 and on the RX 470. The Vega gains far more.

Per pass, in isolation, FSR4's own shader against our pack5 version, in ms:

| pass | Vega orig | Vega pack5 | Polaris orig | Polaris pack5 |
|---|---:|---:|---:|---:|
| pass6 | 1.08 | 0.86 | 2.71 | 1.23 |
| pass7 | 0.96 | 0.88 | 1.75 | 1.40 |
| pass9 | 2.23 | 2.11 | 3.75 | 4.69 |
| pass11 | 3.10 | 2.16 | 3.22 | 2.81 |

In the game, at 1280x720 to 1920x1080 in one light scene, all runs with the real FSR4 path:

| set | Vega upscaler ms | Polaris frame ms |
|---|---|---:|
| stock FSR4 | 7.5 | 22.04 |
| pack5, 12 passes | 6 | 22.02 |
| selected per pass | | 21.29 |

On the Vega, pack5 takes the upscaler from 7.5 ms to 6 ms with no visible change. On Polaris the same
set is a wash, and only the per-pass selection wins, by about 0.75 ms.

The likely cause is that Polaris runs FSR4's own code near its limit already. The original extracts
its weight bytes on the scalar unit and hands `v_mad_i32_i24` an SGPR operand, so it reaches one
vector instruction per multiply. Packing halves the multiplies, but its constants are literals, and
it pays for the accumulator flushes and the final scaling. On the Vega that trade wins clearly. On
Polaris it barely does, and on pass9 it loses.

**Pick the variant per card.** The tool times every variant on the card it runs on, and keeps the
original wherever nothing beats it. A set built for one card can be slower on another: `ovr_pair6`
measured 25.40 ms on Polaris against 22.18 ms for stock, purely because of pass9.

## Pruning against packing, judged by eye on the RX 570

Both families were measured per pass on this card, for cost and for error against the exact result.
They are complementary rather than ranked: on pass7 pruning is much cheaper (0.86 ms at 51% error
against packing's 1.41 ms at 10%), while on pass6 packing wins outright (1.17 ms at 11% against
0.92 ms at 28%).

In the game, upscaler time and the observer's verdict:

| set | upscaler ms | picture |
|---|---|---|
| packing, per-pass budget 15% | 13 | better than prune16 |
| packing, per-pass budget 25% | 11 to 12 | the best of the group |
| packing, per-pass budget 50% | 11 to 12 | very noisy, not worth it |
| pack3 everywhere | 10 to 11 | decent, noisy hair |
| mixed families, budget 15% | 13 | better than prune16 |
| mixed families, budget 25% | 11 | decent, near prune16 |
| mixed families, budget 40% | 9.5 | near prune16 |
| prune16 | 9.5 | decent |

**At equal measured error, pruning looks worse than quantization.** The budget 25% sets differ only
in whether pruning is allowed, and the packing-only one is clearly better to the eye. Pruning removes
a share of every sum, so its error is systematic, while quantization spreads a small unbiased error
over every term. The metric counts both the same way, so prefer packing when the two are close, and
use pruning only when the speed matters more than the picture.

## The Vulkan layer

`tools/fsr4_layer` replaces the shaders below the game rather than through vkd3d's override. It
hashes every SPIR-V module the application creates and swaps in a replacement of that name. The
original is used whenever the driver rejects one.

Two things had to be handled.

1. vkd3d-proton rarely creates `VkShaderModule` objects. It puts the module inline in
   `vkCreateComputePipelines`, so the layer hooks that call as well.
2. vkd3d serves pipelines from its own cache without handing SPIR-V to the driver. The layer then
   sees nothing to replace, so `VKD3D_CONFIG=pipeline_library_ignore_spirv` is required. The
   `fsr4-run` wrapper sets it.

Measured through the layer on the RX 570, one scene, frame time rather than upscaler time:

| set | frames | mean ms | fps |
|---|---:|---:|---:|
| stock FSR4 | 2,764 | 21.85 | 45.8 |
| quality (fin15) | 2,984 | 20.39 | 49.0 |
| prune12 | 3,297 | 18.30 | 54.7 |

The layer costs nothing itself: stock through the layer measured 21.85 ms against 22.04 ms for the
same scene through vkd3d's override path.

## Design: bake the weights at load time, in vkd3d-proton

The exact bake is bit-identical, so it carries no quality risk. It wins on some shaders and loses on
others. The decision is cheap to make at load time. This design makes the win automatic for every
game that uses this FSR4 DLL. It is also the base that the pruned variants plug into later.

### Where the weights come from

Read them from the DLL file. `InitializerBuffer` is static data: bytes 864 to 130975 of the buffer
live at file offset 0xc04a00 of `amd_fidelityfx_upscaler_dx12.dll`, in 5 identical copies. No GPU
readback and no host-visible heap trick are needed. Make sure that the block is the one the shaders
index, because it differs between DLL builds.

### How to decide per shader

Do not decide from the source. pass6 has 128 loops in its source and ACO unrolls it completely, while
pass7 keeps its loops. Decide from the compiled pipeline instead:

1. Compile the original SPIR-V and the baked SPIR-V.
2. Query both with `VK_KHR_pipeline_executable_properties`, which RADV supports on this card.
3. Keep the variant with the lower instruction count, and drop the other pipeline.

### What to cache

Key the cache on the shader hash and a hash of the weight block. Store the winning SPIR-V and the
decision. Later runs then load the cache and skip the bake. The first run pays one bake and two
compiles per network shader. That is a one-time stutter. Do it during pipeline creation, not in the
frame loop.

### Why it generalizes

The bake depends on the DLL build and the tensor size bucket, not on the game or the scene. The same
cache therefore serves every game that ships this DLL. Scenes that use other tensor buckets, and
other games, bring in shader variants this session never compiled, and each one gets the same test.

## Next steps

1. Capture `rw_mlsr_output_color` from the GPU for a fixed frame sequence, so that quality can be
   compared between configurations without the headless compositor.
2. Build the full data-flow graph between passes from the scratch-buffer offsets. pass7 and pass8
   write alternately to the same two 240x135 tensors, and pass4 and pass5 do the same at 480x270.
   These pairs are candidates for residual blocks.
3. Replace single passes with a copy shader through `VKD3D_SHADER_OVERRIDE`, and measure time and
   quality for each removed pass.
4. Profile OutputScaling 0.75 and 0.833 per pass, and compare quality against pass removal.

## gfx803 environment

1. Pragmata runs inside the Steam Linux Runtime container, which does not mount `/data`. Point
   `VKD3D_TIMESTAMP_PROFILE` and `VKD3D_LOG_FILE` under the prefix `drive_c`, and copy the files out.
2. The profiler needs a vkd3d-proton build with `-Denable_profiling=true` and
   `-Dc_args=-Wno-error=incompatible-pointer-types`.
3. `/data/fsr4_tools/bench_pragmata.sh` is an old weston script. The repo script is at
   `/data/tmp/fsr4re/bench_pragmata.sh`.
4. The game directory now holds the 4.1.1b DLL. Proton and the prefix hold the repo vkd3d-proton
   DLLs. `OptiScaler.ini` is unchanged (md5 `fbabd2d609a907ae1a16bf510b4271b9`).
