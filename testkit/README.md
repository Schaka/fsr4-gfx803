# The FSR4 SDK sample test kit

This is the deployment of AMD's FidelityFX FSR sample that produced the SDK sample results in
`../REPRODUCE.md`. It was copied from the test machine on 2026-09-12.

Five of its files also exist elsewhere in this repository, so the repository stores them only once.
Run `./assemble.sh` after cloning. It copies them into place and checks every checksum.

## The directory layout is required

The sample finds its assets through `..\..\..\..\..\..\media\`. The executable must therefore
stay exactly six levels below the tree root. That is why the path here is
`Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release`. A flat copy breaks asset loading.

## Files in `Release/`

| file | what it is | origin |
|---|---|---|
| `dxgi.dll` | OptiScaler 0.9.4 | OptiScaler release. Stored only here. |
| `OptiScaler.ini` | OptiScaler configuration with `Dx12Upscaler=fsr31`, `Fsr4Update=true`, `Fsr4ForceEnableInt8=true` and frame generation off | Edited for this rig |
| `configs/cauldronconfig.json` | sample configuration, `"Vsync": false` | Edited from the sample release |
| `configs/fsrapiconfig.json` | sample configuration, `"FPSLimiter": { "Enable": false }` | Edited from the sample release |
| `configs/rm_configs/` | render-module configurations | Unchanged sample release |
| `shaders/` | HLSL sources that the sample compiles at run time | Unchanged sample release |
| `cauldron.res` | Cauldron framework resources | Unchanged sample release |
| `D3D12Core.dll` | Microsoft DirectX 12 Agility SDK runtime | Unchanged sample release |
| `dxcompiler.dll`, `dxil.dll` | Microsoft DirectX shader compiler | Unchanged sample release |
| `WinPixEventRuntime.dll` | Microsoft PIX event markers | Unchanged sample release |
| `amd_ags_x64.dll`, `amd_acs_x64.dll` | AMD GPU Services libraries | Unchanged sample release |

Copied in by `assemble.sh`:

| file | copied from |
|---|---|
| `FidelityFX_FSR.exe` | `../sdk_sample/patched/FidelityFX_FSR.exe`, the exe with both binary patches |
| `amd_fidelityfx_upscaler_dx12.dll` | `../fsr4_dlls/4.0.2-int8/` |
| `amd_fidelityfx_loader_dx12.dll` | `../fsr4_dlls/4.1.1-stock/` |
| `amd_fidelityfx_dx12.dll` | `../fsr4_dlls/sdk2.0-base/` |
| `amd_fidelityfx_framegeneration_dx12.dll` | `../proxies/fg_passthrough_proxy.dll` |

The vsync and FPS-limiter edits are the only changes to `configs/`. Without them every run reads
exactly 16.67 ms.

## Not included

The Cauldron media, about 1.5 GB of Sponza assets, is not tracked. It ships with the FidelityFX
SDK sample release. Put it at the tree root as `media/`, next to `Samples/`.

## Deploying it

```bash
./testkit/assemble.sh
rsync -a testkit/ <test machine>:/data/fsr_sdk_test/repo/
```

Then follow `../REPRODUCE.md` sections 5 and 6. The test machine also needs `~/.drirc`, from
`../drirc.d/99-fsr4-gfx803.conf`.
