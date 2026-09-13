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

2. Set up OptiScaler 0.9.4 in the game directory, with an FSR4 4.1.1 upscaler
   DLL, and set Dx12Upscaler=fsr31, UpscalerIndex=0, Fsr4Update=true and
   Fsr4ForceEnableInt8=true in OptiScaler.ini.

Then launch the game.

  Steam, in the launch options:
    VK_DRIVER_FILES=$DEST/radeon_icd.x86_64.json \\
    FSR4_SET=balanced $SRC/layer/fsr4-run %command%

  Heroic: put $SRC/layer/fsr4-run in Settings, Advanced, Wrapper command, and
  add VK_DRIVER_FILES and FSR4_SET to the environment variables.

FSR4_SET takes lossless, quality, balanced or speed, or the name of any set in
layer/sets. Run it with FSR4_SET=list to see them all, and read layer/SETS.md.
EOF
