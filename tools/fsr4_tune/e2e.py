"""End-to-end error of a candidate override set, measured through the whole pass chain.

The per-pass metric in err.py scores each pass on its own, so it cannot see error that grows as it
travels down the chain. This runs all twelve passes in order on the captured tensor, once with the
exact shaders and once with the candidate set, and compares the tensor at the end. That is the error
that reaches the output head.

Both runs read the same captured tensor, so the inputs of pass1 are identical. From pass2 on, the
candidate run reads what the candidate produced, which is the point of the measurement.

Usage: e2e.py tensor.bin workdir candidate_dir [candidate_dir ...]

A candidate directory holds one .glsl per shader hash, named as in pass_hashes.txt. A pass with no
file in the directory runs its exact shader, so partial sets work.

It does not model time. Frame N of the network reads its own output from frame N-1 through the
recurrent tensor, and this harness sees one frame only, so flicker stays out of reach.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
tensor, wd, cands = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(wd, exist_ok=True)

order = []
for line in open(os.path.join(HERE, "pass_hashes.txt")):
    h, p = line.split()
    n = int(p[4:])
    meta = open(os.path.join(HERE, f"p{n}", "meta.txt")).read().split()
    order.append((n, p, h, int(meta[2]), int(meta[3])))
order.sort()


def conv(path, fname):
    """Translate one flat shader into a C++ function over the tensor."""
    body = open(path).read().split("void main()", 1)[1]
    out = []
    for line in body.splitlines():
        s = line.strip()
        if not s or s in "{}" or s.startswith(("uint GX", "uint GY", "if (GX", "if (GY")):
            continue
        s = re.sub(r"_\d+\._m0\[\d+u\]\.[xyzw]", "0.0f", s)
        s = re.sub(r"PhysicalPointer\w+\(registers\._m\d+\)\.value\[\d+u\]\.[xyzw]", "0u", s)
        s = re.sub(r"_\d+\[registers\._m\d+ \+ 11u\]\._m0\[(uint\(int\(.*?\)\))\]", r"T[\1]", s)
        s = re.sub(r"imageLoad\(_11, (int\(.*?\))\)\.x", r"T[uint(\1)]", s)
        m = re.match(r"imageStore\(_11, (int\(.*\)), uvec4\((.*)\)\);", s)
        if m:
            s = f"T[uint({m.group(1)})] = {m.group(2)};"
        out.append("    " + s)
    return f"static void {fname}(Tensor T, uint GX, uint GY) {{\n" + "\n".join(out) + "\n}\n"


PRE = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using uint = uint32_t;
static size_t g_n;
static uint *g_sink;
struct Tensor {
    uint *p;
    uint &operator[](size_t i) { return i < g_n ? p[i] : *g_sink; }
};
static inline int bitfieldExtract(int v, int o, int b) { return (int)((uint)v << (32 - o - b)) >> (32 - b); }
template <class T> static inline T max(T a, T b) { return a > b ? a : b; }
template <class T> static inline T min(T a, T b) { return a < b ? a : b; }
static inline float roundEven(float f) { return std::nearbyint(f); }
struct u8vec4 { uint8_t x, y, z, w; u8vec4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : x(a), y(b), z(c), w(d) {} };
static inline uint pack32(u8vec4 v) { return v.x | (v.y << 8) | (v.z << 16) | ((uint)v.w << 24); }
"""

src = PRE
runs = []
for n, p, h, bx, by in order:
    src += conv(os.path.join(HERE, f"std_real_{p}.glsl"), f"exact_{n}")
for ci, d in enumerate(cands):
    for n, p, h, bx, by in order:
        f = os.path.join(d, f"{h}.glsl")
        src += conv(f, f"cand{ci}_{n}") if os.path.exists(f) else f"static void cand{ci}_{n}(Tensor T, uint GX, uint GY) {{ exact_{n}(T, GX, GY); }}\n"

chain = lambda pre: "\n".join(
    f"        for (uint y = 0; y <= {by}u; y++) for (uint x = 0; x <= {bx}u; x++) {{ Tensor t{{buf}}; {pre}_{n}(t, x, y); }}"
    for n, p, h, bx, by in order)

src += r"""
static uint *g_base;
static void run_chain(uint *buf, int which);
int main() {
    FILE *f = fopen("%TENSOR%", "rb");
    if (!f) { perror("tensor"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    g_base = (uint *)malloc(sz); fread(g_base, 1, sz, f); fclose(f);
    g_n = sz / 4;
    uint sink; g_sink = &sink;
    uint *a = (uint *)malloc(sz), *b = (uint *)malloc(sz);
    memcpy(a, g_base, sz);
    run_chain(a, -1);
    double ss = 0; long c = 0;
    for (size_t i = 0; i < g_n; i++) if (a[i] != g_base[i])
        for (int k = 0; k < 4; k++) { int v = (int8_t)(a[i] >> (8 * k)); ss += (double)v * v; c++; }
    double sig = std::sqrt(ss / (c ? c : 1));
    printf("chain output: bytes=%ld rms=%.3f\n", c, sig);
    for (int w = 0; w < %NCAND%; w++) {
        memcpy(b, g_base, sz);
        run_chain(b, w);
        double se = 0; long n2 = 0; int mx = 0;
        for (size_t i = 0; i < g_n; i++) {
            if (a[i] == g_base[i] && b[i] == g_base[i]) continue;
            for (int k = 0; k < 4; k++) {
                int va = (int8_t)(a[i] >> (8 * k)), vb = (int8_t)(b[i] >> (8 * k));
                int d = va - vb; se += (double)d * d; n2++;
                if (std::abs(d) > mx) mx = std::abs(d);
            }
        }
        double rms = std::sqrt(se / (n2 ? n2 : 1));
        printf("%s: rms=%.3f relative=%.1f%% max=%d\n", g_names[w], rms, 100.0 * rms / sig, mx);
    }
    return 0;
}
"""
src = src.replace("%TENSOR%", os.path.abspath(tensor)).replace("%NCAND%", str(len(cands)))
src += "static const char *g_names[] = {" + ", ".join(f'"{os.path.basename(d.rstrip("/"))}"' for d in cands) + "};\n"
src += "static void run_chain(uint *buf, int which) {\n    if (which < 0) {\n" + chain("exact") + "\n    }\n"
for ci in range(len(cands)):
    src += f"    else if (which == {ci}) {{\n" + chain(f"cand{ci}") + "\n    }\n"
src += "}\n"

cpp = os.path.join(wd, "e2e.cpp")
open(cpp, "w").write(src)
exe = os.path.join(wd, "e2e")
r = subprocess.run(["g++", "-std=c++17", "-O1", "-fwrapv", "-w", "-o", exe, cpp], capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:3000]); sys.exit(2)
print(f"built {exe}; run it to measure. Passes in order: {[n for n, *_ in order]}")
