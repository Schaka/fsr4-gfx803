# Running the BC-250 FSR4 build on GCN4

`daniel-h-0/bc250-fsr4-fork` rebuilds AMD's FSR4 DLL with its shaders rewritten by hand. It is built
for the BC-250, a gfx1013 part. It does not upscale on gfx803 as shipped, and it does after one
change. This note records that change, what it is worth, and which of the fork's other ideas survive
contact with this hardware. The version measured is `v4.0.0-rc10`.

`../tools/bc250/README.md` gives the build steps and `../tools/bc250/hybrid.md` the arrangement that
combines this build with the tuned shader sets.

## The one change that matters

The fork's model pass shaders declare that they need a wave of 32 lanes. GCN4 has one wave size, 64,
so vkd3d-proton refuses the compute pipeline:

    Required WaveSize range [32, 32], but supported range is [64, 64]
    Failed to update compute state, ignoring dispatch

Nothing reports a failure. The DLL loads, the model selection hook binds, the context is created and
dispatches begin. The only sign is the dispatch count. The network runs four passes per frame where
FSR4 runs about twenty six. The picture becomes a wrongly placed copy of part of the frame.

`tools/bc250/wave64_fix.py` drops the requirement. DXIL writes it two ways, a range under tag 23 and
a fixed size under tag 11, and both have to go. Handling only the first leaves 14 shaders asking for
32 lanes, which keeps the network broken.

## Where this build wins

Each row keeps the SDK's own shader for one role and the fork's for the rest, so the difference is
that role alone. Frametime on an RX 570 in Pragmata, 1280x720 to 1920x1080, three interleaved cycles
each, spread under 0.05 ms.

| role taken from the SDK instead | frametime, lossless | frametime, balanced |
|---|---:|---:|
| nothing, the fork everywhere | 14.56 | 11.21 |
| the postpass | 16.08 | 12.73 |
| the prepass | not measured | 11.26 |

The postpass is where this build earns its place: 1.5 ms, on both settings. The prepass is worth
0.05 ms, which is at the edge of what these runs resolve.

`tools/bc250/fp32_prepass.py` moves the prepass off 16-bit floats. It unpacks a pair of halves,
converts each to a float, converts each straight back to a half, and multiplies in fp16. GCN4 runs
fp16 at the same rate as fp32, so every one of those conversions is spent for nothing, and the
driver then converts each product back to fp32 to accumulate it. Promoting the whole subgraph takes
the prepass from 1,820 instructions to 1,646. It is also more accurate: an fp32 multiply keeps 24
bits of product where fp16 keeps 11.

## The instruction floor

Every hot shader is close to the least work its maths can be expressed in. In the postpass, 3,676 of
6,103 instructions are the multiply-accumulates themselves, at 0.98 per multiply. In pass9 it is
27,418 of 44,706, at 0.95. The int8 to float conversion is free: the texture unit does it on load
through `buffer_load_format_x`, so only a few hundred convert instructions exist for tens of
thousands of multiplies.

The GPU counters agree about where the limit is. During a run `gpu_busy_percent` reads 99.6 and
`mem_busy_percent` reads 29.2, at full clocks, so this is bound by instruction issue and not by
memory. That is why a rewrite which leaves the instruction count alone changes nothing, and it sets
the floor for anything that keeps the maths exact. The best lossless arrangement measured is 12.85 ms
of upscaler time. Reaching 11 ms without touching the result needs about 15 percent fewer
instructions, and at 0.95 per multiply there are none left to remove without removing multiplies.

## What does not transfer

Each of these was measured, not reasoned about.

**Converting the float chains to integers.** Those chains are integer arithmetic in float clothing:
every activation is a sign extended byte, and across the 96 postpass shaders all 356,842 weights are
whole numbers no larger than 128. In integers each multiply-accumulate can become one
`v_mad_i32_i24`. It is slower, by 1.1 ms of frametime in both the lossless and the balanced
arrangement. The float form already costs one instruction, because `v_mac_f32` is VOP2 and takes its
weight as a 32-bit literal. `v_mad_i32_i24` is VOP3, where GCN4 allows no literal, and only 76
percent of the weights fall in the inline constant range of -16 to 64. Loading the rest costs more
than the byte extracts the change saves. `tools/bc250/int24_postpass.py` keeps the method.

**Rewriting the packed 16-bit arithmetic.** GCN4 has `v_mad_legacy_u16`. The fork's code already
compiles to 1,806 of them in one pass, about one instruction per multiply-accumulate. Converting it
to 32-bit `v_mad_i32_i24` changed a shader from 6,240 instructions to 6,238.

**Forcing higher occupancy.** These shaders take 256 registers and run one wave per SIMD. Capping the
register file gives two waves for two spilled registers, and in game it changed nothing: 13.06 ms
against 12.92 ms. A GCN wave64 instruction occupies its SIMD for four cycles regardless, so one wave
already saturates the vector units on code with this much instruction level parallelism. The same cap
badly hurts stock FSR4, at 33 ms.

**Dropping small weights.** Their weight scale differs per layer, from single digits to over three
thousand. One threshold therefore guts one layer and misses the next. Pruning by share of each
layer's magnitude fixes that and still costs too much. At 8 percent the picture is unstable around
hair, and that is inherent. Pruning removes a share of every sum, which biases it, and a biased sum
shifts the temporal accumulation instead of averaging out across frames.

**Packing two output channels into one multiply.** This is the trick behind the shader sets in this
repository, and it needs one activation multiplied by two weights. The fork's chains are four terms
long and every activation is used exactly once, across 3,753 of them in the postpass and 28,734 in
pass9. The structure it needs does not exist here.

**Their extra barrier.** The repacker forces a UAV barrier the SDK skips, to stop a padding clear
overlapping the model dispatch on their hardware. Removing it measured 12.80 against 12.94, which is
noise, so their correctness fix stays.
