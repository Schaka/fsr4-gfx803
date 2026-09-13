"""Translate the main() of a dxil-spirv GLSL compute shader into C++ for trace.hpp.
Usage: translate.py in.glsl out_body.inc out_meta.txt"""
import re
import sys

src = open(sys.argv[1]).read()
names = {}
for b, kind, nm in re.findall(r"layout\(set = 0, binding = (\d+)(?:, [a-z0-9]+)?\) uniform (\w+) (_\d+)", src):
    names.setdefault((int(b), kind), nm)
t_name = next(nm for (b, k), nm in names.items() if b == 11 and k == "uimageBuffer")
w_name = next((nm for (b, k), nm in names.items() if b == 18 and k == "usamplerBuffer"), None)
u_name = None
m = re.search(r"layout\(set = 0, binding = 0, std140\) uniform \w+\s*\{[^}]*\}\s*(_\d+);", src)
if m:
    u_name = m.group(1)
ls = re.search(r"local_size_x = (\d+), local_size_y = (\d+)", src)
bounds = re.search(r"gl_GlobalInvocationID\.x > (\d+)u\) \|\| \(gl_GlobalInvocationID\.y > (\d+)u", src)

body = src[src.index("void main()"):]
body = body.replace("void main()", "void shader_main()", 1)
body = re.sub(r"\b" + t_name + r"\b", "T_", body)
if w_name:
    body = re.sub(r"\b" + w_name + r"\b", "W_", body)
if u_name:
    body = re.sub(r"\b" + u_name + r"\b", "U_", body)
types = "uint int bool float uvec2 uvec3 uvec4 ivec2 ivec4 u8vec4 i8vec4 int16_t uint16_t int8_t uint8_t float16_t f16vec2 f16vec4 vec2 vec4 i16vec4 u16vec4".split()
body = re.sub(r"\b(" + "|".join(types) + r")\b", r"G_\1", body)


def wrap(s):
    out, i = [], 0
    while i < len(s):
        ch = s[i]
        if ch == "[" and i > 0 and (s[i - 1].isalnum() or s[i - 1] in "_])"):
            depth, j = 1, i + 1
            while depth:
                depth += {"[": 1, "]": -1}.get(s[j], 0)
                j += 1
            inner = wrap(s[i + 1:j - 1])
            out.append("[" + inner + "]" if re.fullmatch(r"\d+", inner) else "[IX(" + inner + ")]")
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


body = wrap(body)
open(sys.argv[2], "w").write(body)
open(sys.argv[3], "w").write(f"{t_name} {int(ls.group(1))} {int(bounds.group(1))} {int(bounds.group(2))} {u_name or '-'}\n")
print("tensor", t_name, "weights", w_name, "ubo", u_name, "local", ls.groups(), "bounds", bounds.groups())
