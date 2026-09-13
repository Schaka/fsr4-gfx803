#!/bin/bash
# Canonical launch environment for the FSR4-on-Polaris test rig (run ON the gfx803 box).
# Source this before launching FidelityFX_FSR.exe manually.
export FSR4_TESTDIR=/data/fsr_sdk_test/repo/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release
export WINEPREFIX=/data/fsr_sdk_test/pfx_rx7600spoof
export PROTONPATH=/data/proton-cachyos/proton-cachyos-11.0-20260703-slr-x86_64
export GAMEID=0
# Box has TWO AMD GPUs. This hides the weak OLAND card so the RX 470 is the only
# device the whole Vulkan stack can see. Also fixes overlays naming the wrong GPU.
export MESA_VK_DEVICE_SELECT=1002:67df
# radv_enable_float16_gfx8 is required. Copy drirc.d/99-fsr4-gfx803.conf from the repo
# to ~/.drirc. Check with: vulkaninfo | grep shaderFloat16   (must print true)
# Weston socket to connect to. Weston puts its socket in /run/user/1000.
# Do NOT export XDG_RUNTIME_DIR for umu-run. An overridden value makes the Wine
# client wait forever for a socket that does not exist.
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-combo-20}
