# FSR4 INT8 on Polaris (gfx803)

Run AMD's FSR4 upscaler on GCN4 cards, such as the RX 470, RX 480, RX 570 and RX 580, and make it
fast enough to use.

Two pieces do the work.

1. **A patched RADV.** GCN4 has no hardware `dp4a`, so every int8 multiply-accumulate in FSR4's
   network is emulated. Stock Mesa uses full 32-bit multiplies. The patch emits `v_mad_i32_i24`
   instead, a full-rate 24-bit multiply-add that GCN has had since its first generation, and it is
   exact for 8-bit operands. In Pragmata that alone makes frames 2.28 times faster than stock Mesa.
2. **A Vulkan layer with tuned shaders.** FSR4's network shaders are rewritten: the weights become
   constants, and two output channels share one multiply through a quantized, packed operand. The
   layer swaps them in at run time, and falls back to the game's own shader whenever the driver
   rejects one. On a Vega 56 the upscaler pass goes from 7.5 ms to 3 ms, and on an RX 570 from about
   14.5 ms to 9.5 ms.

Neither piece is installed system-wide. The driver is a second copy of RADV that one game opts into
through an environment variable, and the layer is enabled per game in the same way.

---

## Quick start

Install the driver as described below, then put the launcher in front of the game.

**Steam.** In the game's launch options:

```
FSR4_SET=balanced /path/to/tools/fsr4_layer/fsr4-run %command%
```

**Heroic.** Settings, Advanced, Wrapper command:

```
/path/to/tools/fsr4_layer/fsr4-run
```

and add `FSR4_SET=balanced` to the environment variables in the same panel.

**Anything else.** Put `fsr4-run` in front of the command.

Build the layer once before first use:

```bash
cd tools/fsr4_layer
gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread
```

### The sets

`FSR4_SET` picks how far the shaders are rewritten. Try them in a game you know and keep the one you
like. The error each one adds is measured against the exact result, and the tables further down show
what it costs in milliseconds.

| value | what it does | how it looks |
|---|---|---|
| `quality` | tight error budget per pass, mostly 5-bit and 6-bit packing | hard to tell from stock |
| `balanced` | medium budget, mixes 4-bit packing, 3-bit packing and light pruning | very close to stock |
| `speed` | drops every weight of magnitude 16 or less | visibly softer, clearly faster |
| `off` | change nothing | stock FSR4 |

Other variables: `FSR4_DEBUG=1` prints one line per replaced shader, `FSR4_SETS` points at another
directory of sets, and `FSR4_CACHE` moves the cache.

The sets in `tools/fsr4_layer/sets/` are built for the vkd3d-proton in this repository. If you use a
different build, the layer finds no match and changes nothing, and `tools/fsr4_tune/` rebuilds them
for your card and your vkd3d.

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
FSR4_SET=balanced /path/to/tools/fsr4_layer/fsr4-run \
  %command%
```

Drop the `fsr4-run` line to run stock FSR4 shaders on the patched driver.

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

---

## Upscaler times with the tuned shaders

The numbers above are whole frames with the driver patch alone. The table below is the upscaler pass
only, at 1280x720 to 1920x1080 with FSR 4.1.1b, which is what the layer changes.

| shaders | Vega 56 | RX 570 | picture |
|---|---:|---:|---|
| stock FSR4 | 7.5 | about 14.5 | the reference |
| weights baked in, exact | 7.5 | 14 | bit-identical to stock |
| 5-bit packing, all network passes | 6 | 14.6 | no visible change |
| `quality` set | 4.0 to 4.5 | 13 | hard to tell from stock |
| `balanced` set | 3.5 | 11 | very close to stock |
| `speed` set | 3 to 5 | 9.5 | visibly softer |

The Vega gains far more than Polaris. Polaris already runs FSR4's own code at one vector instruction
per multiply, because it extracts weight bytes on the scalar unit, so there is less to win. The Vega
also rewards weight baking on its own, while on Polaris baking alone is close to a wash.

Two findings are worth knowing before you pick a set.

1. **Leave the output head alone.** Rewriting it costs temporal stability, whatever the method. The
   head writes the history buffer that the next frame reads, so an error there returns frame after
   frame. None of the shipped sets touches it.
2. **At equal measured error, pruning looks worse than quantization.** Pruning removes a share of
   every sum, which biases the result, while quantization spreads a small unbiased error over every
   term. Prefer the packed sets when the two are close.

`notes/FSR4_411_ANALYSIS.md` has the per-pass measurements behind all of this.

---

## Rebuilding the sets for your card

The best variant differs per card: on the Vega, 5-bit packing wins on pass9, and on Polaris FSR4's
own shader wins there. `tools/fsr4_tune/` rebuilds and re-times everything on the card it runs on.
See its README. The short version:

```bash
cd tools/fsr4_tune
python3 tune.py capture /your/shader/dump weights.bin
python3 tune.py generate --modes pack6,pack5,pack4,prune16
python3 tune.py bench /your/shader/dump
python3 tune.py install ./override --mode best
python3 install_layer.py /your/shader/dump ./override ~/.cache/fsr4_opt/spirv
```

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
