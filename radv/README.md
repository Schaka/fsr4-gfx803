# Prebuilt RADV drivers

Three builds of `libvulkan_radeon.so`, the Mesa Vulkan driver for AMD GPUs. All results in this
repository were measured with them.

| directory | Mesa | patch | md5 |
|---|---|---|---|
| `mesa-26.1.6-patched/` | 26.1.6, commit `ffa422e53d` | `../patches/mesa-nir-imul24-int8.patch` | `a076b4c7d31da85ebc11496265a1f421` |
| `mesa-26.2.2-patched/` | 26.2.2, commit `0ae52750` | `../patches/mesa-26.2.2-nir-imul24-int8.patch` | `a34fca39f8839fcaefe46baf7c81775c` |
| `mesa-26.2.2-stock/` | 26.2.2, commit `0ae52750` | none | `d698ce62ca4edf969a42916f6456327f` |

The stock build is the control. It already contains upstream merge request 41178, so it shows what
upstream Mesa reaches without our patch. The 26.1.6 build is the first patched driver. It performs
the same as the 26.2.2 patched build, within noise.

## Build options

The 26.2.2 builds used these options:

    -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=wayland,x11
    -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release -Db_ndebug=true

RADV compiles shaders with ACO, not LLVM. With `-Dllvm=disabled` the library links no LLVM, so it
runs on a machine with a different LLVM version.

## Install

```bash
./INSTALL.sh [destination]      # default destination: /data
```

The script copies the three drivers to `<destination>/radv_custom` (26.1.6),
`<destination>/radv_262/patched` and `<destination>/radv_262/stock`. It writes an ICD file next to
each one, with the absolute library path. It does not change the system Mesa.

Select a driver per launch:

```bash
VK_DRIVER_FILES=/data/radv_262/patched/radeon_icd.x86_64.json <command>
```

## Dependency

The 26.2.2 libraries link `libSPIRV-Tools.so`, and that library name carries no version number. If
`INSTALL.sh` reports it as missing, install the `spirv-tools-libs` package of your distribution, or
copy the library from the build machine and set `LD_LIBRARY_PATH` to its directory.
