#!/usr/bin/env python3
"""Remove the wave size requirement from the BC-250 fork's shaders, so GCN4 can run them.

Those shaders declare that they need a wave of 32 lanes. GCN4 has one wave size, 64, so
vkd3d-proton refuses the compute pipeline outright:

    d3d12_device_validate_shader_meta: Required WaveSize range [32, 32], but supported range is [64, 64]
    d3d12_command_list_Dispatch: Failed to update compute state, ignoring dispatch

Nothing announces that. The DLL loads, the model selection hook binds, the context is created and
dispatches begin, so the only visible sign is that the network runs four passes per frame instead of
about twenty six, and the picture is a wrongly placed copy of part of the frame.

The requirement is dropped rather than changed, because GCN4 cannot satisfy any range that excludes
64. The shaders read the lane count at run time where they need it, so they work at either size.

DXIL writes the requirement two ways and both have to go: tag 23 carries a range, tag 11 a fixed
size. Missing the second one leaves 14 shaders still asking for 32 lanes, which is enough to keep the
network broken.

Usage: wave64_fix.py <shader dir>
"""
import re
import sys
from pathlib import Path


def main():
    d = Path(sys.argv[1])
    patched = 0
    for f in sorted(d.glob("*.ll")):
        t = f.read_text()
        m = re.search(r"^(![0-9]+) = !\{i32 0, i64 [0-9]+(.*)\}$", t, re.M)
        if not m:
            raise SystemExit("no entry point properties in " + f.name)
        props = m.group(2)
        stripped = re.sub(r", i32 (?:11|23), !\d+", "", props)
        if stripped == props:
            continue
        head = re.search(r"i64 (\d+)", m.group(0)).group(1)
        t = t[:m.start()] + f"{m.group(1)} = !{{i32 0, i64 {head}{stripped}}}" + t[m.end():]
        f.write_text(t)
        patched += 1
    print(f"dropped the wave size requirement from {patched} shaders")


if __name__ == "__main__":
    main()
