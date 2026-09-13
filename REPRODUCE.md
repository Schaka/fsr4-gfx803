<!-- REPRODUCE.md describes the original test machine, with its own paths and hardware.
     For instructions aimed at your own machine, read README.md instead. -->

# Reproducing the Mesa 26.2.2 result

This file records everything needed to rebuild the drivers, rebuild the test rig, and repeat the
four benchmark runs. Follow the sections in order.

Checksums for every untracked binary artifact are in `CHECKSUMS.md`.

Date of the recorded result: 2026-09-12. Hardware: AMD RX 470 (gfx803, Polaris10, GCN4).

---

## 1. The result

All four runs used one identical configuration. The driver changed between them, and in run C the
layer left FSR4's packed dot products alone instead of expanding them.

| run | driver | int8 dot product | mean ms | fps |
|---|---|---|---:|---:|
| A | Mesa 26.1.6 + our patch | expanded | 23.19 | 43.1 |
| B | Mesa 26.2.2 + our patch | expanded | 23.36 | 42.8 |
| C | Mesa 26.2.2 stock | `OpSDot` | 24.96 | 40.1 |
| D | Mesa 26.2.2 stock | expanded | 35.25 | 28.4 |

Expanded means the form the layer writes: four sign-extending byte extracts, four 32-bit multiplies
and three adds per packed dot product.

Three conclusions follow.

1. Upgrading Mesa gains nothing. A and B differ by 0.7 percent, which is inside run-to-run noise.
2. Our patch is worth 34 percent on the code path we use. Compare D against B.
3. Upstream Mesa cannot replace our patch for free. C is the best that stock 26.2.2 reaches, and it
   is 6.9 percent slower than B.

Keep `patches/mesa-26.2.2-nir-imul24-int8.patch`.

Run A was repeated later the same day, after the harness environment file changed, and returned
23.20 ms against the original 23.19 ms. Treat a difference below about 1 percent as noise.

---

## 2. Why upstream Mesa does not help us

Mesa merge request 41178 landed on 2026-05-01 and first shipped in Mesa 26.2. It rewrites the NIR
software expansion of `sdot_4x8` and related opcodes to use `imul24_relaxed`. That expansion only
runs when a module reaches the driver with an `OpSDot` instruction in it.

The modules the driver sees carry none. vkd3d-proton compiles FSR4's `dot4add_i8packed` into
`OpSDot`, and the layer rewrites every one of them into byte extracts, 32-bit multiplies and adds
before the module reaches the driver. NIR therefore never receives an `nir_op_sdot_4x8_iadd` node,
and upstream's rule never fires.

Check the rewrite with `FSR4_DEBUG=1`, which prints one `dot product expanded` line per module the
layer rewrites.

Our patch matches the expanded `extract_i8` and `imul` form instead. The layer's rewrite and the
driver patch work as a pair. Run D is the control that proves it: the rewrite on a stock driver,
with nothing to fuse the multiplies, is the slowest of the four.

---

## 3. Build the drivers

Build on a fast machine. The gfx803 box has 12 slow cores.

```bash
git clone https://gitlab.freedesktop.org/mesa/mesa.git mesa-src
cd mesa-src
git fetch --depth=1 origin 26.2
git branch -f mesa262 FETCH_HEAD          # VERSION 26.2.2, commit 0ae52750
git worktree add ../mesa-262 mesa262
git worktree add ../mesa-262-patched mesa262 --detach
cd ../mesa-262-patched
git apply ../patches/mesa-26.2.2-nir-imul24-int8.patch
```

Configure both worktrees with the same options:

```bash
meson setup build.stock \
  -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=wayland,x11 \
  -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release -Db_ndebug=true
ninja -C build.stock
```

`-Dllvm=disabled` is deliberate and important. RADV uses ACO, not LLVM, so the library links no
LLVM at all. This makes the build portable between machines with different LLVM versions. Confirm
it with `ldd build.stock/src/amd/vulkan/libvulkan_radeon.so | grep -i llvm`, which must print
nothing.

