# Winograd F(2×2,3×3) vs direct 3×3 convolution — measured on RX 470

Isolated microbenchmarks, run with `dp4a_bench/bench <shader>.spv <iterations> POLARIS10`.
Sources in this directory. Each iteration produces one 2×2 output tile from a 3×3 kernel.

## Single channel (C=1) — Winograd's worst case, transforms not amortised

| shader | 1000 iters | 3000 iters |
|---|---|---|
| `direct_conv3x3` | 40.43 ms | 121.62 ms |
| `winograd_f23`   | 42.50 ms | 127.32 ms |

**Winograd loses by ~5%.** Expected: it trades multiplies for adds, and on issue-bound GCN an add
costs exactly as much as a multiply, so there is nothing to gain when transforms cannot amortise.

## 8 channels (C=8) — representative of a real conv layer

| shader | 500 iters | 1500 iters | vs direct |
|---|---|---|---|
| `direct_conv3x3_mc`     | 55.23 ms | 168.53 ms | 1.00× |
| `winograd_f23_mc`       | 24.00 ms |  71.81 ms | **2.30×** — *numerically unsafe* |
| `winograd_f23_mc_i32`   | 40.62 ms | 121.66 ms | **1.36×** — correct |

With channels the 16-vs-36 multiply ratio dominates while transforms amortise, and Winograd wins.

**The int16 variant is not usable.** Winograd's input transform sums up to four int8 values
(range ±508), so element-wise products reach ~64,500 and overflow int16. `winograd_f23_mc_i32`
keeps the products and accumulation in int32 (shifting back to int16 afterwards) and is the
honest number: **~1.36–1.38×**, consistent at both iteration counts.

That also matches the analytical estimate: direct 288 MACs × ~1.5 VALU/MAC ≈ 432 VALU versus
Winograd ≈ 32 (input transform) + 8×32 (element-wise) + 24 (output transform) ≈ 312 VALU ≈ 1.4×.

## Conclusion
Winograd is worth roughly **1.36×** here, not the textbook 2.25× — that figure assumes multiplies
cost more than adds, which is false on issue-bound GCN. Applied to the measured 26.4 ms upscaler
time this predicts ~19.4 ms. Real, but it requires a custom kernel and knowledge of which layers
are actually 3×3.

Untested: F(4×4,3×3) has a better multiply ratio (36 multiplies for 16 outputs) but much larger
transforms and worse numerics; on issue-bound hardware the transform growth probably cancels it.
