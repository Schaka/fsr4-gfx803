#!/bin/bash
# Completes the test kit. Five of its files exist elsewhere in the repo, so the repo stores
# them once and this script copies them into place. Run it once after cloning.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
R="$ROOT/testkit/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release"

cp "$ROOT/sdk_sample/patched/FidelityFX_FSR.exe"               "$R/FidelityFX_FSR.exe"
cp "$ROOT/fsr4_dlls/4.0.2-int8/amd_fidelityfx_upscaler_dx12.dll" "$R/amd_fidelityfx_upscaler_dx12.dll"
cp "$ROOT/fsr4_dlls/4.1.1-stock/amd_fidelityfx_loader_dx12.dll"  "$R/amd_fidelityfx_loader_dx12.dll"
cp "$ROOT/fsr4_dlls/sdk2.0-base/amd_fidelityfx_dx12.dll"         "$R/amd_fidelityfx_dx12.dll"
# The sample loads a frame-generation DLL. The passthrough proxy keeps frame generation off.
cp "$ROOT/proxies/fg_passthrough_proxy.dll"                      "$R/amd_fidelityfx_framegeneration_dx12.dll"

cd "$R"
md5sum -c --quiet <<'EOF'
7f68c091536c9839878abe2e39347aa6  FidelityFX_FSR.exe
0ca99991ce3669d1d5320eb011aadb25  amd_fidelityfx_upscaler_dx12.dll
de911b016f4849d4dc7a358058c9e3cb  amd_fidelityfx_loader_dx12.dll
17cfa67bd4692e1339f9bc8fc25b1d11  amd_fidelityfx_dx12.dll
37df1eea87c5cb5f923852972647e05b  amd_fidelityfx_framegeneration_dx12.dll
d917da31633f309195916d8ed0570d82  dxgi.dll
EOF
echo "test kit complete: $R"
