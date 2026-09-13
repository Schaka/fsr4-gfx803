# Collected data

This directory holds analysis data, not results. It was captured in the FSR SDK sample in August
2026, before the Mesa patch existed, against **FSR4 4.0.2 INT8**
(`fsr4_dlls/4.0.2-int8/`, md5 `0ca99991ce3669d1d5320eb011aadb25`) on an RX 470. Most of it comes
from `tools/collect_data.sh`.

| File | What it is |
|---|---|
| `benchmarks/benchmarks.csv` | SDK sample frametimes per decomposition mode on stock RADV |
| `shaderstats/shaderstats_mad16.log` | raw ACO `RADV_DEBUG=shaderstats` output |
| `shaderstats/shader_stats.csv` | parsed: instructions, code size, occupancy, latency vs throughput, **stall %** |
| `isa/isa_mad16.log.gz` | full GCN ISA disassembly of every shader (27MB uncompressed) |
| `dispatch_trace.log` | every `Dispatch(x,y,z)` for ~20s, traced in vkd3d |
| `dispatch_topology.csv` | distinct dispatch shapes + how often each is issued per run |
| `spirv/*.spv` | SPIR-V for all 98 shaders the app compiles |
| `spirv_inventory.csv` | per-shader op counts; `bitfield_extract` ≈ int8 MAC density |
| `resource_inventory.log` | every D3D12 resource the sample creates, with sizes and heap types |
| `topology.md` | the network layers, reconstructed from the dispatch shapes and shaders |
| `roofline.md` | what is and is not reachable on this hardware |
| `imul24/` | ISA instruction counts with the patched RADV. See `imul24/README.md`. |
| `weights/`, `weights_runtime/` | recovered INT8 weights. See the sections below. |
| `activations/` | a sample of intermediate activations. See `activations/README.md`. |
| `winograd/` | a hand-written Winograd convolution experiment. See `winograd/RESULTS.md`. |

## The numbers that matter

**Hot shaders are ~100% instruction-issue bound.** From `shader_stats.csv`:

| instructions | code | waves/SIMD | stall % | VMEM clauses |
|---|---|---|---|---|
| 61535 | 382 KB | 1 | **0.0** | 19 |
| 44979 | 279 KB | 1 | **0.0** | 6 |
| 44979 | 279 KB | 1 | **0.0** | 6 |

`stall% = (latency - inverse_throughput) / latency`. Zero stalls with almost no memory traffic
means: **nothing is waiting on memory, so nothing can be fixed by caching, prefetching, more VRAM,
or higher occupancy.** Only fewer instructions or fewer MACs help. Weights are baked into the
instruction stream as immediates, which is why code size is enormous and VMEM is ~nil.

The 5th shader (14934 instrs, 156 VMEM clauses, 9216 scratch) is the one exception that *does*
spill — worth a look if someone wants a small independent win.

## Network topology
`dispatch_topology.csv` has 18 distinct dispatch shapes, ~19 upscaler dispatches per frame.
Cross-reference with `spirv_inventory.csv` to map layers to shaders: the top 5 shaders by
`bitfield_extract` count carry essentially all the int8 MAC work
(`d4ef88fbc345bce2` alone: 46,080 byte-extracts / 23,062 multiplies).
These two files plus `spirv/` are the raw material for reimplementing the network by hand.

## Extracted weights (`weights/`)
`weights/*.i8` — raw int8 weight bytes recovered from shaders that bake them as immediates,
in the order the shader consumes them. `weights/summary.json` has per-shader counts and the
full value histogram.

**36,608 baked weights** were recovered from 9 shaders. The remaining **168,192** MAC operands in
the hot shaders are *not* baked — they are loaded at runtime from StorageBuffers (679 loads feed
46,080 byte-extracts in the largest shader, i.e. each load is reused heavily, which is why VMEM
traffic is negligible). Recovering those requires dumping the SSBO at dispatch time, not static analysis.

Note the DLL is **36.7 MB of DXBC shader blobs** (1,325 of them) out of 40.7 MB total — the model is
compiled into shaders rather than stored as a weight tensor. Only ~2.1 MB is non-shader data.

## Runtime weight buffers (`weights_runtime/`)
28 unique GPU-produced weight buffers, 668 KB, recovered by forcing weight-sized DEFAULT-heap
buffers into host-visible memory (`FSR4_FORCE_HOST_WEIGHTS=1`). Full method, reproduction steps and
caveats in `weights_runtime/README.md`. These are the weights the hot shaders actually stream —
`weights/` holds only the statically baked ones.

See `roofline.md` before planning any optimisation work.
