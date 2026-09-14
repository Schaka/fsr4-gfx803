#!/usr/bin/env python3
"""Promote the prepass fp16 arithmetic to fp32, whole subgraph at a time, for GCN4.

GCN4 runs fp16 at the same rate as fp32, so every convert between them is spent for nothing. The
prepass converts a loaded pair of halves to floats, converts them straight back to halves, does its
arithmetic in fp16, and then the driver converts each product back to fp32 to accumulate it.

Promoting one operation at a time does not pay, because each promoted operation then needs its
operands converted. This promotes the closure: a half value is promotable when it is a truncated
float, a half literal, or an operation whose operands are all promotable. Everything in that set is
rewritten in fp32 and every convert between the two disappears.

The result is more accurate, not less. An fp32 multiply keeps 24 bits of product where fp16 keeps 11.

Usage: fp32_prepass.py <shader dir>
"""
import re
import struct
import sys
from pathlib import Path

TAIL = r"\s*(?:;.*)?$"
RE_TRUNC = re.compile(r"^(\s*)%(\S+) = fptrunc float %(\S+) to half" + TAIL)
RE_BIN = re.compile(r"^(\s*)%(\S+) = (fadd|fsub|fmul|fdiv) (fast )?half ([^,;]+), ([^,;]+?)" + TAIL)
RE_DOT2 = re.compile(
    r"^(\s*)%(\S+) = call float @dx\.op\.dot2AddHalf\.f32\(i32 162, float ([^,]+), "
    r"half ([^,]+), half ([^,]+), half ([^,]+), half ([^)]+)\)" + TAIL)
RE_ANYHALF = re.compile(r"\bhalf\b")


def half_literal_to_float(tok):
    """0xHxxxx is a half constant. Give back the float literal for the same value."""
    if not tok.startswith("0xH"):
        return None
    bits = int(tok[3:], 16)
    v = struct.unpack("<e", struct.pack("<H", bits))[0]
    if v != v or v in (float("inf"), float("-inf")):
        return None
    return "0x%X" % struct.unpack("<Q", struct.pack("<d", float(v)))[0]


def main():
    d = Path(sys.argv[1])
    files = dots = ops = 0
    for f in sorted(d.glob("*.ll")):
        text = f.read_text()
        if "dot2AddHalf" not in text:
            continue
        lines = text.split("\n")

        # pass one: what is promotable, and what does it become
        val = {}                       # half name -> float text
        producer = {}
        for i, l in enumerate(lines):
            m = RE_TRUNC.match(l)
            if m:
                val[m.group(2)] = "%" + m.group(3)
                producer[m.group(2)] = i
                continue
            m = RE_BIN.match(l)
            if m:
                producer[m.group(2)] = i
        changed = True
        while changed:
            changed = False
            for i, l in enumerate(lines):
                m = RE_BIN.match(l)
                if not m or m.group(2) in val:
                    continue
                a, b = m.group(5), m.group(6)
                ta = val.get(a[1:]) if a.startswith("%") else half_literal_to_float(a)
                tb = val.get(b[1:]) if b.startswith("%") else half_literal_to_float(b)
                if ta and tb:
                    val[m.group(2)] = "%" + m.group(2) + ".f32"
                    changed = True

        out, nd, no = [], 0, 0
        for i, l in enumerate(lines):
            m = RE_BIN.match(l)
            if m and m.group(2) in val and val[m.group(2)].endswith(".f32"):
                a, b = m.group(5), m.group(6)
                ta = val.get(a[1:]) if a.startswith("%") else half_literal_to_float(a)
                tb = val.get(b[1:]) if b.startswith("%") else half_literal_to_float(b)
                out.append(f"{m.group(1)}{val[m.group(2)]} = {m.group(3)} {m.group(4) or ''}"
                           f"float {ta}, {tb}")
                out.append(l)                  # kept: dead code elimination drops it if unused
                no += 1
                continue
            m = RE_TRUNC.match(l)
            if m:
                out.append(l)                  # kept for the same reason
                continue
            m = RE_DOT2.match(l)
            if m:
                ind, res, acc = m.group(1), m.group(2), m.group(3).strip()
                got, pre = [], []
                for tok in (m.group(4), m.group(5), m.group(6), m.group(7)):
                    tok = tok.strip()
                    t = val.get(tok[1:]) if tok.startswith("%") else half_literal_to_float(tok)
                    if t:
                        got.append(t)
                    else:
                        n = f"%h803x{i}_{len(pre)}"
                        pre.append(f"{ind}{n} = fpext half {tok} to float")
                        got.append(n)
                out.extend(pre)
                t = f"%h803t{i}"
                # Dot2AddHalf(acc, ax, ay, bx, by) is acc + ax*bx + ay*by. The first pair is the
                # a vector and the second the b vector, so the multiplies cross between them.
                out.append(f"{ind}{t} = call float @dx.op.tertiary.f32(i32 46, float {got[1]}, "
                           f"float {got[3]}, float {acc})")
                out.append(f"{ind}%{res} = call float @dx.op.tertiary.f32(i32 46, float {got[0]}, "
                           f"float {got[2]}, float {t})")
                nd += 1
                continue
            out.append(l)

        text = "\n".join(out)
        if "declare float @dx.op.tertiary.f32" not in text:
            text = text.replace("declare float @dx.op.dot2AddHalf.f32",
                                "declare float @dx.op.tertiary.f32(i32, float, float, float) #0\n"
                                "declare float @dx.op.dot2AddHalf.f32", 1)
        # the validator rejects a declaration nothing calls
        if "@dx.op.dot2AddHalf.f32(" not in re.sub(r"^declare.*$", "", text, flags=re.M):
            text = re.sub(r"^declare float @dx\.op\.dot2AddHalf\.f32.*\n", "", text, flags=re.M)
        f.write_text(text)
        files += 1
        dots += nd
        ops += no
    print(f"promoted {dots} dot products and {ops} fp16 operations in {files} shaders")


if __name__ == "__main__":
    main()
