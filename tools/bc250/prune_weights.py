#!/usr/bin/env python3
"""Drop the least useful weights from the BC-250 shaders, by share rather than by size.

An absolute threshold does not work here. These weights are baked literals and their scale differs
per layer, from single digits to over three thousand, so one number means "drop almost nothing" in
one layer and "drop almost everything" in the next.

This instead drops the smallest weights of each accumulator chain until their combined magnitude
reaches a share of that chain's total, then scales what is left to put the magnitude back. The share
is the quality knob and it means the same thing in every layer.

The surviving weights are rounded back to whole numbers. Every original weight is one, and GCN can
carry a small whole number inside the instruction, where a fraction needs a literal dword that costs
more than the multiply-accumulate the pruning removed.

Usage: gfx803_prune2.py <shader dir> <share> [roles to leave alone, default postpass]
"""
import re
import struct
import sys
from pathlib import Path

RE_FMAD = re.compile(r"^\s*%(\S+) = call float @dx\.op\.tertiary\.f32\(i32 46, float ([^,]+), float ([^,]+), float ([^)]+)\)$")
RE_MUL16 = re.compile(r"^\s*%(\S+) = mul <2 x i16> %(\S+), <i16 (-?\d+), i16 (-?\d+)>$")
RE_FLOATLIT = re.compile(r"^-?[0-9]+\.[0-9]+e[+-][0-9]+$")


def fmt_float(v):
    v = float(round(v))
    return "%.9e" % v


def pick_drops(ws, share):
    """Which terms to drop: the smallest, until their magnitude reaches the share."""
    total = sum(abs(w) for w in ws)
    if total <= 0:
        return set(), 1.0
    budget = total * share
    order = sorted(range(len(ws)), key=lambda i: abs(ws[i]))
    drop, spent = set(), 0.0
    for i in order:
        if spent + abs(ws[i]) > budget:
            break
        drop.add(i)
        spent += abs(ws[i])
    left = total - spent
    if not left or not drop:
        return set(), 1.0
    return drop, total / left


def chains_of(lines):
    seen, out = set(), []
    for i, l in enumerate(lines):
        m = RE_FMAD.match(l)
        if not m or i in seen:
            continue
        chain, cur = [i], m.group(1)
        seen.add(i)
        while True:
            nxt = None
            for j in range(chain[-1] + 1, min(chain[-1] + 8, len(lines))):
                mm = RE_FMAD.match(lines[j])
                if mm and mm.group(4).strip() == "%" + cur:
                    nxt, cur = j, mm.group(1)
                    break
            if nxt is None or nxt in seen:
                break
            chain.append(nxt)
            seen.add(nxt)
        if len(chain) > 2:
            out.append(chain)
    return out


def prune_floats(lines, share):
    edits, dropped, kept = {}, 0, 0
    for chain in chains_of(lines):
        ws = []
        for i in chain:
            tok = RE_FMAD.match(lines[i]).group(3).strip()
            ws.append(float(tok) if RE_FLOATLIT.match(tok) else None)
        if any(w is None for w in ws):
            continue
        drop, scale = pick_drops(ws, share)
        if not drop:
            continue
        prev = None
        for pos, i in enumerate(chain):
            m = RE_FMAD.match(lines[i])
            acc = m.group(4).strip() if prev is None else prev
            if pos in drop:
                edits[i] = f"  %{m.group(1)} = fadd float {acc}, 0.000000e+00"
                dropped += 1
            else:
                edits[i] = (f"  %{m.group(1)} = call float @dx.op.tertiary.f32(i32 46, "
                            f"float {m.group(2).strip()}, float {fmt_float(ws[pos] * scale)}, "
                            f"float {acc})")
                kept += 1
            prev = "%" + m.group(1)
    return edits, dropped, kept


def prune_packed(lines, share):
    """The packed pairs, per lane, over the whole shader. Their weights share one scale."""
    edits, dropped, kept = {}, 0, 0
    sites = [(i, int(m.group(3)), int(m.group(4)))
             for i, l in enumerate(lines) for m in [RE_MUL16.match(l)] if m]
    if not sites:
        return edits, 0, 0
    new = {i: [w0, w1] for i, w0, w1 in sites}
    for lane in (0, 1):
        ws = [w[1 + lane] for w in sites]
        drop, scale = pick_drops(ws, share)
        if not drop:
            continue
        for n, (i, _, _) in enumerate(sites):
            if n in drop:
                new[i][lane] = 0
                dropped += 1
            else:
                v = int(round(new[i][lane] * scale))
                new[i][lane] = max(-32768, min(32767, v))
                kept += 1
    for i, _, _ in sites:
        m = RE_MUL16.match(lines[i])
        w0, w1 = new[i]
        edits[i] = f"  %{m.group(1)} = mul <2 x i16> %{m.group(2)}, <i16 {w0}, i16 {w1}>"
    return edits, dropped, kept


def main():
    d, share = Path(sys.argv[1]), float(sys.argv[2])
    # Roles to leave alone. The postpass writes the picture and the history buffer the next frame
    # reads, so an error there does not fade, it comes back every frame as noise.
    skip = [x for x in (sys.argv[3].split(",") if len(sys.argv) > 3 else ["postpass"]) if x]
    import json
    manifest = json.loads((d.parent / "manifest.json").read_text())
    role = {r["source"].split("/")[-1]: r["entry"] for r in manifest["replacements"]}
    td = tk = files = skipped = 0
    for f in sorted(d.glob("*.ll")):
        entry = role.get(f.name, "")
        if any(k and k in entry for k in skip):
            skipped += 1
            continue
        lines = f.read_text().split("\n")
        e1, d1, k1 = prune_floats(lines, share)
        e2, d2, k2 = prune_packed(lines, share)
        if not e1 and not e2:
            continue
        for i, new in {**e1, **e2}.items():
            lines[i] = new
        f.write_text("\n".join(lines))
        td += d1 + d2
        tk += k1 + k2
        files += 1
    print(f"share {share}: dropped {td} weights, rescaled {tk}, in {files} shaders; "
          f"left {skipped} alone ({', '.join(skip)})")


if __name__ == "__main__":
    main()