The build produces `src/amd/vulkan/libvulkan_radeon.so` and `src/amd/vulkan/radeon_icd.x86_64.json`.
Edit `library_path` in the JSON file to the absolute install path.

One dependency needs a check. The library links `libSPIRV-Tools.so`, and that soname carries no
version number. If the target machine reports it missing, copy the build machine's copy across and
set `LD_LIBRARY_PATH`.

---

## 4. Rebuild the test rig on the gfx803 box

Put every file under `/data`. The `/home` partition is small.

Install these four trees:

| path | contents | source |
|---|---|---|
| `/data/fsr4_tools` | benchmark harness and the Vulkan layer | `tools/` |
| `/data/radv_custom` | Mesa 26.1.6 plus our patch | `radv/mesa-26.1.6-patched/` |
| `/data/radv_262/stock` | Mesa 26.2.2 stock | `radv/mesa-26.2.2-stock/` |
| `/data/radv_262/patched` | Mesa 26.2.2 plus our patch | `radv/mesa-26.2.2-patched/` |

`radv/INSTALL.sh` installs all three and writes their ICD files.

Install Proton-CachyOS:

```bash
cd /data/proton-cachyos
curl -L -o pc.tar.xz https://github.com/CachyOS/proton-cachyos/releases/download/cachyos-11.0-20260703-slr/proton-cachyos-11.0-20260703-slr-x86_64.tar.xz
tar xf pc.tar.xz
```

Copy the sample tree to `/data/fsr_sdk_test/repo`. The sample resolves its assets through
`..\..\..\..\..\..\media\`, so the executable must stay exactly six levels below the tree root, at
`Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release`. A flat copy breaks asset loading.

Build the layer in place:

```bash
cd /data/fsr4_tools/fsr4_layer
gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread
```

---

## 5. The load-bearing configuration

Every item below is required. The rig produces wrong numbers or no upscale at all without it.

### 5.1 The `~/.drirc` file

FSR4 needs the RADV feature `shaderFloat16`, which is off by default on GFX8. Copy
`drirc.d/99-fsr4-gfx803.conf` to `~/.drirc`.

Check it with `vulkaninfo | grep shaderFloat16`, which must report `true` with no variable set.

### 5.2 OptiScaler 0.9.4

| file | md5 |
|---|---|
| `dxgi.dll` (OptiScaler 0.9.4) | `d917da31633f309195916d8ed0570d82` |
| `amd_fidelityfx_upscaler_dx12.dll` (FSR4 4.0.2 INT8, 40 MB) | `0ca99991ce3669d1d5320eb011aadb25` |
| `FidelityFX_FSR.exe` (patched, see 5.4) | `7f68c091536c9839878abe2e39347aa6` |

Set these four keys in `OptiScaler.ini`:

```
Dx12Upscaler=fsr31
UpscalerIndex=0
Fsr4Update=true
Fsr4ForceEnableInt8=true
```

### 5.3 Turn off vsync and the frame limiter

The sample ships both of them on. Leave either one on and every run reads exactly 16.67 ms.

- `configs/cauldronconfig.json`: set `"Vsync": false`.
- `configs/fsrapiconfig.json`: set `"FPSLimiter": { "Enable": false }`.

### 5.4 The two binary patches in `FidelityFX_FSR.exe`

The sample exposes both settings only as ImGui checkboxes. Patch the constructor defaults instead.
Field offsets come from `FidelityFX_FSR.pdb` and were confirmed with `llvm-pdbutil dump -types`.

| field | offset | VMA | original | patched |
|---|---|---|---|---|
| `m_FrameInterpolation` | 169 (`0xa9`) | `0x14000e495` | `movl $0x1010101,0xa8(%rbx)` | `movl $0x1010001,0xa8(%rbx)` |
| `m_overrideVersion` | 220 (`0xdc`) | `0x14000e4db` | `mov %dil,0xdc(%rbx)` | `movb $0x1,0xdc(%rbx)` |

Both replacements keep the original instruction length, so they patch in place. The first turns
frame interpolation off. The second turns on the FSR version override, so
`m_FsrVersionIndex` (offset 216) selects the version. Both patches are already applied in
`sdk_sample/patched/FidelityFX_FSR.exe`.

### 5.5 Display

The box runs headless. Start weston with the VNC and headless backends together. The headless
backend alone has no repaint loop and hangs frame pacing.

```bash
/data/fsr4_tools/restart_compositor.sh wayland-combo-20 5920 1280 720
```

Do not set `XDG_RUNTIME_DIR` for `umu-run`. Weston puts its socket in `/run/user/1000`. An
overridden value makes the Wine client wait forever for a socket that does not exist.

---

## 6. Run the benchmarks

Run one benchmark at a time. The box has one GPU under test.

```bash
export MESA_VK_DEVICE_SELECT=1002:67df   # mandatory, hides the weak OLAND card
export FSR4_SET=off                      # rewrite the dot products, replace no shader
R=/data/fsr4_tools/fsr4_layer/fsr4-run

