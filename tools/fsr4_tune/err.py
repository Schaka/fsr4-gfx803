"""Measure the output error of a pruned pass against the exact pass, on a captured tensor buffer.

Both shaders are translated to C++ and run over the real tensor from gameplay. The error is reported
in int8 units over the bytes the pass writes.

Usage: err.py exact.glsl tensor.bin bx by workdir pruned1.glsl [pruned2.glsl ...]
"""
import os
import re
import subprocess
import sys

exact, tensor, bx, by, wd = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
pruned = sys.argv[6:]
os.makedirs(wd, exist_ok=True)


def conv(path, fname):
    body = open(path).read().split("void main()", 1)[1]
    out = []
    for l in body.splitlines():
        s = l.strip()
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
    return f"static void {fname}(uint *T, uint GX, uint GY) {{\n" + "\n".join(out) + "\n}\n"


pre = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using uint = uint32_t;
static size_t g_n;                 // tensor size in dwords
static uint *g_sink;               // absorbs writes past the end of the buffer
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

main_tpl = r"""
static uint *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint *p = (uint *)malloc(sz + 4);
    fread(p, 1, sz, f); fclose(f);
    *n = sz / 4;
    return p;
}
int main(int argc, char **argv) {
    size_t n; uint *base = load("%TENSOR%", &n);
    g_n = n;
    uint sink; g_sink = &sink;
    uint *a = (uint *)malloc(n * 4), *b = (uint *)malloc(n * 4);
    const uint BX = %BX%, BY = %BY%;
    memcpy(a, base, n * 4);
    for (uint y = 0; y <= BY; y++) for (uint x = 0; x <= BX; x++) { Tensor t{a}; f_exact(t, x, y); }
    {
        double ss = 0; long c = 0;
        for (size_t i = 0; i < n; i++) {
            if (a[i] == base[i]) continue;
            for (int k = 0; k < 4; k++) { int v = (int8_t)(a[i] >> (8 * k)); ss += (double)v * v; c++; }
        }
        printf("exact output: bytes=%ld rms=%.3f\n", c, std::sqrt(ss / (c ? c : 1)));
    }
%RUNS%
    return 0;
}
"""

run_tpl = r"""    {
        memcpy(b, base, n * 4);
        for (uint y = 0; y <= BY; y++) for (uint x = 0; x <= BX; x++) { Tensor t{b}; %F%(t, x, y); }
        double se = 0; long cnt = 0, bad = 0; int mx = 0;
        for (size_t i = 0; i < n; i++) {
            if (a[i] == base[i] && b[i] == base[i]) continue;
            for (int k = 0; k < 4; k++) {
                int va = (int8_t)(a[i] >> (8 * k)), vb = (int8_t)(b[i] >> (8 * k));
                int d = va - vb; se += (double)d * d; cnt++;
                if (d) bad++;
                if (std::abs(d) > mx) mx = std::abs(d);
            }
        }
        printf("%NAME%: written_bytes=%ld rms=%.3f max=%d changed=%.1f%%\n", cnt, std::sqrt(se / (cnt ? cnt : 1)), mx, 100.0 * bad / (cnt ? cnt : 1));
    }
"""

src = pre + conv(exact, "f_exact")
runs = ""
for i, p in enumerate(pruned):
    src += conv(p, f"f_p{i}")
    runs += run_tpl.replace("%F%", f"f_p{i}").replace("%NAME%", os.path.basename(p))
src += main_tpl.replace("%TENSOR%", tensor).replace("%BX%", str(bx)).replace("%BY%", str(by)).replace("%RUNS%", runs)
src = src.replace("(uint *T, uint GX, uint GY)", "(Tensor T, uint GX, uint GY)")

cpp = os.path.join(wd, "err.cpp")
open(cpp, "w").write(src)
exe = os.path.join(wd, "err")
r = subprocess.run(["g++", "-std=c++17", "-O1", "-fwrapv", "-w", "-o", exe, cpp], capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:3000]); sys.exit(2)
sys.exit(subprocess.run([exe]).returncode)
