#!/usr/bin/env bash
# Builds the release archive from the repository.
#
#   tools/make_release.sh r3 [output_dir]
#
# The archive holds the patched RADV, the drirc option, the Vulkan layer with every shader set, an
# install script and a README. It carries no Proton and no vkd3d-proton.
set -euo pipefail
TAG="${1:?usage: make_release.sh <tag> [output_dir]}"
REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)"
OUT="${2:-$REPO/build/release}"
NAME="fsr4-gfx803-$TAG"
DIR="$OUT/$NAME"

rm -rf "$DIR"
mkdir -p "$DIR/radv" "$DIR/drirc" "$DIR/layer"

cp "$REPO/radv/mesa-26.2.2-patched/libvulkan_radeon.so" "$DIR/radv/"
# The loader resolves library_path against the json, and install.sh writes the real path in.
cat > "$DIR/radv/radeon_icd.x86_64.json" <<'JSON'
{
    "ICD": {
        "api_version": "1.4.321",
        "library_path": "REPLACED_BY_INSTALL_SH"
    },
    "file_format_version": "1.0.0"
}
JSON

cp "$REPO/drirc.d/99-fsr4-gfx803.conf" "$DIR/drirc/"

cp "$REPO/tools/fsr4_layer/fsr4_layer.c" \
   "$REPO/tools/fsr4_layer/spv_sdot.c.inc" \
   "$REPO/tools/fsr4_layer/autotune.h" \
   "$REPO/tools/fsr4_layer/autotune.c.inc" \
   "$REPO/tools/fsr4_layer/fsr4_layer.json" \
   "$REPO/tools/fsr4_layer/fsr4-run" \
   "$REPO/tools/fsr4_layer/README.md" "$DIR/layer/"
cp -r "$REPO/tools/fsr4_layer/sets" "$DIR/layer/"
cp "$REPO/docs/SETS.md" "$DIR/layer/SETS.md"
# The layer README points at the repository layout, which the archive does not have.
sed -i 's#\.\./\.\./docs/SETS\.md#SETS.md#g; s#\.\./fsr4_tune/#the repository, tools/fsr4_tune/#g' \
    "$DIR/layer/README.md"

cp "$REPO/tools/release_install.sh" "$DIR/install.sh"
cp "$REPO/tools/release_readme.md" "$DIR/README.md"
sed -i "s/RELEASE_TAG/$TAG/g" "$DIR/README.md" "$DIR/install.sh"

( cd "$DIR" && find . -type f ! -name MD5SUMS -printf '%P\n' | sort | xargs md5sum > MD5SUMS )
( cd "$OUT" && tar czf "$NAME.tar.gz" "$NAME" )
echo "$OUT/$NAME.tar.gz"
du -sh "$OUT/$NAME.tar.gz"
