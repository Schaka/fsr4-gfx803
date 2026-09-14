# The BC-250 FSR4 DLL on GCN4

`daniel-h-0/bc250-fsr4-fork` ships a modified `amd_fidelityfx_upscaler_dx12.dll` for the AMD BC-250,
which is a gfx1013 part. It does not upscale on gfx803. This note records what it does, what happens
here, and which of its ideas are worth taking.

## What it is

348 of FSR4's shader containers, hand-edited as DXIL and recompiled, repacked into the SDK DLL by
`dll/repack_dll.py`. Twenty-seven bytes of host x86 are patched as well: the SDK's INT8 eligibility
check is forced true and its GPU capability test is skipped. The model, the tensor shapes and the
pass count are unchanged, so the work is the same work, done differently.

Their own measurements are GPU time for one upscale dispatch at FSR 4.1.1 Quality, on a BC-250:
7.13 ms becomes 3.93 ms at 1080p, 25.72 ms becomes 12.08 ms at 4K. The repository publishes the raw
timestamps and a script that re-derives every figure, and the outputs are byte-identical to stock.

## What happens on gfx803

Measured on an RX 570 in Pragmata at 1280x720 to 1920x1080, under OptiScaler 10.0.0-pre1 with the
settings the fork asks for. The layer reports the GPU time.

| upscaler DLL | upscaler GPU time | network dispatches per frame | frametime |
|---|---:|---:|---:|
| FSR4 4.1.1b | 24.3 ms | 26.2 | 30.66 ms |
| BC-250 RC9 | 1.6 ms | 4.0 | 8.25 ms |

FSR4 runs 26 network dispatches per frame here, and 29 or more in the configurations measured in
`evidence/upscaler-time-rx570/`. The BC-250 build runs four. It is not upscaling faster, it is not
upscaling. Every present returns `DXGI_ERROR_INVALID_CALL`, and on screen the frame
is a wrongly placed copy of part of the image.

The DLL loads, the model selection hook binds, the context is created and the dispatches begin, so
nothing announces the failure. Only the dispatch count gives it away, which is why the layer's
profiler reports it.

## What transfers

Their packed 16-bit path does not. It rests on `v_pk_mul_lo_u16` and `v_pk_add_i16`, which retire two
16-bit lanes per instruction. Every one of those opcodes is gated `gfx9=` in Mesa
(`src/amd/compiler/aco_opcodes.py`), and `ac_gpu_info.c` sets `has_packed_math_16bit` only from GFX9.
gfx803 has 16-bit ALU but not packed 16-bit ALU, so the same sequence costs one instruction per
multiply, which is what `v_mad_i32_i24` already gives us. gfx803 does not even have `v_mad_i16`, so
a 16-bit multiply-add there is two instructions rather than one.

Three of their ideas do transfer, in the order worth trying:

1. **A runtime weight guard.** Every specialized shader carries the weights it was built from,
   compares them against the buffer the game supplies, and votes wave-wide with `waveAllTrue`. A
   mismatch falls back to the generic path. That makes a baked shader safe to ship against a game
   that supplies different weights, which our sets currently are not.
2. **Dropping only the weights that are exactly zero.** Free, exact, and unconditionally correct.
   Our pruning drops weights below a threshold and rescales, which is lossy.
3. **Lowering the dot product in NIR rather than in SPIR-V.** They keep the compact dot form through
   one full NIR optimization round before expanding it, and choose the expansion by whether the
   operands are constant. The layer expands in SPIR-V, so NIR never sees the compact form. Their v3
   notes report a shader going from 1,314 spills to none.
