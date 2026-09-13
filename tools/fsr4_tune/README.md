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

A game that uses FSR4, plus `spirv-cross`, `glslc`, `gcc`, `g++`, python3, and the Vulkan loader
headers.

## Using the result

The Vulkan layer in `../fsr4_layer` loads what this tool produces, and `fsr4-run` puts the layer in
front of a game. Read that README first if you only want the shipped sets.

## The steps

1. Dump the shaders the game compiles, through the layer:

       FSR4_LAYER_DUMP=/some/dir FSR4_SET=off ../fsr4_layer/fsr4-run <launch the game>

   Play for a few seconds in the scene you care about, then quit. Each file is named after the hash
   of the SPIR-V inside it, which is the name the layer looks a replacement up under.

2. Build the variants and time them. `../../data/weights/fsr4_411b_weights.bin` is FSR4's weight
   block, which is a property of the DLL and the same on every machine:

       python3 tune.py capture /some/dir ../../data/weights/fsr4_411b_weights.bin
       python3 tune.py generate --modes pack6,pack5,pack4,prune16
       python3 tune.py bench /some/dir

   For another FSR4 build, dump the buffers it fills and run `pick_weights.py <dump_dir> weights.bin`
   to find the weight block among them.

3. Write the winners into a set, and run the game with it:

       python3 tune.py install ~/.local/share/fsr4/sets/mine --mode best
       FSR4_SETS=~/.local/share/fsr4/sets FSR4_SET=mine ../fsr4_layer/fsr4-run <launch the game>

   `--mode best` takes the fastest variant that beats the game's own shader by the margin, per
   shader. `--mode pack5` forces one mode everywhere.

   The set is valid for the Proton build you dumped from. A set for another build means dumping and
   tuning again.

## Teaching the shipped sets another Proton build

The sets in `../fsr4_layer/sets/` are named after DXIL blobs, and `keys.txt` maps the SPIR-V hash the
layer sees to those names. If the shipped sets replace nothing on your Proton, add your build:

    VKD3D_SHADER_DUMP_PATH=/some/dir <launch the game>
    python3 make_keys.py /some/dir ../fsr4_layer/sets

## Checking the result

The log of your upscaler tells you whether FSR4 still runs. With OptiScaler, a correct run logs
`FSR31FeatureDx12` with `FSR4ModelSelection` and no `FSR2FeatureDx12_212`.

`err.py` measures a variant against the exact shader on a captured tensor, and `e2e.py` measures a
whole set through the pass chain. Both need a tensor capture, which the repository notes describe.

## Limits

The tool rewrites only shaders whose multiply chains it recognizes. It measures one shader at a time,
so a variant that wins in isolation can still lose in the frame. Neither check sees temporal
behavior, so judge flicker with your own eyes.
