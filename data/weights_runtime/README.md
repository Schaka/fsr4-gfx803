# Runtime-resident weight buffers (GPU-produced), extracted 2026-08-22

28 unique buffers, 668,388 bytes. Sizes:
8496, 9020, 10376, 14904, 16200×2, 16524, 17756, 20100, 20408×3, 22392, 28668,
29292×2, 29352, 29376, 29616, 30072, 30144, 30156, 30644, 31296, 31320, 31584, 31688, 32696

Combined statistics: mean|w| = 34.5, zeros 4.81%, 256 distinct values, full −128..127 range.
(Compare `../weights/` — the statically baked weights — at mean|w| = 22.9, zeros 1.88%.)

## How these were obtained
The hot shaders stream their weights from SSBOs rather than baking them, and those SSBOs are
produced **on-GPU** by FSR4's setup dispatches — so neither static analysis nor intercepting CPU
uploads reaches them. Two routes were ruled out first:

1. **`CopyBufferRegion` interception** — captured 400 buffers, but statistical comparison against the
   known baked-weight signature showed they were Cauldron scene data (mean|v| ≈ 65, i.e. uniform
   random), not weights. FSR4 does not CPU-upload its weights.
2. **Direct `memcpy` from `mem.cpu_address`** — impossible: a probe showed DEFAULT-heap buffers are
   device-local on this small-BAR box (`cpu_address == NULL` for **0 of 2450**).

The working route avoids GPU readback plumbing entirely: intercept
`d3d12_device_CreateCommittedResource1` so that DEFAULT-heap **buffers between 4 KB and 256 KB** are
allocated from a `D3D12_HEAP_TYPE_CUSTOM` heap with `CPU_PAGE_PROPERTY_WRITE_BACK` +
`MEMORY_POOL_L0` — i.e. host-visible system memory. `mem.cpu_address` then becomes valid, and the
buffers can simply be `memcpy`'d out after the GPU has filled them (dump fires on dispatch #4000).

Reproduce with:

    FSR4_FORCE_HOST_WEIGHTS=1   # plus VKD3D_DEBUG=err to see progress

1,197 host-visible buffers (19 MB) are captured; the 28 here are those passing a weight-signature
filter (12 ≤ mean|w| ≤ 40, zeros < 8%, ≥200 distinct byte values) and deduplicated by MD5.

**This is a capture mode only** — it moves those buffers off VRAM into system memory and costs
performance. Use it to dump weights, never to measure frametimes.

## Caveats / still to verify
* The signature filter is heuristic. Combined mean|w| (34.5) and zero rate (4.81%) differ from the
  baked reference (22.9 / 1.88%), which may simply reflect different layers — or may mean some of
  these 28 are not weights. Cross-check against layer shapes before using them functionally.
* Buffer→layer mapping is not established. The hot layer needs 8 × 2,880 = 23,040 weight bytes per
  wave; several buffers here (22,392 / 28,668 / 29,292 …) are in that neighbourhood.
* **The im2col question is still open.** These are weights; resolving 3×3-im2col vs a wide 1×1
  requires the *activation* tensors (3 × 83,232,256 B), which are far above the 256 KB window used
  here. Widening the size filter would capture them, at ~250 MB of dump and a large performance hit.
