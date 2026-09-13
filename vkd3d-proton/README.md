# Patched vkd3d-proton

`d3d12core.dll` and `d3d12.dll` are vkd3d-proton with the changes in
`../patches/vkd3d-proton-fsr4.patch` and `../patches/dxil-spirv-fsr4-int8.patch`.

| file | md5 |
|---|---|
| `d3d12core.dll` | `688bc2f88dfa2b77c54d94a9b9e86ca0` |
| `d3d12.dll` | `5a52fc493768e145d45530ca3607639f` |

All published results used these two files.

## What the changes do

The dxil-spirv patch is the part that matters for speed:

* It adds the `FSR4_DOT_MODE` switch to `emit_i8_dot_instruction`, which decides how DXIL's
  `dot4add_i8packed` is decomposed. The main `README.md` lists the values.
* It adds the INT8 and INT32 cooperative-matrix formats to `dxil_ags.cpp`.

The vkd3d-proton patch only adds diagnostics. None of them is active unless its variable is set.

| variable | effect |
|---|---|
| `FSR4_TRACE_DISPATCH` | Logs every compute `Dispatch` call. Produced `../data/dispatch_trace.log`. |
| `FSR4_FORCE_HOST_WEIGHTS` | Puts weight-sized buffers in host-visible memory so they can be read. Produced `../data/weights_runtime/`. |
| `FSR4_HOST_BIG` | Raises the size limit for `FSR4_FORCE_HOST_WEIGHTS`. |
| `FSR4_DUMP_COPIES` | Logs buffer copies. |

## Source base

| tree | commit |
|---|---|
| vkd3d-proton | `3dfc6f07d0953b1e8b41705275c2c59cc7374fc5` |
| dxil-spirv submodule | `7ecda135de740f4db016c2bbdf8b021ce6b0bebd` |

The patches are the state of the source tree after the last edit. The tracked DLLs were built
eight minutes before the last change to `libs/vkd3d/device.c`, one of the diagnostic hooks above. A
rebuild from the patches therefore does not reproduce the checksums byte for byte. The `FSR4_DOT_MODE`
code is older than both DLLs, so it is identical in both.

## Build

```bash
git clone --recursive https://github.com/HansKristian-Work/vkd3d-proton.git
cd vkd3d-proton
git checkout 3dfc6f07d0953b1e8b41705275c2c59cc7374fc5
git submodule update --init --recursive
git -C subprojects/dxil-spirv checkout 7ecda135de740f4db016c2bbdf8b021ce6b0bebd
git apply /path/to/patches/vkd3d-proton-fsr4.patch
git -C subprojects/dxil-spirv apply /path/to/patches/dxil-spirv-fsr4-int8.patch
./package-release.sh fsr4 /tmp/vkd3d-out --no-package
```
