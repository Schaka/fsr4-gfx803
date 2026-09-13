"""Pair the output head's multiplies, the same trick pair5.py applies to the network passes.

The head keeps its weights as dword literals, and each output accumulates in one statement of the
form `... (0u + (x0 * w0) + (x1 * w1) + ...) ...`. Two statements that read the same activations
share their multiplies: both weight sets are quantized to BITS bits, packed as `q_a + (q_b << 16)`,
and one multiply-add then serves both. The sums are recovered with a mask and a shift and put back
into each statement in place of its sum.

Usage: pairhead.py in.glsl out.glsl [bits]
"""
import re
import sys
from collections import defaultdict

BITS = int(sys.argv[3]) if len(sys.argv) > 3 else 5
LIM = 2 ** (BITS - 1) - 1
CHUNK = 32768 // (2 ** (BITS - 1) * 128)

lines = open(sys.argv[1]).read().splitlines()
MAC = re.compile(r"\(uint\(bitfieldExtract\(int\((\w+)\), int\((\d+)u\), int\(8u\)\)\) \* "
                 r"uint\(bitfieldExtract\(int\((\d+)u\), int\((\d+)u\), int\(8u\)\)\)\)")


def weight_byte(const, shift):
    v = (int(const) >> int(shift)) & 255
    return v - 256 if v > 127 else v


def seed(line):
    """Index of the `(0u + ` that starts the accumulation chain."""
    i = line.find("(0u + ")
    return i if i >= 0 else None


def rewrite(line, i, expr):
    """Put expr in place of the chain seed and blank every multiply, leaving the brackets alone."""
    rest = line[i:].replace("(0u + ", "(" + expr + " + ", 1)
    return line[:i] + MAC.sub("0u", rest)


acc = {}                      # line index -> (weights dict, span)
for n, line in enumerate(lines):
    terms = MAC.findall(line)
    if len(terms) < 8:
        continue
    i = seed(line)
    if i is None:
        continue
    w = defaultdict(int)
    for act, shift, const, shift2 in terms:
        if shift != shift2:
            continue
        w[f"{act}_{shift}"] += weight_byte(const, shift)
    if len(w) >= 8:
        acc[n] = (dict(w), i)

# Pair statements that share activations, nearest first so the packed block can sit just above them.
pairs, used = [], set()
keys = sorted(acc)
for a in keys:
    if a in used:
        continue
    best, score = None, 0
    for b in keys:
        if b <= a or b in used:
            continue
        ov = len(set(acc[a][0]) & set(acc[b][0]))
        if ov > score:
            best, score = b, ov
    if best is not None and score >= 8:
        used.add(a)
        used.add(best)
        pairs.append((a, best))


def quant(ws):
    mx = max(abs(v) for v in ws.values()) or 1
    step = mx / LIM
    return {k: max(-LIM - 1, min(LIM, round(v / step))) for k, v in ws.items()}, step


block_at = defaultdict(list)
repl = {}
for a, b in pairs:
    wa, sa = quant(acc[a][0])
    wb, sb = quant(acc[b][0])
    acts = sorted(set(wa) | set(wb))
    tag = f"h{a}_{b}"
    out = [f"    int lo_{tag} = 0, hi_{tag} = 0;"]
    for c0 in range(0, len(acts), CHUNK):
        terms = []
        for key in acts[c0:c0 + CHUNK]:
            node, shift = key.rsplit("_", 1)
            packed = wa.get(key, 0) + (wb.get(key, 0) << 16)
            if packed:
                terms.append(f"{packed} * bitfieldExtract(int({node}), {shift}, 8)")
        if terms:
            out.append(f"    int p_{tag}_{c0} = 32768 + " + " + ".join(terms) + ";")
            out.append(f"    lo_{tag} += (p_{tag}_{c0} & 0xFFFF) - 32768; hi_{tag} += p_{tag}_{c0} >> 16;")
    block_at[min(a, b)] = out
    repl[a] = (f"uint(int(float(lo_{tag}) * {sa:.9f}))", tag)
    repl[b] = (f"uint(int(float(hi_{tag}) * {sb:.9f}))", tag)

out_lines = []
for n, line in enumerate(lines):
    if n in block_at:
        out_lines += block_at[n]
    if n in repl:
        line = rewrite(line, acc[n][1], repl[n][0])
    out_lines.append(line)

open(sys.argv[2], "w").write("\n".join(out_lines) + "\n")
before = sum(len(MAC.findall(l)) for l in lines)
after = sum(len(MAC.findall(l)) for l in out_lines) + sum(len(re.findall(r"\* bitfieldExtract", l)) for l in out_lines)
print(f"{sys.argv[2]}: accumulators {len(acc)}, paired {len(pairs)}, multiplies {before} -> {after}, bits {BITS}, chunk {CHUNK}")
