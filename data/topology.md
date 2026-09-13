# Recovered network topology (static analysis of the dumped SPIR-V)

| shader | LocalSize | bindings | stores (outputs/thread) | MACs/thread | MACs/output |
|---|---|---|---|---|---|
| `d4ef88fbc345bce2` (largest) | 8×8×1 = 64 | 3 | 8 | 23,040 | **2,880** |
| `67a84454521597b9` | 64×1×1 | 3 | 16 | 16,896 | 1,056 |
| `33fc02c0d8eb3d9e` | 64×1×1 | 1 | 16 | 8,448 | 528 |
| `362807e95e196f1f` | 64×1×1 | 2 | 16 | 8,192 | 512 |

All are pure SSBO compute — **zero** `OpImageRead`/`OpImageWrite`. One workgroup = one wave64.

## Access pattern
The largest shader issues 1,920 `OpAccessChain`s; 1,312 of them use constant final indices that are
**perfectly consecutive (stride 1, range 3752..5063)** — a flat, contiguous scan, not a 2D spatial
gather. Only 679 `OpLoad`s feed 46,080 byte-extracts, i.e. each loaded dword is reused ~17×, and the
weight-side extracts are wave-uniform (they land on the scalar unit as `s_bfe_i32`).

## Is it a 3×3 convolution? — **ANSWERED: no.**
Autocorrelation on the captured activation tensor shows no byte replication at any stride from 1 to
4.2 M (best ratio 1.81× vs a required ~1.0 absolute). It is **not** im2col-expanded, so
`2,880 MACs/output` is `1 × 1 × 2,880`: these are 1×1 convolutions / matmuls over ~2,880 input
channels. **Winograd does not apply.** Full method and numbers in `activations/README.md`.

## Weights
The hot shaders stream their weights from SSBOs. 36,608 weights are baked into shaders as constants
and are in `weights/`. The streamed buffers are produced on the GPU by the small setup dispatches
(`1×1×1`, `1600×1×1` in `dispatch_topology.csv`). They were read back with
`FSR4_FORCE_HOST_WEIGHTS=1` and are in `weights_runtime/`.

## Runtime resource inventory (`resource_inventory.log`)
Captured by instrumenting `d3d12_device_CreateCommittedResource1` in vkd3d (first 400 resources).

400 committed resources: 216 buffers, 184 textures. DEFAULT-heap buffers total **239.5 MB**:

| | |
|---|---|
| 3 × 83,232,256 B (79.4 MB) | activation / intermediate tensors |
| 197 small buffers, 1.3 MB total, median 2,448 B | **the weights** |
| 21 buffers in the 15–35 KB range | one per hot layer — the largest shader needs 8 × 2,880 = 23,040 weight bytes per wave, which lands squarely in this range |

(The 419 MB and 78.6 MB UPLOAD-heap buffers are Cauldron's own `UploadHeapSize` /
`DynamicBufferPoolSize` from `configs/cauldronconfig.json`, not FSR4.)

**Total weight footprint is ~1.3 MB**, consistent with the ~2.1 MB of non-shader data in the DLL.