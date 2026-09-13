#!/usr/bin/env bash
# Installs the three RADV builds next to each other and writes an ICD file for each.
# Does not touch the system Mesa.
#   usage: INSTALL.sh [destination]   (default /data)
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST=${1:-/data}

install_one() {  # <source dir> <target dir>
  mkdir -p "$2"
  cp "$SRC/$1/libvulkan_radeon.so" "$2/"
  cat > "$2/radeon_icd.x86_64.json" <<EOF
{
    "ICD": {
        "api_version": "1.4.354",
        "library_arch": "64",
        "library_path": "$2/libvulkan_radeon.so"
    },
    "file_format_version": "1.0.1"
}
EOF
  echo "installed $1 -> $2"
  if ldd "$2/libvulkan_radeon.so" | grep "not found"; then
    echo "  missing dependencies above, see README.md"
  fi
}

install_one mesa-26.1.6-patched "$DEST/radv_custom"
install_one mesa-26.2.2-patched "$DEST/radv_262/patched"
install_one mesa-26.2.2-stock   "$DEST/radv_262/stock"

md5sum -c --quiet <<EOF
a076b4c7d31da85ebc11496265a1f421  $DEST/radv_custom/libvulkan_radeon.so
a34fca39f8839fcaefe46baf7c81775c  $DEST/radv_262/patched/libvulkan_radeon.so
d698ce62ca4edf969a42916f6456327f  $DEST/radv_262/stock/libvulkan_radeon.so
EOF
echo "checksums OK"
