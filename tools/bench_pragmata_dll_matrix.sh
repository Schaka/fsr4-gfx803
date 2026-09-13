#!/bin/bash
# Runs bench_pragmata.sh for several upscaler DLL and driver pairs, one after another.
# Each run swaps amd_fidelityfx_upscaler_dx12.dll in the game directory first and records
# its md5, so the log shows which DLL each number belongs to.
#   usage: bench_pragmata_dll_matrix.sh <seconds>
#   output: /data/tmp/evidence/matrix.csv and one OptiScaler log per run
SECS=${1:-100}
G="/home/user/Games/Heroic/Prefixes/Pragmata/drive_c/Program Files (x86)/Pragmata"
E=/data/tmp/evidence; mkdir -p "$E"
PATCHED=/data/radv_262/patched/radeon_icd.x86_64.json
STOCK=/data/radv_262/stock/radeon_icd.x86_64.json
DLL402=/data/tmp/fsr402_int8.dll
DLL411B=/data/tmp/fsr411b_upscaler.dll
DLL411="$G/amd_fidelityfx_upscaler_dx12.dll.411keep"

# label | upscaler DLL | ICD
RUNS=(
  "fsr402_patched|$DLL402|$PATCHED"
  "fsr411b_patched|$DLL411B|$PATCHED"
  "fsr411b_stock|$DLL411B|$STOCK"
  "fsr411_patched|$DLL411|$PATCHED"
  "fsr402_patched_repeat|$DLL402|$PATCHED"
)

echo "label,dll_md5,frames,mean_ms,median_ms,min_ms,fps" > "$E/matrix.csv"
for r in "${RUNS[@]}"; do
  IFS='|' read -r LABEL DLL ICD <<< "$r"
  cp "$DLL" "$G/amd_fidelityfx_upscaler_dx12.dll.tmp" && \
    mv "$G/amd_fidelityfx_upscaler_dx12.dll.tmp" "$G/amd_fidelityfx_upscaler_dx12.dll"
  MD5=$(md5sum "$G/amd_fidelityfx_upscaler_dx12.dll" | cut -c1-32)
  echo "=== $LABEL dll=$MD5 icd=$ICD $(date +%T)" >&2
  ROW=$(/data/fsr4_tools/bench_pragmata.sh "$LABEL" "$SECS" "$ICD")
  echo "$ROW" | sed "s/^$LABEL,/$LABEL,$MD5,/" >> "$E/matrix.csv"
  echo "$LABEL: $ROW" >&2
done
pkill -f "PRAGMA""TA.exe" 2>/dev/null
# Leave the known-good 4.0.2 INT8 DLL active.
cp "$DLL402" "$G/amd_fidelityfx_upscaler_dx12.dll"
echo "=== done $(date +%T)" >&2
