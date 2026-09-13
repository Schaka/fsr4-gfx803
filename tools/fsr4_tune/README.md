# fsr4_tune: faster FSR4 shaders for your own GPU

FSR4's upscaler spends almost all of its time on int8 multiply-accumulate chains. This tool rewrites
those chains, times every rewrite on your card, and keeps the ones that win. The output is a
directory of SPIR-V. The Vulkan layer in `../fsr4_layer` loads it, so no file in the game or in the
FSR4 DLL is touched.

Measured with FSR 4.1.1b at 1280x720 to 1920x1080, the upscaler pass fell from 7.5 ms to 3 ms on a
Vega 56, and from about 14.5 ms to 9.5 ms on an RX 570.

## The rewrites

**Weight baking.** FSR4 streams its weights from a buffer. Once they are known, they become constants
in the shader. That helps only where the shader is already unrolled, so the tool measures it.

**Packing (`packN`).** Two output channels of a layer read the same activations. Their weights are
quantized to N bits and packed into one operand as `q_a + (q_b << 16)`. One `v_mad_i32_i24` then adds
`q_a * x` to the low half of the accumulator and `q_b * x` to the high half. A bias of 32768 keeps
the low half non-negative, so the halves never mix. The accumulator is flushed every few steps to
stay inside 32 bits. Nothing is dropped: every input still reaches every output.

Measured on FSR4's pass7 against the exact result, on captured gameplay data:

| mode | relative error |
|---|---:|
| pack6 | 5% |
| pack5 | 11% |
| pack4 | 21% |
| pack3 | 44% |

**Pruning (`pruneT`).** Multiplies whose weight has magnitude T or less are dropped. The surviving
weights are scaled up to carry the lost magnitude. Pruning is faster than packing, but it costs much
more quality: pruning at 16 gives 51% error on the same pass, where pack5 gives 11%.

## What you need

A game that uses FSR4 through vkd3d-proton, plus `spirv-cross`, `glslc`, `gcc`, `g++`, python3, and
the Vulkan loader headers.

## Using the result

The Vulkan layer in `../fsr4_layer` loads what this tool produces. `install_layer.py` copies a set
into its cache, and `fsr4-run` puts the layer in front of a game. Read that README first if you only
want to use the shipped sets.

## The steps

1. Dump the shaders the game compiles:

       VKD3D_SHADER_DUMP_PATH=/some/dir <launch the game>

   Play for a few seconds in the scene you care about, then quit.

2. Get the weight buffer. Run the game once with `FSR4_FORCE_HOST_WEIGHTS=1` and `FSR4_DUMP_DIR`
   set, which needs the patched vkd3d-proton from this repository, then:

       python3 pick_weights.py /the/dump/dir weights.bin

3. Build the variants and time them:

       python3 tune.py capture /some/dir weights.bin
       python3 tune.py generate --modes pack6,pack5,pack4,prune16
       python3 tune.py bench /some/dir

4. Write the winners and load them through the layer:

       python3 tune.py install ./override --mode best
       python3 install_layer.py /some/dir ./override ~/.cache/fsr4_opt/spirv
       FSR4_SETS=... ../fsr4_layer/fsr4-run <launch the game>

   `VKD3D_SHADER_OVERRIDE=$PWD/override` also works, and skips the layer.

   `--mode best` takes the fastest variant that beats the game's own shader by the margin, per
   shader. `--mode pack5` forces one mode everywhere.

## Checking the result

The log of your upscaler tells you whether FSR4 still runs. With OptiScaler, a correct run logs
`FSR31FeatureDx12` with `FSR4ModelSelection` and no `FSR2FeatureDx12_212`.

`err.py` measures a variant against the exact shader on a captured tensor, and `e2e.py` measures a
whole set through the pass chain. Both need a tensor capture, which the repository notes describe.

## Limits

The tool rewrites only shaders whose multiply chains it recognizes. It measures one shader at a time,
so a variant that wins in isolation can still lose in the frame. Neither check sees temporal
behavior, so judge flicker with your own eyes.
