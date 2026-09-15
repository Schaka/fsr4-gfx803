# FSR4 on a Vega 56 (gfx900)

Measured once, on 15 September 2026, against FSR 4.1.1b in Pragmata at 1280x720 upscaled to
1920x1080. Not maintained: the card it was derived on is no longer in the machine.

## What to run

Take `amd_fidelityfx_upscaler_dx12.vega.dll` from the release's DLL archive, rename it to
`amd_fidelityfx_upscaler_dx12.dll`, and put it in the `OptiScaler/` folder of the game directory.
Then put `fsr4-vega` in front of the game, the same way `fsr4-run` works on the GCN4 path:

    VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
    PROTON_FSR4_UPGRADE=0 /path/to/fsr4-vega %command%

It needs OptiScaler 10.0.0-pre1 or newer, with `Dx12Upscaler=ffx`, `UpscalerIndex=0` and
`Fsr4ForceModel=2`.

There is one tier and it is exact, so there is nothing to choose. `fsr4-vega` still takes
`FSR4_SET` if you want to try a set, and says why that is not the default.

## What it costs

| build | upscaler ms | picture |
|---|---:|---|
| AMD's own DLL, no set | 8.35 | the reference |
| the fork's build everywhere | 5.04 | identical, its shaders are exact |
| **the shipped build** | **4.39** | **identical** |
| the shipped build with `fin15` | 4.33 | possibly slightly worse |

Confirmed by eye as well as by frametime: 4.7 ms, 4.3 ms and 4.2 ms measured by hand on the card,
with no difference anyone could see between the first two.

## How it was built

The same rc10 sources and the same two scripts as the `bc250` DLL on the main path, differing only
in the skip list:

    cp -r <daniel-h-0/bc250-fsr4-fork v4.0.0-rc10>/dll work
    python3 ../../tools/bc250/wave64_fix.py work/shaders
    python3 ../../tools/bc250/fp32_prepass.py work/shaders
    cp ../../tools/bc250/build_variant.py work/
    cd work && SKIP_ENTRIES=pass9,pass11 python3 build_variant.py \
        --sdk ../../fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll \
        --dxcompiler /path/to/libdxcompiler.so --output /tmp/vega

`SKIP_ENTRIES=pass9,pass11` is the whole Vega-specific part. Everything it swaps in is AMD's own
shader, so the build has no approximation in it anywhere.

`FINDINGS.md` records what else was tried and why none of it shipped.

## The driver patch is still needed

More here than on GCN4, at least for AMD's own shaders:

| driver | AMD's DLL | the fork's build |
|---|---:|---:|
| stock Mesa 26.2.2 | 32.97 ms | 5.13 ms |
| with the patch | 8.35 ms | 4.96 ms |

The fork's shaders barely touch the path the patch accelerates, which is also why the Vulkan layer's
dot product rewrite is worth nothing on them: three modules contain one, and turning the rewrite off
moves no frametime. The layer is still what loads a set, and it still asks the device before
rewriting anything, so it is harmless to keep in front of the game.
