# `v_mad_i32_i24` — LANDED (2026-08-22, session 8)

> Measured in the FSR SDK sample on Mesa 26.1.6, August 2026. Game results are in
> `../../README.md`.
>
> `isa_histograms_patched_radv.txt` is the first patched build, with only the extract-times-extract
> rules, for `i32` and `dot`. `isa_histograms_patched_radv_v2.txt` is the final build, which added the
> constant rules. The "Final ISA" block below comes from v2.

Patched RADV lowers the int8 MAC path to full-rate 24-bit multiply-add.
`i32` mode + patched driver is the new default: **37.6 ms / 26.6 fps** total,
**19.1 ms upscaler cost** (was 26.4 ms on mad16) — **1.38× on the upscaler**.

Reproduce:

    VK_DRIVER_FILES=/data/radv_custom/radeon_icd.x86_64.json \
        /data/fsr4_tools/bench_fsr4.sh i32 1 22

## Final results (mean of 60 frames, vsync off, floor 18.48 ms)

| mode | stock RADV | patched RADV | upscaler cost |
|---|---|---|---|
| `mad16` | 44.90 ms | 44.79 ms | 26.4 / 26.3 ms |
| `i16` | 52.80 ms | 52.67 ms | 34.3 ms |
| `i32` | 60.73 ms | **37.62 ms** | 42.3 / **19.1 ms** |
| `dot` | slowest | 41.82 ms | 23.3 ms |

## Final ISA (whole frame, patched + i32)

    93,736 v_mad_i32_i24        <- the MAC path, 1.0 VALU/MAC = hardware floor
     1,208 v_mul_i32_i24        <- unfused (accumulator used elsewhere)
         0 v_mul_lo_u32         <- ZERO. was 19,451. target reached
     7,123 v_add_u32_e32        <- address/index math (AMD's own shader code)
    ~25,000 fp32 ops            <- AMD's requant layers
    total VALU 172,313 / SALU 96,287 (SALU co-issued, hidden behind VALU)

Top shader: 22,785 mad24 for 23,040 MACs, **zero** quarter-rate ops,
no spills (the mad16-era spilling shader no longer spills — fusion also cut
register pressure). Hot shaders still `latency == inverse throughput` (issue bound).

## What the patch is (patches/mesa-nir-imul24-int8.patch)

Two layers, both in `src/compiler/nir/`:

1. `nir_opt_algebraic.py` rules:
   `imul(extract_i8, extract_i8)` → `imul24_relaxed` (u8 → `umul24_relaxed`),
   guarded by `options->has_mul24_relaxed` (true for all AMD).
2. **The rule that finished it:** `imul(extract_i8, #const)` → `imul24_relaxed`
   when the constant fits signed 24 bits (`is_s24` helper in
   `nir_search_helpers.h`; u8 analogue `is_u24`).

Why #2 is needed: the ~19.4 K
surviving `v_mul_lo_u32` were **not** caused by `OpBitFieldSExtract` vs
`extract_i8` canonicalisation (that canonicalisation works fine via
`ibfe` — RADV sets `lower_bitfield_extract` + `has_bfe`). Per-shader ISA
analysis showed the top-3 hot shaders had *already* fused to ~1.0 VALU/MAC
through ACO's own range analysis (`get_alu_src_ub` at
`aco_select_nir_alu.cpp:1761`). The misses were all in nine mid-tier shaders
where NIR constant-folded the **weight-side** extract into a literal
(`imul(extract_i8(a), 37)`), so the extract×extract pattern no longer matched.
The constant-folding happens because those shaders bake (some) weights as
immediates while the hot ones stream them from SSBOs.

Mixed-sign operands (i8×u8) are left as plain `imul` — correct but unfused;
FSR4 uses `dot4add_i8packed` (all-signed) so this doesn't matter here.

## Correctness argument
`imul24_relaxed` → `v_mul_i32_i24`/`v_mad_i32_i24` sign-extend their 24-bit
operands. A sign-extended int8 (−128..127) or an s24-ranged constant passes
through the 24-bit operand path unchanged; products of 8-bit×24-bit values are
exact in 32 bits. `umul24_relaxed` zero-extends; u8 (0..255) and u24 constants
likewise exact. No saturation/relaxed rounding involved — bit-exact.

## What's left in the frame (why codegen is now closed)

Per-MAC cost is 1.0 VALU (mad24) + ~1.0 SALU (weight-byte unpack, `s_bfe`/
`s_sext`/`s_ashr`) — but SALU co-issues on the scalar unit and is hidden
behind the VALU stream (hot-shader VALU 26 K vs SALU 24 K, `latency ==
inv.throughput`, stall% = 0). The remaining non-mad24 VALU is:
* address/index math (~7 K adds + shifts) — AMD's flat-index computation,
  partially folded by ACO's `v_lshl_add_u32` already;
* fp32 requant layers (~25 K fp ops) — AMD's own model math;
* 1.2 K unfused mul24.

Realistic codegen endpoint from here: ~16–17 ms. **15 ms is not reachable by
codegen** — 1.0 VALU/MAC is the floor without dp4a. Below that only work
reduction: MV reprojection on skipped frames (next), tile draft-and-verify
(needs custom kernel).

## Upstreaming
Both rules are generically correct for any `has_mul24_relaxed` hardware (all
GCN→RDNA). Worth a Mesa MR once visually validated.
