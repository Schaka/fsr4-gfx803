#!/usr/bin/env python3
"""Pick FSR4's weight buffer out of a capture and write it to one file.

The learning run dumps every small buffer that FSR4 fills on the GPU. The weight buffer is the one
the network passes index, and it stands out: it is dense int8 data, and its byte values spread over
the whole range. Output tensors are far sparser, and constant buffers are mostly zero.

Usage: pick_weights.py <dump_dir> <weights.bin>
"""
import os
import sys

d, out = sys.argv[1], sys.argv[2]
best = None
for name in sorted(os.listdir(d)):
    if not name.endswith(".bin"):
        continue
    b = open(os.path.join(d, name), "rb").read()
    if not 16384 <= len(b) <= (1 << 21):
        continue
    zeros = b.count(0) / len(b)
    distinct = len(set(b))
    mean = sum(abs(v - 256 if v > 127 else v) for v in b[:65536]) / min(len(b), 65536)
    # Weights: few zeros, every byte value present, and a middling mean magnitude.
    if zeros > 0.25 or distinct < 250 or not 10 <= mean <= 60:
        continue
    score = len(b) * (1 - zeros)
    if best is None or score > best[0]:
        best = (score, name, b, zeros, mean)

if best is None:
    sys.exit(f"no weight-like buffer in {d}; was the learning run made with FSR4_FORCE_HOST_WEIGHTS=1?")

_, name, b, zeros, mean = best
open(out, "wb").write(b)
print(f"weights from {name}: {len(b)} bytes, {100 * zeros:.1f}% zero, mean magnitude {mean:.1f}")
