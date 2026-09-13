"""Prune multiplies by baked constant weight bytes in a decompiled shader (spirv-cross GLSL).

A term `uint(bitfieldExtract(int(X), int(Ku), int(8u))) * uint(bitfieldExtract(int(Cu), int(Ku), int(8u)))`
with a literal dword C is a MAC with weight byte (C >> K) & 255. If |byte| <= threshold, the term becomes 0u.
With WSCALE=1 in the environment, the surviving weights take over the magnitude the pruned ones carried.
Usage: prune_const.py in.glsl out.glsl threshold"""
import os
import re
import sys

src = open(sys.argv[1]).read()
thr = int(sys.argv[3])
scale = 1.0
bfe = r"uint\(bitfieldExtract\(int\((\w+)\), int\((\d+)u\), int\(8u\)\)\)"
term = re.compile(r"\(" + bfe + r" \* " + bfe + r"\)")
total = pruned = 0
wbytes = []


def sub(m):
    global total, pruned
    a, ka, b, kb = m.groups()
    const = [(x[:-1], int(k)) for x, k in ((a, ka), (b, kb)) if re.fullmatch(r"\d+u", x)]
    if len(const) != 1:
        return m.group(0)
    c, k = const[0]
    w = ((int(c) >> k) & 255)
    w = w - 256 if w > 127 else w
    total += 1
    wbytes.append(abs(w))
    if abs(w) <= thr:
        pruned += 1
        return "0u"
    if scale != 1.0:
        # The weight is one byte of a shared dword, so the scaled value replaces the whole term
        # with a plain multiply by a literal.
        v = int(round(w * scale))
        other = a if re.fullmatch(r"\d+u", b) else b
        k = ka if re.fullmatch(r"\d+u", b) else kb
        return f"uint(bitfieldExtract(int({other}), int({k}u), int(8u)) * {v})"
    return m.group(0)


if os.environ.get("WSCALE"):
    term.sub(sub, src)          # first sweep only measures
    kept = [w for w in wbytes if w > thr]
    scale = sum(wbytes) / sum(kept) if kept else 1.0
    print(f"  weight magnitude kept {sum(kept) / sum(wbytes):.3f}, scaling by {scale:.3f}")
    total = pruned = 0
    wbytes = []
out = term.sub(sub, src)
open(sys.argv[2], "w").write(out)
wbytes.sort()
q = lambda f: wbytes[int(f * (len(wbytes) - 1))] if wbytes else 0
print(f"{sys.argv[2]}: constant MACs {total}, pruned {pruned} ({100 * pruned / max(total, 1):.1f}%), |w| quartiles {q(.25)} {q(.5)} {q(.75)}")
