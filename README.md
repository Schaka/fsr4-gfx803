# FSR4 INT8 on Polaris (gfx803)

RADV patches that make AMD's FSR4 INT8 upscaler run faster on GCN4 hardware, such as the RX 470,
RX 480, RX 570 and RX 580.

GCN4 has no hardware `dp4a` instruction, so every int8 multiply-accumulate in FSR4's neural network
is emulated in software. Stock Mesa emulates it with full 32-bit multiplies. These patches emit
`v_mad_i32_i24` instead, a full-rate 24-bit multiply-add that GCN has had since its first
generation. The result is exact for 8-bit operands. In Pragmata it made frames 2.28 times faster
than stock Mesa with FSR 4.1.1, and 1.75 times faster with FSR 4.0.2.

This is not a driver you install system-wide. It is a second copy of RADV that a single game opts
into through one environment variable.

---

## What you need

You need two patches at two different layers. One alone does nothing useful.

| layer | what | where |
|---|---|---|
| Mesa / RADV | NIR rules that emit `v_mad_i32_i24` | `patches/mesa-26.2.2-nir-imul24-int8.patch` |
| vkd3d-proton | decomposes FSR4's int8 dot product, and adds the `FSR4_DOT_MODE` switch | `vkd3d-proton/`, source in `patches/dxil-spirv-fsr4-int8.patch` and `patches/vkd3d-proton-fsr4.patch` |

Use `patches/mesa-26.2.2-nir-imul24-int8.patch` for Mesa 26.2.2. `patches/mesa-nir-imul24-int8.patch`
is the original against 26.1.6; it also applies to 26.2.2, but with line offsets.

Prebuilt copies of everything, with checksums, are listed in `CHECKSUMS.md`.

---

## Prebuilt binaries

