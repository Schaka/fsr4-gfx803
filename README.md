# FSR4 INT8 on Polaris (gfx803)

Run AMD's FSR4 upscaler on GCN4 cards, such as the RX 470, RX 480, RX 570 and RX 580, and make it
fast enough to use.

Three pieces do the work.

1. **A patched RADV.** GCN4 has no hardware `dp4a`, so every int8 multiply-accumulate in FSR4's
   network is emulated. Stock Mesa uses full 32-bit multiplies. The patch emits `v_mad_i32_i24`
   instead, a full-rate 24-bit multiply-add that GCN has had since its first generation, and it is
   exact for 8-bit operands. In Pragmata that alone makes frames 2.28 times faster than stock Mesa.
2. **A Vulkan layer.** It does two things. It rewrites FSR4's packed dot products into the plain
   32-bit multiplies the driver patch matches, on any card that has no hardware instruction for
   them. It also swaps FSR4's network shaders for tuned ones, in which the weights are constants and
   two output channels share one multiply through a quantized, packed operand. A shader the driver
   rejects falls back to the game's own. On a Vega 56 the upscaler pass goes from 7.5 ms to 3 ms, and
   on an RX 570 from about 14.5 ms to 9.5 ms.

3. **A choice of upscaler DLL.** `stock` is AMD's own. `bc250` is the `daniel-h-0/bc250-fsr4-fork`
   rebuild at `v4.0.0-rc10`, made to run on GCN4, whose shaders carry their weights as constants.
   `hybrid` is that rebuild with ten model passes handed back to AMD's shaders, so the tuned sets
   can replace them, and it is the fastest of the three. `FSR4_DLL` tells the launcher which one you
   installed, because the four tier names below map to a different shader set on each.
   `docs/DLLS.md` describes all three.

Nothing else is patched. Your normal Proton and vkd3d-proton are used as they are.

Neither piece is installed system-wide. The driver is a second copy of RADV that one game opts into
through an environment variable, and the layer is enabled per game in the same way.

---

## Quick start

Install the driver as described below, then put the launcher in front of the game.

**Steam.** In the game's launch options:

```
FSR4_DLL=stock FSR4_SET=balanced PROTON_FSR4_UPGRADE=0 \
    /path/to/tools/fsr4_layer/fsr4-run %command%
```

**Heroic.** Settings, Advanced, Wrapper command:

```
/path/to/tools/fsr4_layer/fsr4-run
```

and add `FSR4_DLL`, `FSR4_SET=balanced` and `PROTON_FSR4_UPGRADE=0` to the environment variables in
the same panel. Without `PROTON_FSR4_UPGRADE=0` Proton replaces the FSR4 DLL on every launch and
undoes whichever one you installed.

**Anything else.** Put `fsr4-run` in front of the command.

Build the layer once before first use:

```bash
cd tools/fsr4_layer
gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread
```

### The sets

`FSR4_SET` picks how far the shaders are rewritten. Four names cover the common cases, and each DLL
maps them to its own best set, so read this table together with `FSR4_DLL` in `docs/DLLS.md`.

| name | what it does | how it looks |
|---|---|---|
| `lossless` | the same maths, with the weights baked in. Faster only where that wins | bit-identical to stock |
| `quality` | per-pass mix, every pass held under 15 percent error | hard to tell from stock |
| `balanced` | per-pass mix, under 25 percent | very close to stock |
| `speed` | drops every weight of magnitude 16 or less | visibly softer, fastest |
| `off` | replace no shader, but still rewrite the dot products | stock FSR4, faster on GCN4 |
| `none` | do not load the layer at all | stock FSR4 |

Nineteen sets ship in total, one per variant we measured, and any of them can be named directly.
`FSR4_SET=list` prints them, and `docs/SETS.md` says what each one is, what it measured on two cards,
and how it looked.

Other variables: `FSR4_DEBUG=1` prints one line per replaced shader, `FSR4_SETS` points at another
directory of sets, and `FSR4_CACHE` moves the cache.

A set holds SPIR-V that replaces what vkd3d-proton compiled, so the layer has to recognise the build
you run. Two Proton builds are covered out of the box. On a third the layer replaces nothing, which
is harmless and leaves you with `off` behaviour, and `tools/fsr4_tune/make_keys.py` teaches the
shipped sets that build in one step. `tools/fsr4_tune/` rebuilds the sets from scratch for your card.

---

## What you need

Two parts, and your normal Proton.

| part | what it does | where |
|---|---|---|
| Mesa / RADV | NIR rules that emit `v_mad_i32_i24`, so an int8 multiply-add costs one instruction | `patches/mesa-26.2.2-nir-imul24-int8.patch`, prebuilt in `radv/` |
| Vulkan layer | rewrites the packed dot products, and swaps FSR4's network shaders for tuned ones | `tools/fsr4_layer/` |

The driver patch is the one that makes FSR4 usable at all on GCN4. The layer is what turns FSR4's
multiplies into the form the driver patch matches, so the two belong together: the driver patch alone
leaves most of the gain on the table.

Use `patches/mesa-26.2.2-nir-imul24-int8.patch` for Mesa 26.2.2. `patches/mesa-nir-imul24-int8.patch`
is the original against 26.1.6. It also applies to 26.2.2, but with line offsets.

Prebuilt copies of everything, with checksums, are listed in `CHECKSUMS.md`.

---

## Prebuilt binaries

