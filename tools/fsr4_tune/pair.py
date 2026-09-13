"""Rewrite a flat FSR4 pass so that two output channels share one multiply.

The flat shader accumulates each output as `acc = bias + sum(x_i * w_i)`, one multiply per weight.
Two accumulators that read the same activations can share their multiplies: quantize both weight sets
to BITS bits, put the pair into one operand as `q_a + (q_b << 16)`, and one multiply-add then feeds
the low half of the accumulator with `q_a * x` and the high half with `q_b * x`. A bias of 32768 keeps
the low half non-negative, so the halves never mix, and the accumulator is flushed every CHUNK steps
to stay inside 32 bits. Each sum is recovered with a mask and a shift, scaled back by its own channel
step, and handed to the unchanged tail of the shader.

Dead multiplies are left in place, because the shader compiler removes them.

Usage: pair5.py in.glsl out.glsl [bits]
"""
import os
import re
import sys
from collections import Counter, defaultdict

BITS = int(sys.argv[3]) if len(sys.argv) > 3 else 5
# PRUNE=T first drops every weight of magnitude T or less, then quantizes what is left. The pair
# keeps only the terms where at least one of the two weights survived, so the multiplies go away.
PRUNE = int(os.environ.get("PRUNE", "0"))
LIM = 2 ** (BITS - 1) - 1
CHUNK = 32768 // (2 ** (BITS - 1) * 128)

src = open(sys.argv[1]).read()
head, body = src.split("void main()", 1)

defs, order, pre, post = {}, [], [], []
for raw in body.splitlines():
    s = raw.strip()
    m = re.match(r"(\w+) (n\d+) = (.*);$", s)
    if m:
        defs[m.group(2)] = (m.group(1), m.group(3))
        order.append(m.group(2))
    elif s.startswith(("uint GX", "uint GY", "if (GX", "if (GY")):
        pre.append(s)
    elif s and s not in "{}" and not s.startswith("void"):
        post.append(s)

pos = {v: i for i, v in enumerate(order)}
MUL = re.compile(r"^(n\d+) \* \((\d+)u?\)$")
ADD = re.compile(r"^(n\d+) \+ (n\d+)$")
s32 = lambda v: v - 2 ** 32 if v >= 2 ** 31 else v

muls = {v: (m.group(1), s32(int(m.group(2)))) for v in order for m in [MUL.match(defs[v][1])] if m}
adds = {v: m.groups() for v in order for m in [ADD.match(defs[v][1])] if m}

first_use = {}
for i, v in enumerate(order):
    for r in re.findall(r"n\d+", defs[v][1]):
        first_use.setdefault(r, i)
for s in post:
    for r in re.findall(r"n\d+", s):
        first_use.setdefault(r, len(order))

used_by_add = {x for v, (a, b) in adds.items() for x in (a, b) if x in adds}
roots = [v for v in adds if v not in used_by_add]


def expand(v, w, other):
    if v in muls:
        act, k = muls[v]
        w[act] += k
    elif v in adds:
        a, b = adds[v]
        expand(a, w, other)
        expand(b, w, other)
    else:
        other.append(v)


chains = {}
for r in roots:
    w, other = defaultdict(int), []
    expand(r, w, other)
    if len(w) >= 8:
        chains[r] = (dict(w), other)

# Pair chains that share activations. The packed block goes just before the first use of either
# root, which has to be after every activation both chains read.
# A pair block may only stand after every value it reads: the activations and the bias terms.
act_pos = {}
for r, (w, other) in chains.items():
    act_pos[r] = max((pos[a] for a in list(w) + list(other) if a in pos), default=-1)

pairs, used, where = [], set(), {}
cand = sorted(chains, key=lambda r: pos[r])
for r in cand:
    if r in used:
        continue
    best, score, slot = None, 0, None
    for s2 in cand:
        if s2 == r or s2 in used:
            continue
        p = min(first_use.get(r, len(order)), first_use.get(s2, len(order)))
        if p <= max(act_pos[r], act_pos[s2]):
            continue
        ov = len(set(chains[r][0]) & set(chains[s2][0]))
        if ov > score:
            best, score, slot = s2, ov, p
    if best is not None and score >= 8:
        used.add(r)
        used.add(best)
        pairs.append((r, best))
        where[(r, best)] = slot

place = defaultdict(list)
for a, b in pairs:
    place[where[(a, b)]].append((a, b))
skip = {v for p in pairs for v in p}

def quant(ws):
    if PRUNE:
        ws = {k: (v if abs(v) > PRUNE else 0) for k, v in ws.items()}
    mx = max(abs(x) for x in ws.values()) or 1
    step = mx / LIM
    return {k: max(-LIM - 1, min(LIM, round(v / step))) for k, v in ws.items()}, step


out_lines = []


def emit_pair(a, b):
    wa, sa = quant(chains[a][0])
    wb, sb = quant(chains[b][0])
    acts = sorted(set(wa) | set(wb), key=lambda n: int(n[1:]))
    tag = f"{a}_{b}"
    out_lines.append(f"    int lo_{tag} = 0, hi_{tag} = 0;")
    for c0 in range(0, len(acts), CHUNK):
        terms = [f"{wa.get(x, 0) + (wb.get(x, 0) << 16)} * int({x})"
                 for x in acts[c0:c0 + CHUNK] if wa.get(x, 0) or wb.get(x, 0)]
        if terms:
            out_lines.append(f"    int p_{tag}_{c0} = 32768 + " + " + ".join(terms) + ";")
            out_lines.append(f"    lo_{tag} += (p_{tag}_{c0} & 0xFFFF) - 32768; hi_{tag} += p_{tag}_{c0} >> 16;")
    for root, sc, half in ((a, sa, f"lo_{tag}"), (b, sb, f"hi_{tag}")):
        bias = " + ".join(chains[root][1]) or "0u"
        out_lines.append(f"    {defs[root][0]} {root} = uint(int(float({half}) * {sc:.9f})) + ({bias});")


for i, v in enumerate(order):
    for a, b in place.get(i, []):
        emit_pair(a, b)
    if v in skip:
        continue
    out_lines.append(f"    {defs[v][0]} {v} = {defs[v][1]};")
for a, b in place.get(len(order), []):
    emit_pair(a, b)

text = head + "void main()\n{\n" + "\n".join("    " + p for p in pre) + "\n" + "\n".join(out_lines) + "\n" + "\n".join("    " + p for p in post) + "\n}\n"
open(sys.argv[2], "w").write(text)

live = set()
stack = [r for s in post for r in re.findall(r"n\d+", s)]
kept = {v for v in order if v not in skip}
packed_macs = sum(len(re.findall(r"\* int\(n\d+\)", l)) for l in out_lines)
plain_macs = sum(1 for v in kept if v in muls)
print(f"{sys.argv[2]}: chains {len(chains)}, paired {len(pairs)}, "
      f"multiplies {len(muls)} -> about {packed_macs + plain_macs}, bits {BITS}, chunk {CHUNK}")
