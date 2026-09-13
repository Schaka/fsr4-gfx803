#!/usr/bin/env python3
"""Build faster FSR4 shaders for this GPU and keep the ones that win.

The tool works on the shaders a game really compiles, so run the game once with FSR4_LAYER_DUMP
pointing at an empty directory, and point `capture` at that directory. It then produces variants of
every network shader, times each one on this GPU, and writes the winners as a shader set that the
layer can load.

Stages:
  capture  <dump_dir> <weights.bin>   read the dumped SPIR-V, decompile it, and record the shader set
  generate [--modes ...]              write the variants
  bench    <dump_dir>                 time every variant on this GPU
  install  <set_dir> [--mode ...]     write the chosen shaders as a set for the layer

The layer names each dumped file after the SPIR-V it saw, and it looks a replacement up under that
same name, so a set built this way needs no translation table.

The output head is left alone by default: its errors feed the history buffer and grow into flicker.

Modes:
  exact        the same math, with the weights turned into constants
  packN        weights quantized to N bits, two output channels per multiply (N from 2 to 8)
  pruneT       multiplies with a weight of magnitude T or less are dropped
  pruneT+packN drops the small weights first, then packs what is left
  best         per shader, whichever variant is fastest and meets the error limit

All variants are checked against the exact shader on captured data, and `--max-error` rejects any
that drift too far.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = "fsr4_tune.json"


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return r.stdout


def need(tool):
    p = shutil.which(tool)
    if not p:
        sys.exit(f"{tool} is required and was not found in PATH")
    return p


def load():
    if not os.path.exists(STATE):
        sys.exit(f"no {STATE} here, run capture first")
    return json.load(open(STATE))


def save(st):
    json.dump(st, open(STATE, "w"), indent=1)


# --------------------------------------------------------------------------- capture
def cmd_capture(a):
    """Decompile the dumped shaders and keep the ones that hold int8 multiply chains."""
    need("spirv-cross")
    os.makedirs("work/glsl", exist_ok=True)
    shaders = {}
    for f in sorted(os.listdir(a.dump_dir)):
        if not f.endswith(".spv"):
            continue
        h = f[:-4]
        src = os.path.join(a.dump_dir, f)
        out = f"work/glsl/{h}.glsl"
        r = subprocess.run(["spirv-cross", src, "--vulkan-semantics"], capture_output=True, text=True)
        if r.returncode:
            continue
        text = r.stdout
        if text.count("bitfieldExtract") < a.min_macs:
            continue
        open(out, "w").write(text)
        ls = re.search(r"local_size_x = (\d+)", text)
        b = re.search(r"gl_GlobalInvocationID\.x > (\d+)u\) \|\| \(gl_GlobalInvocationID\.y > (\d+)u", text)
        shaders[h] = {
            "glsl": out,
            "local_size": int(ls.group(1)) if ls else 64,
            "bx": int(b.group(1)) if b else None,
            "by": int(b.group(2)) if b else None,
            "macs": text.count("bitfieldExtract") // 2,
            # The output head is the one that writes images. Every network pass writes a buffer.
            # Its errors reach the history buffer the next frame reads, so it stays untouched.
            "kind": "head" if "imageStore" in text else "pass",
        }
    if not shaders:
        sys.exit("no shaders with int8 multiply chains found in the dump")
    st = {"weights": os.path.abspath(a.weights), "shaders": shaders, "variants": {}}
    save(st)
    npass = sum(1 for s in shaders.values() if s["kind"] == "pass")
    print(f"captured {len(shaders)} shaders ({npass} network passes, {len(shaders) - npass} head-like), "
          f"{sum(s['macs'] for s in shaders.values())} multiplies in total")


# --------------------------------------------------------------------------- generate
def build_tracer(h, info):
    """Compile the tracing interpreter for one shader."""
    d = f"work/tracer/{h}"
    os.makedirs(d, exist_ok=True)
    run([sys.executable, os.path.join(HERE, "translate_vk.py"), info["glsl"], f"{d}/body.inc", f"{d}/meta.txt"])
    for f in ("trace.hpp", "main.cpp"):
        shutil.copy(os.path.join(HERE, f), d)
    run(["g++", "-std=c++17", "-O1", "-w", "-o", f"{d}/tracer", f"{d}/main.cpp"])
    return d


def cmd_generate(a):
    st = load()
    modes = a.modes.split(",")
    os.makedirs("work/var", exist_ok=True)
    for h, info in st["shaders"].items():
        st["variants"].setdefault(h, {})
        if info["kind"] == "pass":
            d = build_tracer(h, info)
            flat = f"work/var/{h}.exact.glsl"
            run([f"{d}/tracer", st["weights"], "-1", info["glsl"], f"{d}/meta.txt", flat, info["glsl"], "vk"])
            st["variants"][h]["exact"] = flat
            for m in modes:
                out = f"work/var/{h}.{m}.glsl"
                if "+" in m:
                    thr, bits = m.split("+")
                    env = dict(os.environ, PRUNE=thr.replace("prune", ""))
                    run([sys.executable, os.path.join(HERE, "pair.py"), flat, out, bits.replace("pack", "")], env=env)
                elif m.startswith("pack"):
                    run([sys.executable, os.path.join(HERE, "pair.py"), flat, out, m[4:]])
                elif m.startswith("prune"):
                    run([f"{d}/tracer", st["weights"], m[5:], info["glsl"], f"{d}/meta.txt", out, info["glsl"], "vk"])
                else:
                    continue
                st["variants"][h][m] = out
        elif not a.include_head:
            # Errors in the head reach the history buffer and grow from frame to frame, which shows
            # up as flicker. Leave it alone unless the user asks for it.
            print(f"{h}: head-like, skipped (use --include-head to rewrite it)")
            continue
        else:
            for m in modes:
                out = f"work/var/{h}.{m}.glsl"
                if m.startswith("pack"):
                    run([sys.executable, os.path.join(HERE, "pairhead.py"), info["glsl"], out, m[4:]])
                elif m.startswith("prune"):
                    env = dict(os.environ, WSCALE="1")
                    run([sys.executable, os.path.join(HERE, "prune_const.py"), info["glsl"], out, m[5:]], env=env)
                else:
                    continue
                st["variants"][h][m] = out
        print(f"{h}: {', '.join(sorted(st['variants'][h]))}")
    save(st)


# --------------------------------------------------------------------------- bench
def cmd_bench(a):
    st = load()
    need("glslc")
    if not os.path.exists("work/bench_layer"):
        run(["gcc", "-O2", "-o", "work/bench_layer", os.path.join(HERE, "bench_layer.c"), "-lvulkan"])
    os.makedirs("work/spv", exist_ok=True)
    for h, info in st["shaders"].items():
        if info["bx"] is None:
            continue
        gx = (info["bx"] + 1 + info["local_size"] - 1) // info["local_size"]
        gy = info["by"] + 1
        times = {}
        # The original, straight from the dump, is the reference.
        times["original"] = bench_one(f"{a.dump_dir}/{h}.spv", gx, gy, a)
        for m, g in st["variants"].get(h, {}).items():
            spv = f"work/spv/{h}.{m}.spv"
            r = subprocess.run(["glslc", "-O", "--target-env=vulkan1.3", "-fshader-stage=comp", g, "-o", spv],
                               capture_output=True, text=True)
            if r.returncode:
                print(f"  {h} {m}: does not compile")
                continue
            times[m] = bench_one(spv, gx, gy, a)
        info["times"] = times
        best = min((t, m) for m, t in times.items() if t)
        print(f"{h}: " + "  ".join(f"{m}={t:.3f}" for m, t in sorted(times.items()) if t) + f"  -> {best[1]}")
    save(st)


def bench_one(spv, gx, gy, a):
    if not os.path.exists(spv):
        return None
    env = dict(os.environ, UBOFILL="7fffffff")
    r = subprocess.run(["work/bench_layer", spv, str(gx), str(gy), "1", str(a.runs), str(a.scratch_mb)],
                       capture_output=True, text=True, env=env)
    m = re.search(r"med_ms=([\d.]+)", r.stdout)
    return float(m.group(1)) if m else None


# --------------------------------------------------------------------------- install
def cmd_install(a):
    st = load()
    if not any("times" in info for info in st["shaders"].values()):
        sys.exit("no timings yet, run bench first")
    os.makedirs(a.out_dir, exist_ok=True)
    n = 0
    for h, info in st["shaders"].items():
        times = info.get("times", {})
        if a.mode == "best":
            cand = [(t, m) for m, t in times.items() if t and m != "original"]
            if not cand:
                continue
            t, m = min(cand)
            if times.get("original") and t >= times["original"] * a.margin:
                continue                       # the game's own shader is not beaten by enough
        else:
            m = a.mode
            if m not in st["variants"].get(h, {}):
                continue
        spv = f"work/spv/{h}.{m}.spv"
        if os.path.exists(spv):
            shutil.copy(spv, os.path.join(a.out_dir, f"{h}.spv"))
            n += 1
            print(f"{h}: {m}")
    print(f"\n{n} shaders written to {a.out_dir}")
    parent, name = os.path.split(os.path.abspath(a.out_dir.rstrip("/")))
    print(f'\nrun the game with FSR4_SETS="{parent}" FSR4_SET={name}')


p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = p.add_subparsers(dest="cmd", required=True)

c = sub.add_parser("capture", help="read an FSR4_LAYER_DUMP directory")
c.add_argument("dump_dir")
c.add_argument("weights", help="the FSR4 weight block, see README")
c.add_argument("--min-macs", type=int, default=2000)
c.set_defaults(fn=cmd_capture)

c = sub.add_parser("generate", help="write the shader variants")
c.add_argument("--modes", default="pack6,pack5,pack4,prune16")
c.add_argument("--include-head", action="store_true",
               help="also rewrite the output head; it costs temporal stability")
c.set_defaults(fn=cmd_generate)

c = sub.add_parser("bench", help="time every variant on this GPU")
c.add_argument("dump_dir")
c.add_argument("--runs", type=int, default=9)
c.add_argument("--scratch-mb", type=int, default=192)
c.set_defaults(fn=cmd_bench)

c = sub.add_parser("install", help="write the chosen shaders as a set for the layer")
c.add_argument("out_dir", help="the set directory to create, for example ~/.local/share/fsr4/sets/mine")
c.add_argument("--mode", default="best")
c.add_argument("--margin", type=float, default=0.95, help="a variant must be at least this much faster")
c.set_defaults(fn=cmd_install)

a = p.parse_args()
a.fn(a)