If you would rather not build anything, the
[latest release](https://github.com/Schaka/fsr4-gfx803/releases/latest) has the patched RADV, the
Vulkan layer and every shader set attached, with an `install.sh`. The numbered steps below still
describe what that script leaves to you.

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

## 3. Build the Vulkan layer

```bash
cd tools/fsr4_layer
gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread
```

That is all it needs. `fsr4-run` finds the layer and the sets next to itself.

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
FSR4_SET=balanced /path/to/tools/fsr4_layer/fsr4-run \
  %command%
```

Set `FSR4_SET=none` to run stock FSR4 on the patched driver with the layer out of the way. That is
much slower, because the layer is what feeds the driver patch.

If your machine has more than one AMD GPU, add `MESA_VK_DEVICE_SELECT=<vendor>:<device>` so the
right one is used. Find the IDs with `lspci -nn | grep VGA`.


---

## How the dot product reaches the driver patch

FSR4 drives every multiply-accumulate through DXIL's `dot4add_i8packed`. vkd3d-proton turns that into
one SPIR-V `OpSDot`, the packed 4x8 integer dot product, and GCN4 has no instruction for it, so the
driver lowers it in software. That lowering never produces the pattern the Mesa patch matches.

The layer rewrites each `OpSDot` into four sign-extending byte extracts, four multiplies and three
adds, all in 32 bits. The rewrite is exact, because a signed 4x8 dot product is nothing but the sum
of the four signed byte products. NIR then proves the operands fit in 24 bits and ACO fuses each
multiply into one `v_mad_i32_i24`, which is the whole point of the driver patch.

The layer asks the device whether it accelerates the packed signed dot product, and only rewrites
when the answer is no, so the same layer is a no-op on a card that has `dp4a`. `FSR4_NO_SDOT_EXPAND=1`
turns the rewrite off. In Pragmata on an RX 570, with the patched driver in both cases, the rewrite
alone takes whole frames from 25.99 ms to 23.50 ms.

## Did it work?

Check OptiScaler's log. A correct run shows `FSR31FeatureDx12` together with `FSR4ModelSelection`.
Turn on `Fsr4EnableWatermark=true` in `OptiScaler.ini` to see the FSR4 version on screen.

If you see `FSR2FeatureDx12_212`, FSR4 is not running at all. OptiScaler has quietly fallen back to
its own built-in FSR 2.1.2 copy, and any frametime you measure is meaningless.

---

## Results

Measured on an RX 470 in Pragmata, real gameplay, upscaler running on every frame, one identical
scene, 1280x720 upscaled to 1920x1080, with the dot product rewrite in place and FSR4's own network
shaders. They answer which FSR4 DLL to use, not which shader set.

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
to NIR's software `sdot_4x8` lowering. It helps, and it is not enough: with stock vkd3d-proton the
FSR4 shaders do contain `OpSDot`, 4,264 of them across 13 modules, and letting Mesa lower them costs
2.5 ms per frame more than the layer's own rewrite.

The logs are in `evidence/fsr-4.0.2-vs-4.1.1/` and `evidence/mesa-26.2.2-vs-our-patch/`.

---

## Upscaler times with the tuned shaders

The numbers above are whole frames with the driver patch alone. The table below is the upscaler
itself, measured with GPU timestamps around every network dispatch, on an RX 570 in Pragmata at
1280x720 to 1920x1080 with FSR 4.1.1b. `FSR4_PROFILE=1` produces these numbers on your own card.

| configuration | upscaler GPU ms | picture |
|---|---:|---|
| layer neutral, the control | 18.93 | FSR4 as it arrives |
| `off`, dot products rewritten | 14.86 | unchanged, the rewrite is exact |
| `lossless` | 14.60 | bit-identical |
| `quality` | 12.57 | hard to tell from stock |
| `balanced` | 11.32 | very close to stock |
| `speed` | 8.98 | visibly softer |

The control is the layer loaded with `FSR4_SET=off FSR4_NO_SDOT_EXPAND=1`, so it sets the same
vkd3d-proton options and changes nothing else. The dot product rewrite alone takes 4 ms off the
upscaler without touching the picture, and `speed` takes off 53 percent.

A Vega 56 gains far more. There the upscaler goes from 7.5 ms to about 3 ms. `docs/SETS.md` has the
frametimes and every other set.

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

Run the game once with `FSR4_LAYER_DUMP=/tmp/fsr4dump` to collect the shaders it really compiles,
then:

```bash
cd tools/fsr4_tune
python3 tune.py capture /tmp/fsr4dump weights.bin
python3 tune.py generate --modes pack6,pack5,pack4,prune16
python3 tune.py bench /tmp/fsr4dump
python3 tune.py install ~/.local/share/fsr4/sets/mine --mode best
```

Then run with `FSR4_SET=mine`. The layer names every dumped shader after the SPIR-V it saw and looks
a replacement up under that same name, so a set built this way needs no translation table and is
valid for exactly the Proton build you dumped it from.

![Pragmata title screen on the test machine, with the FSR4 watermark reading FSR4-I8 UPSCALE 4.1.1 and the OptiScaler 0.9.4 overlay open](docs/pragmata-fsr4-411-rx480.jpeg)

The photo shows FSR 4.1.1 INT8 running in Pragmata on this GCN4 card, with the patched Mesa 26.1
RADV, in August 2026. The overlay frametime is from the title screen, not a benchmark.

---

## Repository layout

| path | what |
|---|---|
| `patches/` | the Mesa patch |
| `radv/` | the three prebuilt RADV drivers, with an install script |
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
