# Running the BC-250 FSR4 build on GCN4

`daniel-h-0/bc250-fsr4-fork` rebuilds AMD's FSR4 DLL with its shaders rewritten by hand. It is built
for the BC-250, a gfx1013 part. It does not upscale on gfx803 as shipped, and it does after one
change. This note records that change, what it is worth, and which of the fork's other ideas survive
contact with this hardware.

## The one change that matters

The fork's shaders declare that they need a wave of 32 lanes. GCN4 has one wave size, 64, so
vkd3d-proton refuses the compute pipeline:

    Required WaveSize range [32, 32], but supported range is [64, 64]
    Failed to update compute state, ignoring dispatch

Nothing reports a failure. The DLL loads, the model selection hook binds, the context is created and
dispatches begin. The only sign is the dispatch count. The network runs four passes per frame where FSR4 runs about
twenty six, and the picture becomes a wrongly placed copy of part of the frame.

`tools/bc250/wave64_fix.py` drops the requirement. DXIL writes it two ways, a range under tag 23 and
a fixed size under tag 11, and both have to go: handling only the first leaves 14 shaders asking for
32 lanes, which keeps the network broken.

## What it costs and what it gains

Upscaler GPU time on an RX 570 in Pragmata, 1280x720 to 1920x1080, measured with `FSR4_PROFILE=1`.

| build | upscaler ms | frametime ms |
|---|---:|---:|
| FSR4 4.1.1b | 15.18 | 21.08 |
| BC-250 with the wave size fix | 12.8 | 18.8 |
| the same, with the SDK's own pass11 | 12.7 | 18.4 |

The fork's own shaders are exact, so this costs no picture quality.

## Rebuilding

The fork builds from the SDK DLL this repository already carries,
`fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll`, whose sha256 matches the one it pins. With
the pinned DXC 1.9.2607 its published DLL reproduces byte for byte, which is what makes any change
trustworthy.

Its own `build.py` refuses a modified source, because it exists to prove that reproduction.
`tools/bc250/build_variant.py` does the same work and takes the sources as they are, keeping every
structural check in the fork's repacker. `SKIP_ENTRIES=pass11` keeps the SDK's own shader for one
role and the fork's for the rest.

## What does not transfer

Each of these was measured, not reasoned about.

**Rewriting the packed 16-bit arithmetic.** GCN4 has `v_mad_legacy_u16`. The fork's code already
compiles to 1,806 of them in one pass, about one instruction per multiply-accumulate. Converting it
to 32-bit `v_mad_i32_i24` changed a shader from 6,240 instructions to 6,238.

**Forcing higher occupancy.** These shaders take 256 registers and run one wave per SIMD. Capping the
register file gives two waves for two spilled registers, and in game it changed nothing: 13.06 ms
against 12.92 ms. A GCN wave64 instruction occupies its SIMD for four cycles regardless. One wave therefore already
saturates the vector units on code with this much instruction level parallelism. The same cap badly
hurts stock FSR4, at 33 ms.

**Converting the float chains to integers.** Those chains are integer arithmetic in float clothing:
every activation is a sign extended byte and all 346 float literals are whole numbers. In integers
the postpass went from 6,222 instructions to 6,323, and from four waves per SIMD to three. It already
runs at 64 registers and full occupancy, so there was nothing to reclaim.

**Dropping small weights.** Their weight scale differs per layer, from single digits to over three
thousand. One threshold therefore guts one layer and misses the next. Pruning by share of each
layer's magnitude fixes that and still costs too much. At 8 percent the picture is unstable around
hair, and that is inherent. Pruning removes a share of every sum, which biases it, and a biased sum shifts the
temporal accumulation instead of averaging out across frames.

**Packing two output channels into one multiply.** This is the trick behind the shader sets in this
repository. It needs one activation multiplied by two weights. The fork's chains are four terms long
and every activation is used exactly once, across 3,753 of them in the postpass and 28,734 in
pass9. The structure it needs does not exist here.

**Their extra barrier.** The repacker forces a UAV barrier the SDK skips, to stop a padding clear
overlapping the model dispatch on their hardware. Removing it measured 12.80 against 12.94, which is
noise, so their correctness fix stays.

## Choosing shaders per role

Every one of the 348 shaders is modified by the fork, including all 180 prepass shaders. Keeping the
SDK's own shader for one role at a time and measuring gives:

The fork's pass6 and pass7 are clearly better, and swapping either out costs about 0.45 ms. pass11 is
the one role where the SDK's shader wins, on both upscaler time and frametime, by about 0.4 ms of
frametime. Everything else lands inside the run to run spread, which is about 0.4 ms here, so it is
not resolvable by single runs.
