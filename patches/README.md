# Patches

| patch | applies to | what it does |
|---|---|---|
| `mesa-26.2.2-nir-imul24-int8.patch` | Mesa 26.2.2, commit `0ae52750` | The Mesa patch. Use this one. |
| `mesa-nir-imul24-int8.patch` | Mesa 26.1.6, commit `ffa422e53d` | The same rules against 26.1.6. It built `../radv/mesa-26.1.6-patched/`. |
| `dxil-spirv-fsr4-int8.patch` | dxil-spirv `7ecda135de74` | Adds `FSR4_DOT_MODE` and the INT8 cooperative-matrix formats. |
| `vkd3d-proton-fsr4.patch` | vkd3d-proton `3dfc6f07d095` | Diagnostic hooks only. See `../vkd3d-proton/README.md`. |

The two vkd3d-proton-side patches built `../vkd3d-proton/`. That README has the build steps.

---

## The Mesa patch

It adds NIR algebraic rules that turn int8 multiplies into the full-rate 24-bit multiply-add
`v_mad_i32_i24`. That costs 1.0 VALU per multiply-accumulate, the lowest cost possible on hardware
without `dp4a`.

It changes two files:

* `src/compiler/nir/nir_opt_algebraic.py` gets four rules: `extract_i8 × extract_i8 → imul24_relaxed`,
  `extract_u8 × extract_u8 → umul24_relaxed`, and the forms where NIR already folded the weight to a
  constant, `extract_i8 × #s24const → imul24_relaxed` and `extract_u8 × #u24const → umul24_relaxed`.
  Several mid-sized shaders store their weights as constants, so the constant forms are required.
* `src/compiler/nir/nir_search_helpers.h` gets the `is_s24` and `is_u24` range checks for those
  constants.

The two Mesa patches contain the same rules. The 26.1.6 patch also applies to 26.2.2, but with line
offsets. The 26.2.2 patch applies with none.

The result is bit-exact. `v_mad_i32_i24` sign-extends 24-bit operands. An int8 value or a constant
in signed 24-bit range passes through unchanged, and the product of an 8-bit and a 24-bit value fits
in 32 bits. `../data/imul24/README.md` has the full argument and the ISA counts.

### Why upstream Mesa 26.2 does not replace it

Upstream merge request 41178, merged 2026-05-01 and first shipped in 26.2, applies the same 24-bit
idea to NIR's software `sdot_4x8` expansion. That expansion runs only for shaders that contain
`OpSDot`. These shaders contain none. In `i32` mode, dxil-spirv emits the decomposition itself, so
NIR never builds the node that the upstream rule matches. This patch matches the expanded form.

Pragmata measurements, upscaler on every frame, 1280x720 to 1920x1080:

| driver | mean ms | fps |
|---|---:|---:|
| 26.1.6 + patch | 30.27 | 33.0 |
| 26.2.2 + patch | 30.38 | 32.9 |
| 26.2.2 stock | 52.93 | 18.9 |

The logs are in `../evidence/mesa-26.2.2-vs-our-patch/`.

### Build

```bash
meson setup build -Dvulkan-drivers=amd -Dgallium-drivers= \
  -Dplatforms=wayland,x11 -Dllvm=disabled -Dvideo-codecs= \
  -Dbuildtype=release -Db_ndebug=true
ninja -C build
```

The build needs the Python module `mako`.
