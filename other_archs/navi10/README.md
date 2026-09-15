# FSR4 on an RX 5700 XT (gfx1010, Navi 10, RDNA1)

Measured once, on 15 September 2026, against FSR 4.1.1b in Pragmata at 1280x720 upscaled to
1920x1080. Not maintained: the card it was derived on is no longer in the machine.

This is the simplest of the three cards. Nothing in this repository needs to be built or installed
for it, and there is no DLL of its own.

## What to run

Take `amd_fidelityfx_upscaler_dx12.bc250.dll` from the release's DLL archive, rename it to
`amd_fidelityfx_upscaler_dx12.dll`, and put it in the `OptiScaler/` folder of the game directory.
That is the whole recipe.

    PROTON_FSR4_UPGRADE=0 %command%

It needs OptiScaler 10.0.0-pre1 or newer, with `Dx12Upscaler=ffx`, `UpscalerIndex=0` and
`Fsr4ForceModel=2`. Without `PROTON_FSR4_UPGRADE=0`, Proton replaces the DLL on every launch.

There is no wrapper here and no `fsr4-run`. The Vulkan layer does nothing measurable on this card,
so there is nothing for one to do.

## Why there is nothing else

Three things that matter on the other two cards are worth nothing here, and each was measured rather
than assumed.

**The patched driver.** 0.05 ms on the DLL you would actually run. It is still worth 1.9x on AMD's
own shaders, so the patch is not useless on this card, only useless for this build.

| driver | the fork's build | AMD's DLL |
|---|---:|---:|
| stock Mesa 26.2.2 | 4.25 | 13.13 |
| patched | 4.20 | 6.78 |

**The Vulkan layer.** Its dot product rewrite measures 9.04 ms of frametime with it on and 9.06 with
it off. The fork's shaders are float chains that never reach the path it rewrites.

**Giving any role back to AMD's shaders.** On the Vega two roles win by 0.3 to 0.4 ms. Here not one
of the eight tested does, and the wholesale split costs 0.2 to 0.3 ms. The fork targets RDNA2, so on
RDNA1 its shaders are already the right shape.

| build | upscaler ms | frametime ms |
|---|---:|---:|
| the fork everywhere | 4.38 | 9.04 |
| AMD keeps pass9 | 4.48 | 9.14 |
| AMD keeps pass7 | 4.40 | 9.11 |
| AMD keeps the prepass | 4.50 | 9.09 |
| AMD keeps pass6 | 4.50 | 9.10 |
| AMD keeps the postpass | 4.43 | 9.23 |
| AMD keeps pass11 | 4.69 | 9.27 |
| AMD keeps pass12 | 4.57 | 9.35 |
| AMD keeps pass1 | 4.74 | 9.46 |
| the Vega split, pass9 and pass11 | 4.65 | 9.22 |

## The wave size, which was the surprise

RDNA1 runs waves of 32 or 64, and the fork asks for 32, which is what it was written against. So
`wave64_fix.py` should have been unnecessary here, and forcing 64 should have cost something. It
does the opposite:

| build | upscaler ms | frametime ms |
|---|---:|---:|
| wave64, prepass in fp32 | 4.33 | 8.98 |
| wave32, as the fork ships | 4.40 | 9.10 |
| wave32, prepass in fp32 | 4.47 | 9.05 |

About 0.1 ms in favour of wave64. It holds across both cycles, so it is real, but at 110 fps it is
1.3 percent and nobody can see it: the two builds were compared side by side on the card and are
indistinguishable in both picture and feel.

So the wave64 build is not recommended here because it is faster. It is recommended because it is
the `bc250` DLL that already ships, and using it means this card needs no binary of its own.
Shipping the wave32 build to gain 0.1 ms would mean carrying a fifth DLL for a difference no one can
observe. The useful conclusion is the other one: the wave64 fix costs nothing even on a card that
runs waves of 32 natively, so one build covers all three architectures.

## How the DLL was built

It is the `bc250` DLL from the main release, unchanged, md5 `da72b6dc8b8af7e16ec05ec4db23bb4d`. See
`../../tools/bc250/README.md`. No `SKIP_ENTRIES`.
