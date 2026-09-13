"""Translate a vkd3d-proton SPIR-V compute shader (spirv-cross --vulkan-semantics output) into C++ for trace.hpp.

Tensor (register 11) and weight (register 18) accesses go through descriptor arrays of SSBOs, indexed by
push constants. They become TL/TL4/TS and WF/WF4 calls. Uniform reads through the
PhysicalPointer...CBVArray buffer reference become CBV objects.

Usage: translate_vk.py in.glsl out_body.inc out_meta.txt
meta line 1: "<tensor name for std form> <local_size_x> <bound x> <bound y> -"
meta line 2: "vk <uint tensor array> <tensor register expression>"
"""
import re
import sys

src = open(sys.argv[1]).read()
head_end = src.index("void main()")
head, body = src[:head_end], src[head_end:]

arrays = {}
for ro, ty, nm in re.findall(r"layout\(set = \d+, binding = \d+, std430\) (restrict readonly |readonly |restrict )?buffer \w+\s*\{\s*(uint|uvec4) _m0\[\];\s*\}\s*(_\d+)\[\];", head):
    arrays[nm] = (ty, "readonly" in ro)
regdefs = dict(re.findall(r"uint (_\d+) = (registers\._m\d+ \+ \d+u);", body))

ls = re.search(r"local_size_x = (\d+), local_size_y = (\d+)", head)
bounds = re.search(r"gl_GlobalInvocationID\.x > (\d+)u\) \|\| \(gl_GlobalInvocationID\.y > (\d+)u", body)


def match_bracket(s, i):
    depth, j = 1, i + 1
    while depth:
        depth += {"[": 1, "]": -1}.get(s[j], 0)
        j += 1
    return j


tensor_reg = None
out, i = [], 0
pat = re.compile(r"(_\d+)\[")
while i < len(body):
    m = pat.match(body, i)
    if m and m.group(1) in arrays and (i == 0 or not (body[i - 1].isalnum() or body[i - 1] == "_")):
        name = m.group(1)
        j = match_bracket(body, m.end() - 1)
        reg = body[m.end():j - 1].strip()
        reg = regdefs.get(reg, reg)
        if not body.startswith("._m0[", j):
            out.append(body[i:j]); i = j; continue
        k = match_bracket(body, j + 4)
        idx = body[j + 5:k - 1]
        ty, ro = arrays[name]
        if reg.endswith("+ 18u"):
            kind = "W"
        elif reg.endswith("+ 11u"):
            kind = "T"
            if tensor_reg is None:
                tensor_reg = reg
            elif tensor_reg != reg:
                sys.exit(f"two tensor registers: {tensor_reg} / {reg}")
        else:
            sys.exit(f"unknown register expression {reg!r}")
        rest = body[k:]
        am = re.match(r"\s*=\s*", rest)
        if am and not rest.lstrip().startswith("=="):
            if kind != "T" or ty != "uint":
                sys.exit("write to non-tensor or uvec4 array")
            end = body.index(";", k)
            out.append(f"TS({idx}, {body[k + am.end():end]})")
            i = end
        else:
            fn = ("TL" if kind == "T" else "WF") + ("4" if ty == "uvec4" else "")
            out.append(f"{fn}({idx})")
            i = k
    else:
        out.append(body[i]); i += 1
body = "".join(out)

body = re.sub(r"PhysicalPointer(\w+)CBVArray (_\d+) = PhysicalPointer\w+CBVArray\((registers\._m\d+)\);",
              lambda m: f'CBV {m.group(2)}("PhysicalPointer{m.group(1)}CBVArray({m.group(3)})");', body)
body = body.replace("void main()", "void shader_main()", 1)
types = "uint int bool float uvec2 uvec3 uvec4 ivec2 ivec4 u8vec4 i8vec4 int16_t uint16_t int8_t uint8_t float16_t f16vec2 f16vec4 vec2 vec4 i16vec4 u16vec4".split()
body = re.sub(r"\b(" + "|".join(types) + r")\b", r"G_\1", body)


def wrap(s):
    res, i = [], 0
    while i < len(s):
        ch = s[i]
        if ch == "[" and i > 0 and (s[i - 1].isalnum() or s[i - 1] in "_])"):
            j = match_bracket(s, i)
            inner = wrap(s[i + 1:j - 1])
            res.append("[" + inner + "]" if re.fullmatch(r"\d+", inner) else "[IX(" + inner + ")]")
            i = j
        else:
            res.append(ch); i += 1
    return "".join(res)


body = wrap(body)
uint_tensor = next((nm for nm, (ty, ro) in arrays.items() if ty == "uint" and not ro), None)
open(sys.argv[2], "w").write(body)
open(sys.argv[3], "w").write(f"_11 {ls.group(1)} {bounds.group(1)} {bounds.group(2)} -\nvk {uint_tensor} {tensor_reg}\n")
print("arrays", arrays, "tensor", uint_tensor, tensor_reg, "local", ls.groups(), "bounds", bounds.groups())
