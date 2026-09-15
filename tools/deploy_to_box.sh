#!/usr/bin/env bash
# Put a built release on the gfx803 box, so what you look at is what ships.
#
#   tools/deploy_to_box.sh <release dir> [host]
#
# It copies the layer, the launcher, the shader sets and the two rebuilt DLLs into the places the
# box's play.sh expects, then builds the layer there. It does not touch the driver, which the box
# already carries at /data/radv_262/patched.
set -euo pipefail
SRC="${1:?usage: deploy_to_box.sh <release dir> [host]}"
HOST="${2:-gfx803}"
L=/home/user/.local/share/fsr4
R=/data/tmp/fsr4re

[ -d "$SRC/layer" ] || { echo "no layer/ in $SRC" >&2; exit 1; }

ssh "$HOST" "mkdir -p $L/sets"
scp -q "$SRC/layer/fsr4-run" "$SRC/layer/fsr4_layer.c" "$SRC/layer/fsr4_layer.json" \
       "$SRC/layer/spv_sdot.c.inc" "$SRC/layer/autotune.h" "$SRC/layer/autotune.c.inc" \
       "$SRC/layer/profile.h" "$SRC/layer/profile.c.inc" \
       "$SRC/layer/SETS.md" "$SRC/layer/DLLS.md" "$HOST:$L/"
scp -qr "$SRC/layer/sets/." "$HOST:$L/sets/"
ssh "$HOST" "chmod +x $L/fsr4-run && cd $L && gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread && echo 'layer built on the box'"

# The two rebuilt DLLs, under the names play.sh looks for.
DLLS="$SRC/../$(basename "$SRC")-dlls"
if [ -d "$DLLS" ]; then
    scp -q "$DLLS/amd_fidelityfx_upscaler_dx12.bc250.dll"  "$HOST:$R/bc250_full_upscaler.dll"
    scp -q "$DLLS/amd_fidelityfx_upscaler_dx12.hybrid.dll" "$HOST:$R/bc250_hybrid_upscaler.dll"
    echo "both rebuilt DLLs deployed"
else
    echo "no DLL archive next to $SRC, so the box keeps the DLLs it has"
fi

ssh "$HOST" "bash $R/play.sh list" || true
echo
echo "Look at one with:  ssh $HOST   then   bash $R/play.sh <dll> <tier>"
