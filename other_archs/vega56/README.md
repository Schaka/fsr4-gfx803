# FSR4 on a Vega 56 (gfx900)

Measured once, on 15 September 2026, against FSR 4.1.1b in Pragmata at 1280x720 upscaled to
1920x1080. Not maintained: the card it was derived on is no longer in the machine.

## Which DLL this is

The `vega` DLL is the `bc250` DLL with one more setting. It is not a wave64 version of `bc250`,
because `bc250` already is one.

Vega runs waves of 64 lanes only, and the fork asks for 32. The `bc250` DLL in the release already
carries the wave64 fix that removes that request. So it runs on this card as it is, at 5.04 ms. The
`vega` DLL is the same rebuild with `SKIP_ENTRIES=pass9,pass11`, which gives those two roles back to
AMD's own shaders. It measures 4.39 ms.

That split was measured in one game only, Pragmata. Another game can feed the network a different
load, and then the fork's own pass9 and pass11 can be the faster ones again. So the `vega` DLL is
not always the better choice on this card. Try both DLLs in your game and keep the faster one. The
two give the same picture, because every shader in both is exact, so frametime is the only thing to
compare.

## What to run

First run `./install.sh` from the main archive, the same as for the GCN4 path. It puts the patched
RADV under `~/.local/share/radv-fsr4` and writes the
`~/.local/share/radv-fsr4/radeon_icd.x86_64.json` that the command below points at. That file does
not exist until you do this, and the patched driver matters here: see the table at the end.

Then take `amd_fidelityfx_upscaler_dx12.vega.dll` from the release's DLL archive, rename it to
`amd_fidelityfx_upscaler_dx12.dll`, and put it in the `OptiScaler/` folder of the game directory.
Finally put `fsr4-vega` in front of the game, the same way `fsr4-run` works on the GCN4 path:

    VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
    PROTON_FSR4_UPGRADE=0 /path/to/fsr4-vega %command%

`install.sh` takes a directory if you want it somewhere else, and then `VK_DRIVER_FILES` points
there instead.

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
