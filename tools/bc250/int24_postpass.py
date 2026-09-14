#!/usr/bin/env python3
"""Lift the postpass accumulators from 32-bit float back to 32-bit integer.

The BC-250 postpass holds the output head of the network. Its weights are whole numbers between
-128 and 128, and its activations are signed bytes, so every sum in it is integer arithmetic that
happens to be written in floating point. Each multiply-accumulate reaches the card as four
instructions:

    shl i32 %v, 16        ; move the byte to the top
    ashr i32 %s, 24       ; sign extend it
    sitofp i32 %a         ; make a float of it
    fma(%f, -8.0, %acc)   ; one v_mac_f32 with a 32-bit literal

In integer form the same work is one instruction. `Ibfe` becomes `OpBitFieldSExtract`, which NIR
folds to `extract_i8`, and the RADV patch in this repository turns `extract_i8` times a constant
that fits in 24 bits into a single `v_mad_i32_i24`.

The result is identical, not close. Signed bytes times weights of at most 128 give terms under 2^15,
and `v_mad_i32_i24` adds them into a full 32-bit accumulator, so a sum would need tens of thousands
of terms before it could overflow. Float32 is exact over that range as well, which is why the
original form worked.

SECURITY NOTE: this edits shader arithmetic only. It does not touch resource bindings, bounds
checks or addressing, so it cannot widen what a shader is able to read or write.

Usage:

    int24_postpass.py <shader dir>            rewrite every .ll in place
    int24_postpass.py --report <shader dir>   report only, change nothing
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

# The four shapes the arithmetic is written in.
RE_SHL = re.compile(r"^\s*%(\S+) = shl i32 %(\S+), (\d+)$")
RE_ASHR = re.compile(r"^\s*%(\S+) = ashr i32 %(\S+), 24$")
RE_CVT = re.compile(r"^\s*%(\S+) = sitofp i32 %(\S+) to float$")
RE_FMA = re.compile(r"^\s*%(\S+) = call float @dx\.op\.tertiary\.f32"
                    r"\(i32 46, float %(\S+), float (\S+), float (\S+)\)$")
RE_ADD0 = re.compile(r"^\s*%(\S+) = fadd float %(\S+), 0\.000000e\+00$")
RE_PHI = re.compile(r"^\s*%(\S+) = phi float (.+)$")
RE_PHI_ARM = re.compile(r"\[\s*([^,\]]+),\s*(%[A-Za-z0-9_.]+)\s*\]")
RE_DEF = re.compile(r"^\s*%([A-Za-z0-9_.]+) = ")
RE_USE = re.compile(r"%([A-Za-z0-9_.]+)")
RE_BLOCK = re.compile(r"^([A-Za-z0-9_.]+):")

# v_mad_i32_i24 reads its two multiplicands as signed 24-bit values.
S24 = 1 << 23


def whole(text):
    """Return the value as an int when the literal is a whole number, else None."""
    try:
        f = float(text)
    except ValueError:
        return None
    return int(f) if f == int(f) and abs(f) < S24 else None


class Shader:
    """One shader, parsed far enough to find and rewrite its integer accumulators."""

    def __init__(self, text):
        self.lines = text.split("\n")
        self.shl = {}       # name -> (source, shift amount)
        self.ashr = {}      # name -> source
        self.cvt = {}       # name -> source
        self.fma = {}       # name -> (activation, weight, addend)
        self.add0 = {}      # name -> source
        self.phi = {}       # name -> [(value, block), ...]
        self.at = {}        # name -> line index
        self.scan()

    def scan(self):
        for i, line in enumerate(self.lines):
            m = RE_DEF.match(line)
            if m:
                self.at[m.group(1)] = i
            for regex, table, take in (
                (RE_SHL, self.shl, lambda g: (g[1], int(g[2]))),
                (RE_ASHR, self.ashr, lambda g: g[1]),
                (RE_CVT, self.cvt, lambda g: g[1]),
                (RE_FMA, self.fma, lambda g: (g[1], g[2], g[3])),
                (RE_ADD0, self.add0, lambda g: g[1]),
            ):
                m = regex.match(line)
                if m:
                    table[m.group(1)] = take(m.groups())
                    break
            else:
                m = RE_PHI.match(line)
                if m:
                    self.phi[m.group(1)] = RE_PHI_ARM.findall(m.group(2))

    def byte_of(self, name):
        """Give back (source register, byte index) when the value is a sign extended byte."""
        src = self.cvt.get(name)
        if src is None:
            return None
        src = self.ashr.get(src)
        if src is None:
            return None
        pair = self.shl.get(src)
        if pair is None:
            return None
        base, amount = pair
        if amount % 8 or amount > 24:
            return None
        return base, (24 - amount) // 8

    def accumulators(self):
        """Find every value that is an integer sum written as a float.

        The set starts empty and grows until it stops growing. A multiply-accumulate joins when its
        activation is a byte, its weight is a whole number, and its addend is a whole number or a
        member already. A zero add or a phi joins when everything feeding it is a member or a whole
        number. Anything that does not fit keeps the set out, so a shader the tool does not fully
        understand is left alone rather than half converted.
        """
        acc = set()
        while True:
            grew = False
            for name, (act, weight, addend) in self.fma.items():
                if name in acc:
                    continue
                if self.byte_of(act) is None or whole(weight) is None:
                    continue
                if not (addend.startswith("%") and addend[1:] in acc
                        or not addend.startswith("%") and whole(addend) is not None):
                    continue
                acc.add(name)
                grew = True
            for name, src in self.add0.items():
                if name not in acc and src in acc:
                    acc.add(name)
                    grew = True
            for name, arms in self.phi.items():
                if name in acc:
                    continue
                if all(v.startswith("%") and v[1:] in acc or
                       not v.startswith("%") and whole(v) is not None for v, _ in arms):
                    acc.add(name)
                    grew = True
            if not grew:
                return acc

    def escapes(self, acc):
        """Return the members that something outside the set reads, and so must stay floats."""
        inside = set()
        for name in acc:
            if name in self.fma:
                inside.add(self.fma[name][0])
            for reader in (self.fma, self.add0, self.phi):
                pass
        out = set()
        for i, line in enumerate(self.lines):
            m = RE_DEF.match(line)
            owner = m.group(1) if m else None
            body = line[m.end():] if m else line
            if owner in acc and (owner in self.fma or owner in self.add0 or owner in self.phi):
                continue        # a member reading members is the set talking to itself
            for used in RE_USE.findall(body):
                if used in acc:
                    out.add(used)
        return out


def rewrite(text):
    s = Shader(text)
    acc = s.accumulators()
    if not acc:
        return text, 0, 0
    escaping = s.escapes(acc)
    bytes_made = {}
    counter = [0]

    def byte_reg(base, index, sink):
        """Extract one signed byte, once per block, in the form NIR folds to extract_i8."""
        key = (base, index)
        if key in bytes_made:
            return bytes_made[key]
        counter[0] += 1
        name = f"%i24b{counter[0]}"
        sink.append(f"  {name} = call i32 @dx.op.tertiary.i32(i32 51, i32 8, i32 {index * 8}, "
                    f"i32 %{base})")
        bytes_made[key] = name
        return name

    def operand(text_value):
        """A member becomes its integer twin, a whole number literal stays a literal."""
        if text_value.startswith("%"):
            return f"%{text_value[1:]}.i24"
        return str(whole(text_value))

    out = []
    pending = []          # sitofp lines that have to wait until after the phis of a block
    for line in s.lines:
        if RE_BLOCK.match(line) or line.startswith("define") or line.strip().startswith("br "):
            out.extend(pending)
            pending = []
            # A value extracted in one block does not dominate readers in another, so each block
            # extracts the bytes it reads. NIR removes the copies that a block makes twice.
            bytes_made.clear()
        m = RE_DEF.match(line)
        name = m.group(1) if m else None
        if name not in acc:
            if pending and not RE_PHI.match(line):
                out.extend(pending)
                pending = []
            out.append(line)
            continue
        twin = f"%{name}.i24"
        sink = []
        if name in s.fma:
            act, weight, addend = s.fma[name]
            base, index = s.byte_of(act)
            b = byte_reg(base, index, sink)
            counter[0] += 1
            product = f"%i24m{counter[0]}"
            sink.append(f"  {product} = mul i32 {b}, {whole(weight)}")
            sink.append(f"  {twin} = add i32 {product}, {operand(addend)}")
        elif name in s.add0:
            sink.append(f"  {twin} = add i32 %{s.add0[name]}.i24, 0")
        else:
            arms = ", ".join(f"[ {operand(v)}, {b} ]" for v, b in s.phi[name])
            sink.append(f"  {twin} = phi i32 {arms}")
        if name in escaping:
            back = f"  %{name} = sitofp i32 {twin} to float"
            if name in s.phi:
                pending.append(back)       # phis must come first in their block
            else:
                sink.append(back)
        out.extend(sink)
    out.extend(pending)

    return "\n".join(out), len(acc), len(escaping)


def prune_declarations(text):
    """Drop any dx.op declaration the rewrite left with no caller, which the validator rejects."""
    lines = text.split("\n")
    keep = []
    for line in lines:
        m = re.match(r"declare .*@(dx\.op\.[A-Za-z0-9.]+)\(", line)
        if m and not any(f"call " in l and f"@{m.group(1)}(" in l for l in lines):
            continue
        keep.append(line)
    return "\n".join(keep)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    report_only = "--report" in sys.argv
    root = Path(args[0])
    totals = defaultdict(int)
    for path in sorted(root.glob("*.ll")):
        text = path.read_text()
        if "dx.op.tertiary.f32" not in text:
            continue
        new, count, escaped = rewrite(text)
        if not count:
            continue
        totals["shaders"] += 1
        totals["accumulators"] += count
        totals["escaping"] += escaped
        if not report_only:
            path.write_text(prune_declarations(new))
    print(f"lifted {totals['accumulators']} accumulators in {totals['shaders']} shaders, "
          f"{totals['escaping']} of them still read as floats")


if __name__ == "__main__":
    main()
