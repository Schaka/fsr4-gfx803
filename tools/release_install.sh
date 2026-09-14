#!/usr/bin/env bash
# Installs the patched RADV and builds the Vulkan layer, for your user only.
# It does not touch system Mesa. Nothing uses this driver unless VK_DRIVER_FILES points at it.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-$HOME/.local/share/radv-fsr4}"

mkdir -p "$DEST"
cp "$SRC/radv/libvulkan_radeon.so" "$DEST/"
sed "s|REPLACED_BY_INSTALL_SH|$DEST/libvulkan_radeon.so|" \
    "$SRC/radv/radeon_icd.x86_64.json" > "$DEST/radeon_icd.x86_64.json"

echo "Installed the driver to $DEST"
if ldd "$DEST/libvulkan_radeon.so" | grep -q "not found"; then
  echo
  echo "MISSING LIBRARIES:"
  ldd "$DEST/libvulkan_radeon.so" | grep "not found"
  echo "Install those, or build from source. See the repository README."
  exit 1
fi
echo "All libraries resolved."

if command -v gcc >/dev/null 2>&1; then
  gcc -O2 -fPIC -shared -o "$SRC/layer/libfsr4_layer.so" "$SRC/layer/fsr4_layer.c" -lpthread
  echo "Built the Vulkan layer in $SRC/layer"
else
  echo "gcc was not found, so the layer was not built. Build it later with:"
  echo "  gcc -O2 -fPIC -shared -o $SRC/layer/libfsr4_layer.so $SRC/layer/fsr4_layer.c -lpthread"
fi

cat <<EOF

Two things this script does NOT do for you.

1. Enable fp16. Copy drirc/99-fsr4-gfx803.conf to ~/.drirc
   If ~/.drirc already exists, merge it by hand instead of overwriting.
   Then check:  vulkaninfo | grep shaderFloat16      (must say true)

2. Set up the upscaler in the game directory. There are two ways, and
   README.md says which one to pick.

   The stock path: OptiScaler 0.9.4 with an FSR4 4.1.1 upscaler DLL, and
   Dx12Upscaler=fsr31, UpscalerIndex=0, Fsr4Update=true and
   Fsr4ForceEnableInt8=true in OptiScaler.ini.

   The BC-250 path: OptiScaler 10.0.0-pre1 or newer with a DLL you build
   yourself. Follow bc250/README.md, then bc250/hybrid.md.

Set PROTON_FSR4_UPGRADE=0 as well, or Proton replaces the FSR4 DLL on every
launch and undoes whichever one you installed.

Then launch the game.

  Steam, in the launch options:
    VK_DRIVER_FILES=$DEST/radeon_icd.x86_64.json \\
    PROTON_FSR4_UPGRADE=0 FSR4_SET=balanced $SRC/layer/fsr4-run %command%

  Heroic: put $SRC/layer/fsr4-run in Settings, Advanced, Wrapper command, and
  add VK_DRIVER_FILES and FSR4_SET to the environment variables.

FSR4_SET takes lossless, quality, balanced or speed, or the name of any set in
layer/sets. Run it with FSR4_SET=list to see them all, and read layer/SETS.md.
EOF
