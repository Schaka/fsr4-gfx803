#!/usr/bin/env python3
"""Rebuild the DLL from modified shader sources.

build.py refuses any change, because it exists to prove the published DLL reproduces. This does the
same work and takes the sources as they are: assemble each .ll with the pinned DXC, record what came
out, and repack. Every structural check inside repack_dll stays in force.

  build_variant.py --sdk <pinned SDK dll> --dxcompiler <libdxcompiler.so> --output <new dir>
"""
import argparse
import os
import hashlib
import json
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from repack_dll import build

ROOT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sdk", type=Path, required=True)
    p.add_argument("--dxcompiler", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--jobs", type=int, default=8)
    a = p.parse_args()

    manifest = json.loads((ROOT / "manifest.json").read_text())
    skip = [x for x in (os.environ.get("SKIP_ENTRIES") or "").split(",") if x]
    if skip:
        before = len(manifest["replacements"])
        manifest["replacements"] = [r for r in manifest["replacements"]
                                    if not any(r["entry"].endswith(k) for k in skip)]
        print(f"keeping the SDK's own shader for {before - len(manifest['replacements'])} entries: "
              f"{', '.join(skip)}")
    sdk, dxc, out = a.sdk.resolve(), a.dxcompiler.resolve(), a.output.resolve()
    if out.exists():
        raise FileExistsError(out)
    out.mkdir(parents=True)
    (out / "shaders").mkdir()
    subprocess.run(["g++", "-std=c++17", "-O2", "-w", str(ROOT / "assemble.cpp"), "-ldl",
                    "-o", str(out / "assemble")], check=True)

    def compile_one(row):
        row = dict(row)
        src = ROOT / row["source"]
        raw = out / "shaders" / f"{row['original_offset']:08x}.raw.dxil"
        dxil = raw.with_name(raw.name.replace(".raw.dxil", ".dxil"))
        for action, first, last in [("assemble", src, raw), ("validate", raw, dxil)]:
            q = subprocess.run([str(out / "assemble"), str(dxc), action, str(first), str(last)],
                               capture_output=True, text=True, timeout=120)
            if q.returncode:
                raise RuntimeError(f"{row['source']} {action}: {q.stderr.strip()[:400]}")
        raw.unlink()
        row["replacement"] = str(dxil)
        row["replacement_sha256"] = sha(dxil)          # this build, not the published one
        return row

    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        rows = list(pool.map(compile_one, manifest["replacements"]))
    dll = out / "amd_fidelityfx_upscaler_dx12.dll"
    record = build(sdk, dict(sdk_sha256=manifest["sdk_sha256"], replacements=rows), dll)
    print(json.dumps(dict(shaders=len(rows), dll=str(dll), sha256=record["sha256"],
                          bytes=record["bytes"]), indent=2))


if __name__ == "__main__":
    main()
