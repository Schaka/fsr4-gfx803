#!/usr/bin/env python3
"""Put a tuned shader set into the Vulkan layer's cache.

The tuner names its shaders after the hash vkd3d-proton uses for the DXIL blob, while the layer sees
the SPIR-V that vkd3d produced from it. The bridge is the shader dump: for every replacement, this
hashes the dumped SPIR-V of the original and stores the replacement under that name.

Usage: install_layer.py <dump_dir> <set_dir> <cache_dir>

  dump_dir   a VKD3D_SHADER_DUMP_PATH directory, holding <dxil-hash>.spv
  set_dir    an override set, holding <dxil-hash>.spv replacements
  cache_dir  where the layer looks, usually ~/.cache/fsr4_opt/spirv
"""
import os
import shutil
import sys


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h * 0x100000001B3) ^ b) & 0xFFFFFFFFFFFFFFFF
    return h


dump, src, cache = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(cache, exist_ok=True)

done, missing = 0, []
for name in sorted(os.listdir(src)):
    if not name.endswith(".spv"):
        continue
    original = os.path.join(dump, name)
    if not os.path.exists(original):
        missing.append(name)
        continue
    h = fnv1a(open(original, "rb").read())
    shutil.copy(os.path.join(src, name), os.path.join(cache, f"{h:016x}.spv"))
    done += 1

print(f"installed {done} shaders into {cache}")
if missing:
    print(f"no dumped original for: {' '.join(n[:12] for n in missing)}")
    print("run the game once with VKD3D_SHADER_DUMP_PATH set and no override active")
