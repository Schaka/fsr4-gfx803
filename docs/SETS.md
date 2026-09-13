# The shader sets

Every set is a directory of SPIR-V under `tools/fsr4_layer/sets/`. The Vulkan layer swaps in the
shaders it has, and leaves everything else alone. Pick one with `FSR4_SET`.

Four aliases cover the common cases: `lossless` is `exact`, `quality` is `fin15`, `balanced` is
`fin25`, and `speed` is `prune16`. Everything else is named directly.

## How the rewrites work

**Baking.** FSR4 streams its weights from a buffer. Once they are known they become constants in the
shader. The maths does not change, so the output is bit-identical. It is faster only where the
shader was already unrolled. That is why the `exact` set holds two shaders rather than twelve.

**Packing (`packN`).** Two output channels of a layer read the same activations. Their weights are
quantized to N bits and packed into one operand as `q_a + (q_b << 16)`. One `v_mad_i32_i24` then
serves both halves of the accumulator. Nothing is dropped: every input still reaches every output.
The packing itself is exact, so the error is the quantization alone.

**Pruning (`pruneT`).** Multiplies whose weight has magnitude T or less are dropped, and the
survivors are scaled up to carry the lost magnitude.

**Mixed sets.** `mix` uses packing only, `opt` adds pruning, and `fin` adds the hybrids that prune
first and pack what is left. Each one gives every pass the cheapest variant whose measured error
stays under the budget in the name. FSR4's own shader is kept where nothing beats it.

## What each one measured

Upscaler time in milliseconds, at 1280x720 to 1920x1080 with FSR 4.1.1b, one scene per card. Blank
means it was not measured on that card.

| set | what it is | Vega 56 | RX 570 | how it looked |
|---|---|---:|---:|---|
| stock FSR4 | the reference | 7.5 | about 14.5 | |
| `exact` | weights baked in | 7.5 | 14 | bit-identical |
| `pack6` | 6-bit packing everywhere | 6 | | no visible change |
| `pack5` | 5-bit packing everywhere | 6 | 14.6 | no visible change |
| `pack4` | 4-bit packing everywhere | | | slight noise |
| `pack3` | 3-bit packing everywhere | | 10 to 11 | decent, noisy hair |
| `prune8` | drops weights of size 8 or less | | 12 to 13 | good |
| `prune12` | size 12 or less | | 11 | good |
| `prune16` | size 16 or less | 3 to 5 | 9.5 | decent, softer |
| `prune20` | size 20 or less | 5.5 | | washed out, no ghosting |
| `prune24` | size 24 or less | 4 to 5 | | ghosting |
| `mix15` | packing only, under 15 percent error | | 13 | better than prune16 |
| `mix25` | packing only, under 25 percent | | 11 to 12 | the best of that group |
| `mix50` | packing only, under 50 percent | | 11 to 12 | very noisy |
| `opt15` | packing and pruning, under 15 percent | | 13 | better than prune16 |
| `opt25` | packing and pruning, under 25 percent | | 11 | decent |
| `opt40` | packing and pruning, under 40 percent | | 9.5 | near prune16 |
| `fin15` | all families, under 15 percent | 4.0 to 4.5 | 13 | hard to tell from stock |
| `fin25` | all families, under 25 percent | 3.5 | 11 | very close to stock |
| `fin40` | all families, under 40 percent | | 10.5 | worse than prune16 |

The error percentages are measured against the exact result on captured gameplay data. They rank
variants, and they do not predict what you will see, so try a few in a game you know.

## Two things the numbers do not show

**The card matters more than the set.** A Vega 56 gains far more than an RX 570. Polaris already runs
FSR4's own code at one vector instruction per multiply, because it extracts weight bytes on the
scalar unit. There is less to win there, and a set built for one card can be slower on another.

**Pruning looks worse than quantization at equal measured error.** Pruning removes a share of every
sum, which biases the result, while quantization spreads a small unbiased error over every term.
Prefer a `pack` or `mix` set when the measured numbers are close.

The output head is never rewritten in any shipped set. Its errors reach the history buffer that the
next frame reads, so they return frame after frame as flicker.

`notes/FSR4_411_ANALYSIS.md` carries the per-pass measurements behind all of this.
