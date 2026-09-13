# Working configuration for Pragmata

This directory holds the configuration that runs FSR4 4.1.1 INT8 in Pragmata on an RX 470
(Polaris10, GCN4), with the patched RADV. `OptiScaler_pragmata.ini` is the OptiScaler configuration
the game used.

## Results

Real gameplay from one save, upscaler on every frame, 1280x720 upscaled to 1920x1080, measured
2026-09-13.

| upscaler DLL | RADV | mean ms | fps |
|---|---|---:|---:|
| FSR4 4.1.1 stock | Mesa 26.2.2 + patch | 27.32 | 36.6 |
| FSR4 4.1.1b INT8 | Mesa 26.2.2 + patch | 27.78 | 36.0 |
| FSR4 4.1.1b INT8 | Mesa 26.2.2 stock | 63.35 | 15.8 |

For comparison, FSR4 4.0.2 INT8 took 30.38 ms with the patch. Logs and all runs are in
`../evidence/fsr-4.0.2-vs-4.1.1/`.

## Required pieces

### 1. `~/.drirc`

FSR4's fp16 shaders need the RADV feature `shaderFloat16`, which is off on GFX8 by default. Copy
`../drirc.d/99-fsr4-gfx803.conf` to `~/.drirc`.

If `vulkaninfo | grep shaderFloat16` reports `true`, the file works.

### 2. Files in the game directory, next to `PRAGMATA.exe`

| role | file | md5 |
|---|---|---|
| OptiScaler 0.9.4 | `dxgi.dll` | `d917da31633f309195916d8ed0570d82` |
| upscaler | `amd_fidelityfx_upscaler_dx12.dll` | `429d308876434a1247f40bd543efdddb` (4.1.1, ships with the game) or `15103d8c121636b1c3ef1184357b677e` (4.1.1b INT8) |
| loader | `amd_fidelityfx_loader_dx12.dll` | `6bf1c9ef09f2b9d996ecf4cf6305444c` |
| loader under the name OptiScaler 0.9.4 expects | `amd_fidelityfx_dx12.dll` | `6bf1c9ef09f2b9d996ecf4cf6305444c` (copy of the loader) |
| frame generation | `amd_fidelityfx_framegeneration_dx12.dll` | `6a12bc4dc82ae11e81c37930b439f6c9` |
| Vulkan | `amd_fidelityfx_vk.dll` | `9718fd774c61be1af6af411cf65394cd` |

The patched vkd3d-proton (`d3d12.dll` and `d3d12core.dll`) must be in the Proton installation and in
the prefix.

### 3. `OptiScaler.ini`

Use `OptiScaler_pragmata.ini`. These keys are required:

```
[Upscalers]
Dx12Upscaler=fsr31
UpscalerIndex=0
Fsr4Update=true
Fsr4ForceEnableInt8=true
```

### 4. Environment

```
MESA_VK_DEVICE_SELECT=1002:67df
WINEDLLOVERRIDES=dxgi=n
FSR4_DOT_MODE=i32
VK_DRIVER_FILES=<path to the patched radeon_icd.x86_64.json>
```

`MESA_VK_DEVICE_SELECT` picks the RX 470 on a machine with two AMD GPUs. If the patched
`libvulkan_radeon.so` is the system driver, leave out `VK_DRIVER_FILES`.

## Confirm that FSR4 runs

```bash
grep -aoE "FSR2FeatureDx12_212|FSR31FeatureDx12|FSR4ModelSelection" OptiScaler.log | sort | uniq -c
```

A correct run shows `FSR31FeatureDx12` and `FSR4ModelSelection`, and no `FSR2FeatureDx12_212`. With
`Fsr4EnableWatermark=true`, the game shows `FSR4-I8 UPSCALE 4.1.1` in the top left.

## Notes

- `FSR4_DOT_MODE` has no effect in this game. Its FSR4 shaders ship as DXBC that is already lowered
  to scalar INT8 math, with no dot-product operations. The patched RADV fuses those scalar
  multiplies, so the driver does all the work.
- `VKD3D_SHADER_DUMP_PATH` writes nothing for this game.
