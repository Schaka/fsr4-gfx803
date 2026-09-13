# Checksums

Every binary file tracked in this repository, with its size and md5. Each directory's README
explains what the files are and where they came from.

| file | size | md5 | what it is |
|---|---:|---|---|
| `fsr4_dlls/4.0.2-int8/amd_fidelityfx_upscaler_dx12.dll` | 40664064 | `0ca99991ce3669d1d5320eb011aadb25` | FSR4 4.0.2 INT8 upscaler. |
| `fsr4_dlls/4.1.1-stock/amd_fidelityfx_framegeneration_dx12.dll` | 40085776 | `6a12bc4dc82ae11e81c37930b439f6c9` | Frame generation from the 4.1.1 set. |
| `fsr4_dlls/4.1.1-stock/amd_fidelityfx_loader_dx12.dll` | 26376 | `de911b016f4849d4dc7a358058c9e3cb` | FFX loader from the 4.1.1 set. |
| `fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll` | 28761864 | `429d308876434a1247f40bd543efdddb` | FSR4 4.1.1 upscaler, unchanged. |
| `fsr4_dlls/4.1.1b-int8/amd_fidelityfx_upscaler_dx12.dll` | 34013696 | `15103d8c121636b1c3ef1184357b677e` | FSR4 4.1.1b INT8 upscaler. |
| `fsr4_dlls/sdk2.0-base/amd_fidelityfx_dx12.dll` | 13312 | `17cfa67bd4692e1339f9bc8fc25b1d11` | SDK 2.0 base FFX API DLL. |
| `fsr4_dlls/sdk2.0-base/amd_fidelityfx_framegeneration_dx12.dll` | 1079296 | `af3b8e4a737c7d8abb81881c425de27a` | SDK 2.0 base frame generation. |
| `fsr4_dlls/sdk2.0-base/amd_fidelityfx_upscaler_dx12.dll` | 13126656 | `41f10d709d7017b4d4686601ddb798e1` | SDK 2.0 base upscaler. |
| `proxies/fg_passthrough_proxy.dll` | 250830 | `37df1eea87c5cb5f923852972647e05b` | Frame-generation passthrough proxy for the SDK sample. |
| `radv/mesa-26.1.6-patched/libvulkan_radeon.so` | 21075568 | `a076b4c7d31da85ebc11496265a1f421` | RADV, Mesa 26.1.6 + `patches/mesa-nir-imul24-int8.patch`. |
| `radv/mesa-26.2.2-patched/libvulkan_radeon.so` | 21874128 | `a34fca39f8839fcaefe46baf7c81775c` | RADV, Mesa 26.2.2 + `patches/mesa-26.2.2-nir-imul24-int8.patch`. |
| `radv/mesa-26.2.2-stock/libvulkan_radeon.so` | 21869976 | `d698ce62ca4edf969a42916f6456327f` | RADV, Mesa 26.2.2 unchanged. The control. |
| `tools/fsr4_layer/sets/balanced/` | 2437608 | `6004cee7836d5a9670f7963299e46e69` | 11 tuned FSR4 shaders, md5 over the set. |
| `tools/fsr4_layer/sets/quality/` | 2729560 | `d7cc493ef460b1d211dd61ca4736bd01` | 11 tuned FSR4 shaders, md5 over the set. |
| `tools/fsr4_layer/sets/speed/` | 2726184 | `50f4d7fcfc74bff1a035fb1acf18a06e` | 12 tuned FSR4 shaders, md5 over the set. |
| `sdk_sample/patched/FidelityFX_FSR.exe` | 1624576 | `7f68c091536c9839878abe2e39347aa6` | SDK sample exe with both byte patches. |
| `sdk_sample/patched/FidelityFX_FSR.pdb` | 19222528 | `60e7b501c9783f24f1156cf551feaa16` | SDK sample debug symbols. |
| `sdk_sample/stock/FidelityFX_FSR.exe` | 1624576 | `546eb03b61c408fb65b387d68a77f89a` | SDK sample exe, unchanged. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/D3D12Core.dll` | 4533320 | `2d454c43492aa8ea0f25a8dfad5ca706` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/WinPixEventRuntime.dll` | 58368 | `81fe582ce4f18c7caead8890204a3c05` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_acs_x64.dll` | 125952 | `ed0a415ba4e8a4e5c57947e8dec945da` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_ags_x64.dll` | 179408 | `9352802fae8e6ff020afd9257f33af23` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/cauldron.res` | 7656 | `0006da05fccb4a3076bfcd33ebd510e2` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/dxcompiler.dll` | 18091048 | `94b42f116a6e80925d27112b15b058e2` | SDK sample runtime file. See `testkit/README.md`. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/dxgi.dll` | 25379632 | `d917da31633f309195916d8ed0570d82` | OptiScaler 0.9.4. |
| `testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/dxil.dll` | 1525280 | `539ceaadca75cafd3bcb4eca94c01d3b` | SDK sample runtime file. See `testkit/README.md`. |
| `vkd3d-proton/d3d12.dll` | 151552 | `5a52fc493768e145d45530ca3607639f` | vkd3d-proton loader. Pairs with the above. |
| `vkd3d-proton/d3d12core.dll` | 6176768 | `688bc2f88dfa2b77c54d94a9b9e86ca0` | vkd3d-proton with the dxil-spirv patch. Provides `FSR4_DOT_MODE`. |

## Not tracked

| item | where to get it |
|---|---|
| proton-cachyos-11.0-20260703-slr-x86_64 | Download URL in `REPRODUCE.md` section 4. |
| Cauldron sample media (Sponza, about 1.5 GB) | The FidelityFX SDK sample release. |
| `libSPIRV-Tools.so` for the 26.2.2 drivers | Your distribution's SPIRV-Tools package. |
| OptiScaler 0.9.4 | The OptiScaler release. `dxgi.dll` above is a copy. |

## Verify

```bash
grep -oE '^\| `[^`]+` \| [0-9]+ \| `[0-9a-f]{32}`' CHECKSUMS.md \
  | sed -E 's/^\| `([^`]+)` \| [0-9]+ \| `([0-9a-f]{32})`/\2  \1/' | md5sum -c --quiet -
```

Run `testkit/assemble.sh` first, or the check reports nothing for the copied files, because they
are not listed here.
