# FSR4 upscaler DLLs

The FSR4 `amd_fidelityfx_upscaler_dx12.dll` builds used in this repository.

| directory | version | size | md5 | Pragmata, patched RADV |
|---|---|---:|---|---|
| `4.1.1-stock/` | 4.1.1 | 28,761,864 | `429d308876434a1247f40bd543efdddb` | 27.32 ms. Use this one or 4.1.1b. |
| `4.1.1b-int8/` | 4.1.1b | 34,013,696 | `15103d8c121636b1c3ef1184357b677e` | 27.78 ms |
| `4.0.2-int8/` | 4.0.2 | 40,664,064 | `0ca99991ce3669d1d5320eb011aadb25` | 30.38 ms. Used by the SDK sample test kit. |
| `sdk2.0-base/` | 4.0.2 | 13,126,656 | `41f10d709d7017b4d4686601ddb798e1` | Not measured. The test kit uses its `amd_fidelityfx_dx12.dll`. |

The timings are from `../evidence/fsr-4.0.2-vs-4.1.1/`.

## Where each one came from

| directory | origin |
|---|---|
| `4.1.1-stock/` | Ships with Pragmata and with the FidelityFX SDK sample. The same file in both. |
| `4.1.1b-int8/` | `FSR_4.1.1b_INT8_with_RDNA2_fix.7z`, published by the OptiScaler community. |
| `4.0.2-int8/` | The FSR4 INT8 build from a community setup guide. |
| `sdk2.0-base/` | Bundled with OptiScaler 0.7.9. |

## Pass names

4.1.1 runs its INT8 model through shader passes that carry `fp8` in their names. The 4.0.2 build
names its passes `fsr4_model_v07_i8_pass*`. The names do not identify the precision. The FSR4
watermark does: with `Fsr4EnableWatermark=true`, both 4.1.1 DLLs show `FSR4-I8 UPSCALE 4.1.1`.

## Companion files

`4.1.1-stock/` also holds `amd_fidelityfx_loader_dx12.dll` and
`amd_fidelityfx_framegeneration_dx12.dll` from the same set. `sdk2.0-base/` holds
`amd_fidelityfx_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll`. `../testkit/assemble.sh`
uses the 4.1.1 loader and the SDK 2.0 `amd_fidelityfx_dx12.dll`.

## Measure one in Pragmata

Add a line to `RUNS` in `../tools/bench_pragmata_dll_matrix.sh` and run it on the test machine. Read
the logs as `../notes/HEADLESS_GAME.md` describes.
