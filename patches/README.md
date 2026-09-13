# Patches

| patch | applies to | what it does |
|---|---|---|
| `mesa-26.2.2-nir-imul24-int8.patch` | Mesa 26.2.2, commit `0ae52750` | The Mesa patch. Use this one. |
| `mesa-nir-imul24-int8.patch` | Mesa 26.1.6, commit `ffa422e53d` | The same rules against 26.1.6. It built `../radv/mesa-26.1.6-patched/`. |

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
idea to NIR's software `sdot_4x8` expansion. That expansion runs on the `OpSDot` the shader carries.
The Vulkan layer rewrites those dot products into plain 32-bit multiplies before the driver sees the
module, and this patch is what matches that form. The layer's rewrite is the faster of the two: whole
frames on an RX 570 in Pragmata, patched driver in both runs and no shaders replaced, are 25.99 ms
through Mesa's own lowering and 23.50 ms through the layer's.

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