If you would rather not build anything, the
[latest release](https://github.com/Schaka/fsr4-gfx803/releases/latest) has the patched RADV and
the patched vkd3d-proton attached, with an `install.sh`. Steps 2, 3 and 4 below still apply.

---

## 1. Build the driver

Build this on a reasonably fast machine. It takes a while on an older CPU.

```bash
git clone https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa
git fetch --depth=1 origin 26.2
git checkout -b fsr4 FETCH_HEAD
git apply /path/to/patches/mesa-26.2.2-nir-imul24-int8.patch

meson setup build \
  -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=wayland,x11 \
  -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release -Db_ndebug=true
ninja -C build
```

`-Dllvm=disabled` is deliberate. RADV uses its own compiler, ACO, so the library then links no LLVM
at all and will run on a machine whose LLVM version differs from the build machine's. Check it:

```bash
ldd build/src/amd/vulkan/libvulkan_radeon.so | grep -i llvm    # must print nothing
```

Install it somewhere of its own. Do not overwrite the system Mesa.

```bash
mkdir -p ~/.local/share/radv-fsr4
cp build/src/amd/vulkan/libvulkan_radeon.so ~/.local/share/radv-fsr4/
sed "s|\"library_path\":.*|\"library_path\": \"$HOME/.local/share/radv-fsr4/libvulkan_radeon.so\",|" \
  build/src/amd/vulkan/radeon_icd.x86_64.json > ~/.local/share/radv-fsr4/radeon_icd.x86_64.json
```

Nothing uses this driver unless `VK_DRIVER_FILES` points at it.

---

## 2. Enable fp16

FSR4's shaders need the Vulkan feature `shaderFloat16`. RADV turns it off by default on GFX8,
because GCN4 has no double-rate fp16. Turn it on by copying `drirc.d/99-fsr4-gfx803.conf` to
`~/.drirc`.

Check it:

```bash
vulkaninfo | grep shaderFloat16      # must say true, with no variables set
```

If this is wrong, FSR4's compute pipelines fail to build and the upscaler silently does nothing.

---

## 3. Install the patched vkd3d-proton

Copy `vkd3d-proton/d3d12core.dll` and `vkd3d-proton/d3d12.dll` into **both** your Proton install and
your game's Wine prefix:

* `<proton>/files/lib/wine/vkd3d-proton/x86_64-windows/`
* `<prefix>/drive_c/windows/system32/`

Both are required. Proton re-syncs the prefix from its own install on every launch, so patching only
the prefix reverts silently. The install copy is read-only, so `chmod u+w` it first. Verify with
`md5sum` afterwards.

---

## 4. Set up OptiScaler

Install OptiScaler 0.9.4 into the game directory, with an FSR4 4.1.1 upscaler DLL. The stock 4.1.1
DLL and the 4.1.1b INT8 build both work, and both are in `fsr4_dlls/`.

In `OptiScaler.ini`:

```
Dx12Upscaler=fsr31
UpscalerIndex=0
Fsr4Update=true
Fsr4ForceEnableInt8=true
```

---

## 5. Launch

```bash
VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
FSR4_DOT_MODE=i32 \
  %command%
```

`FSR4_DOT_MODE=i32` is what routes the shader through the patched path. Without it the Mesa patch
does nothing.

If your machine has more than one AMD GPU, add `MESA_VK_DEVICE_SELECT=<vendor>:<device>` so the
right one is used. Find the IDs with `lspci -nn | grep VGA`.


---

## `FSR4_DOT_MODE`

FSR4 drives every multiply-accumulate through DXIL's `dot4add_i8packed`. GCN4 has no hardware
`dp4a`, so the patched vkd3d-proton has to decompose it, and this variable picks how.

| value | what it emits | cost | use it? |
|---|---|---|---|
| `i32` | 32-bit extract, multiply, add. NIR proves the operands fit in 24 bits and ACO fuses them into `v_mad_i32_i24` | **1 VALU per MAC** | **Yes. This is the one to use, with the patch.** |
| `mad16` | 16-bit multiply-add path | 2 VALU per MAC | The built-in default. Use only without the patch. |
| `i16` | `v_mul_lo_u16` plus `v_add_u32_sdwa` | 2 VALU per MAC | No |
| `vec16` | vectorised 16-bit variant | about 2 VALU per MAC | No |
| `fp32` | converts to float, `FMul` and `FAdd` | 2 VALU per MAC, plus conversions | No |
| `dot` | native SPIR-V `OpSDotKHR`, lowered by Mesa | slowest measured | No, but see below |

`i32` is the only mode the Mesa patch affects. In every other mode the patch does nothing, because
the shader never produces the pattern it matches.

`dot` is the interesting one. It emits `OpSDotKHR` and lets Mesa lower it, which is the path
upstream merge request 41178 optimises. On stock Mesa 26.2.2 that reaches within about 7 percent of
`i32` with the patch, which is how we know the upstream work is sound. It is still slower, and on
Mesa 26.1.6 or earlier it is much slower, because that lowering used plain 32-bit multiplies.

If you set nothing, you get `mad16`, and the Mesa patch is wasted.

---

## Did it work?

Check OptiScaler's log. A correct run shows `FSR31FeatureDx12` together with `FSR4ModelSelection`.
Turn on `Fsr4EnableWatermark=true` in `OptiScaler.ini` to see the FSR4 version on screen.

If you see `FSR2FeatureDx12_212`, FSR4 is not running at all. OptiScaler has quietly fallen back to
its own built-in FSR 2.1.2 copy, and any frametime you measure is meaningless.

---

## Results

Measured on an RX 470 in Pragmata, real gameplay, upscaler running on every frame, one identical
scene, 1280x720 upscaled to 1920x1080, `FSR4_DOT_MODE=i32`.

| FSR4 DLL | driver | mean ms | fps |
|---|---|---:|---:|
| 4.1.1 stock | Mesa 26.2.2 + this patch | 27.32 | 36.6 |
| 4.1.1b INT8 | Mesa 26.2.2 + this patch | 27.78 | 36.0 |
| 4.1.1b INT8 | Mesa 26.2.2 stock | 63.35 | 15.8 |
| 4.0.2 INT8 | Mesa 26.1.6 + this patch | 30.27 | 33.0 |
| 4.0.2 INT8 | Mesa 26.2.2 + this patch | 30.38 | 32.9 |
| 4.0.2 INT8 | Mesa 26.2.2 stock | 52.93 | 18.9 |

* Use FSR 4.1.1 with the patch. It is 2.6 ms per frame faster than 4.0.2. The stock 4.1.1 DLL and
  the 4.1.1b INT8 build perform the same.
* The patch makes 4.1.1 2.28 times faster, and 4.0.2 1.75 times faster.
* Upgrading Mesa on its own gains nothing. Without the patch, 4.1.1 is slower than 4.0.2.

Stock Mesa 26.2.2 already contains upstream merge request 41178, which applies the same 24-bit idea
to NIR's software `sdot_4x8` lowering. It does not help here, because these shaders contain no
`OpSDot`. vkd3d-proton emits the decomposition itself, so the upstream rule never matches.

The logs are in `evidence/fsr-4.0.2-vs-4.1.1/` and `evidence/mesa-26.2.2-vs-our-patch/`.

![Pragmata title screen on the test machine, with the FSR4 watermark reading FSR4-I8 UPSCALE 4.1.1 and the OptiScaler 0.9.4 overlay open](docs/pragmata-fsr4-411-rx480.jpeg)

The photo shows FSR 4.1.1 INT8 running in Pragmata on this GCN4 card, with the patched Mesa 26.1
RADV, in August 2026. The overlay frametime is from the title screen, not a benchmark.

---

## Repository layout

| path | what |
|---|---|
| `patches/` | the Mesa, dxil-spirv and vkd3d-proton patches |
| `radv/` | the three prebuilt RADV drivers, with an install script |
| `vkd3d-proton/` | the prebuilt patched vkd3d-proton |
| `drirc.d/` | the `~/.drirc` file that turns on fp16 |
| `fsr4_dlls/` | every FSR4 upscaler DLL tested, with where each came from |
| `pragmata_working_set/` | the OptiScaler configuration and file list for Pragmata |
| `sdk_sample/`, `testkit/` | AMD's FSR sample executable, and the deployment used to benchmark it |
| `proxies/` | the frame-generation passthrough proxy the SDK sample uses |
| `evidence/` | logs backing the results above |
| `tools/` | the benchmark scripts |
| `docs/` | the one photo in this README |
| `REPRODUCE.md` | how to reproduce the measurement exactly |
| `notes/` | how the game benchmark runs headless |
| `data/` | captured SPIR-V, ISA, weights, activations, roofline analysis |

`REPRODUCE.md`, `notes/` and `tools/` describe one specific test machine, with its own paths and
hardware. They are a record of how the result was produced, not instructions for your machine.
`data/roofline.md` covers what is and is not reachable on this hardware.

Credentials are deliberately not recorded anywhere in this repository.
