# Roofline: what is actually achievable on an RX 470

Hardware: 32 CU × 4 SIMD × 1206 MHz = **1.54e11 wave-instructions/s** (wave64, 1 instr/SIMD/cycle).

## We are already near the issue-rate ceiling
The largest shader alone, at its observed dispatch shape:

    241 × 133 = 32,053 waves × 61,535 instructions = 1.97e9 wave-instructions
    -> 12.8 ms at *peak* issue rate, for one dispatch of one shader

Measured whole-upscaler time is **26.4 ms** = a budget of 4.08e9 wave-instructions.
Combined with `stall% = 0.0` from `shaderstats/shader_stats.csv`, the kernels are running
essentially at the machine's instruction-issue limit. There is no idle machine to reclaim.

At the current 1.5 VALU/MAC that budget is ~2.7e9 wave-MACs/frame (~174 GMAC at 64 lanes).

## UPDATE: one codegen lever is still open (`imul24/imul24_relaxed`)
The floor below is **our decomposition's** floor, not the hardware's. GCN has a full-rate 24-bit
multiply-add (`v_mad_i32_i24`) that is exact for 8-bit operands and takes a 32-bit accumulator —
1.0 VALU/MAC instead of 1.50. NIR simply never generates it for this pattern, which is a *Mesa*
limitation we can patch (and have, partially). See `imul24/README.md` — mechanism proven, not yet a
net win. Everything below still holds for the *unpatched* driver.

## Why <10 ms cannot come from better codegen

    26.4 ms -> 10 ms  requires 2.64x fewer instructions
    VALU/MAC would have to fall from 1.5 to 0.57
    hardware floor without dp4a is ~1.0 (one multiply + one accumulate, best case fused)

GCN4 has no dp4a and no packed 16-bit math, so 4 MACs can never collapse into one instruction.
**Therefore the target is unreachable by instruction selection or a hand-written kernel alone —
it requires executing fewer MACs.**

## Weight structure offers no shortcut either
From `weights/summary.json` (36,608 baked int8 weights recovered from 9 shaders):

| property | value | implication |
|---|---|---|
| zeros | 1.9% | sparsity skipping is worthless |
| ±1 | 3.8% | strength reduction is worthless |
| distinct values | 255 of 256 | densely quantised |
| distribution | near-uniform over -128..127 | high entropy, no exploitable structure |

The weights behave like dense random int8. Every classic weight-structure trick is dead.

## Findings
1. **Codegen.** The patched RADV emits `v_mad_i32_i24` at 1.0 VALU per MAC, the hardware floor
   without dp4a. See `imul24/README.md`.
2. **Layer shape.** The hot layers are 1×1 convolutions and matmuls, not 3×3. A Winograd transform
   does not apply to them. See `activations/README.md`.
3. **Low-rank factorisation.** The weight matrices are thin. At every plausible shape the break-even
   rank meets or exceeds the matrix rank, so a factorised layer costs more than the original.
4. **Resolution.** The number of MACs per frame scales with the internal render resolution.
5. **The one spilling shader** (14,934 instrs, 9,216 B scratch, 156 VMEM clauses) is the only hot
   shader that spills memory.
