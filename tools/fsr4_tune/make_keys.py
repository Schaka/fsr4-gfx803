#!/usr/bin/env python3
"""Teach the shipped shader sets about another vkd3d-proton build.

A shipped set names each shader after the hash of the DXIL blob it came from. That hash is a property
of the FSR4 DLL, so it is the same everywhere. The layer, on the other hand, only ever sees the
SPIR-V that vkd3d-proton produced from that blob, and a different vkd3d build produces different
SPIR-V, so it hashes to something else. sets/keys.txt carries one line per build to bridge the two:

    <hash of the SPIR-V the layer sees>  <name of the file in the set>

You only need this when the shipped sets replace nothing on a Proton build we never measured. Run the
game once with

    VKD3D_SHADER_DUMP_PATH=/tmp/vkdump

which every vkd3d-proton supports without a patch, and which writes <dxil-hash>.spv. This reads that
directory, hashes the contents the same way the layer does, and appends the missing lines.

Usage: make_keys.py <dump_dir> <sets_dir>

  dump_dir   a VKD3D_SHADER_DUMP_PATH directory holding <dxil-hash>.spv
  sets_dir   the sets directory, the one holding keys.txt

A set you built yourself with tune.py needs none of this: tune.py names its output after the SPIR-V
the layer saw, which is what the layer looks for when keys.txt has no line for it.
"""
import os
import sys


def fnv1a(data):
    """The same 64-bit FNV-1a the layer computes over the SPIR-V it is handed."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h * 0x100000001B3) ^ b) & 0xFFFFFFFFFFFFFFFF
    return h


if len(sys.argv) != 3:
    sys.exit(__doc__)
dump, sets = sys.argv[1], sys.argv[2]

keys = os.path.join(sets, "keys.txt")
if not os.path.isdir(sets):
    sys.exit(f"{sets} is not a directory")

# Every name that any set actually ships. A dumped shader we have no replacement for is not
# interesting, and writing a line for it would only make keys.txt harder to read.
shipped = set()
for entry in os.listdir(sets):
    d = os.path.join(sets, entry)
    if os.path.isdir(d):
        shipped.update(f[:-4] for f in os.listdir(d) if f.endswith(".spv"))

known = set()
if os.path.exists(keys):
    for line in open(keys):
        parts = line.split()
        if len(parts) == 2:
            known.add(parts[0])

added = []
for f in sorted(os.listdir(dump)):
    if not f.endswith(".spv"):
        continue
    dxil = f[:-4]
    if dxil not in shipped:
        continue
    h = f"{fnv1a(open(os.path.join(dump, f), 'rb').read()):016x}"
    if h in known:
        continue
    added.append(f"{h} {dxil}")
    known.add(h)

if not added:
    print("nothing to add, this build is already covered")
    sys.exit(0)

lines = []
if os.path.exists(keys):
    lines = [l.rstrip("\n") for l in open(keys) if l.strip()]
lines.extend(added)
open(keys, "w").write("\n".join(sorted(lines)) + "\n")
print(f"added {len(added)} lines to {keys}")