VK_DRIVER_FILES=/data/radv_custom/radeon_icd.x86_64.json       $R /data/fsr4_tools/bench_fsr4.sh exp 1 40   # A
VK_DRIVER_FILES=/data/radv_262/patched/radeon_icd.x86_64.json  $R /data/fsr4_tools/bench_fsr4.sh exp 1 40   # B
FSR4_NO_SDOT_EXPAND=1 \
VK_DRIVER_FILES=/data/radv_262/stock/radeon_icd.x86_64.json    $R /data/fsr4_tools/bench_fsr4.sh sdot 1 40  # C
VK_DRIVER_FILES=/data/radv_262/stock/radeon_icd.x86_64.json    $R /data/fsr4_tools/bench_fsr4.sh exp 1 40   # D
```

The first argument to the harness is the label it writes into the CSV row. Each run prints one row:
`label,skip_n,frames,mean_ms,median_ms,min_ms,fps`.

`fsr4-run` sets `VKD3D_CONFIG=pipeline_library_ignore_spirv`, and the runs need it. vkd3d-proton
otherwise serves pipelines from its own cache without handing SPIR-V to the driver, and the layer
has nothing to see.

The harness deletes both vkd3d cache files itself. Both must go, because there is a `.cache` and a
`.cache.write`, and a surviving second file makes shader translation silently reuse old results.

---

## 7. Confirm that real FSR4 runs

A fast number can mean the upscaler did no work. Check the logs before trusting any result.

```bash
grep -aoE "FSR2FeatureDx12_212|FSR31FeatureDx12|FSR4ModelSelection" OptiScaler.log | sort | uniq -c
grep -a "hkgetModelBlobSDK\|amdxcffx64" OptiScaler.log
```

`FSR2FeatureDx12_212` means OptiScaler used its own internal FSR 2.1.2 clone, not FSR4. A correct
run shows `FSR31FeatureDx12`, `FSR4ModelSelection`, and `FSR4ModelSelection::hkgetModelBlobSDK`,
and it loads `amdxcffx64.dll`.

---

## 8. Pragmata

The SDK sample results above are the first measurement. The Pragmata results, with FSR 4.0.2 and
4.1.1, are in the main `README.md`. `notes/HEADLESS_GAME.md` describes the game rig. On the test
machine, run:

```bash
/data/fsr4_tools/start_sway.sh
sudo python3 /data/fsr4_tools/vgamepad.py &
/data/fsr4_tools/bench_pragmata_dll_matrix.sh 100
```

The 4.1.1b DLL is staged at `/data/tmp/fsr411b_upscaler.dll` and the 4.0.2 DLL at
`/data/tmp/fsr402_int8.dll`. The script leaves the 4.0.2 DLL active when it finishes.
