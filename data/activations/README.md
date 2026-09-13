# Activation tensor sample + the im2col determination

`act_sample_1MB.bin.gz` — first 1 MB of one of the three 83,232,256-byte activation tensors,
captured with `FSR4_FORCE_HOST_WEIGHTS=1 FSR4_HOST_BIG=1` (see `../weights_runtime/README.md` for
the mechanism; `FSR4_HOST_BIG` widens the host-visible size window to 128 MB and caps each dump at
8 MB per buffer).

## Result: the hot layers are NOT 3×3 convolutions

An im2col-expanded input replicates every source byte ~9× at a fixed stride, so autocorrelation at
that stride must approach **P(match) = 1.0**. Measured on non-zero positions only (the tensor is
~48% zeros, which otherwise swamps the statistic):

| stride range | best P(match) | ratio vs baseline |
|---|---|---|
| 1 … 32,768 | 0.048 @ 128 | 2.2× |
| 65,536 … 4,194,304 | 0.036 @ 3,692,160 | 1.81× |

Baseline (stride 1, both non-zero) = 0.020. **No stride anywhere from 1 to 4.2 M shows exact
replication.** The mild 1.5–2× plateau is ordinary local correlation in feature-map data, not
duplication.

### Consequence
`2,880 MACs per output` is therefore **1 × 1 × 2,880**, not `9 × 320`. The hot layers are
1×1 convolutions / matmuls over ~2,880 input channels.

**Winograd does not apply to the hot path at all** — it is a spatial-convolution transform. The
1.36× measured in `../winograd/RESULTS.md` is real but irrelevant to where the time actually goes.

This closes the last remaining math-level lever. See `../roofline.md`.
