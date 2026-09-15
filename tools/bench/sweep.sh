#!/bin/bash
# Interleaved sweep with a deadline on every run.
#
#   sweep.sh <output csv> <cycles> <dll:set> ...
#
# It runs the whole list once to warm the shader cache, then once per cycle. Cycling rather than
# repeating a configuration spreads any slow drift evenly over all of them, so a comparison inside
# one cycle stays fair. `timeout` bounds each run, so one bad launch costs a slot and not the night.
#
# The result of each run goes to a file rather than through a pipe. A pipe stays open until every
# process holding it exits, and each run leaves a game and a compositor running in the background.
set -u
OUT=$1; CYCLES=$2; shift 2
R=/data/tmp/fsr4re
DRV=/data/radv_262/patched/radeon_icd.x86_64.json
TMP=$(mktemp)
echo "cycle,dll,set,frametime_ms,fps,upscaler_ms,frames,replaced,status" > "$OUT"
field () { grep -oE "$1=[0-9a-z.-]*" "$TMP" | tail -1 | cut -d= -f2; }
for cycle in $(seq 0 "$CYCLES"); do
  for pair in "$@"; do
    dll=${pair%%:*}; set_name=${pair##*:}
    # The stock DLL runs under OptiScaler 0.9.4, the rebuilt ones under 10.0.0-pre1.
    case $dll in
      stock) f=stock_411b_upscaler.dll; opti=094 ;;
      *)     f=bc250_${dll}_upscaler.dll; opti=10 ;;
    esac
    if [ ! -f "$R/$f" ]; then
      [ "$cycle" != 0 ] && echo "$cycle,$dll,$set_name,,,,0,0,missing-dll" | tee -a "$OUT"
      continue
    fi
    : > "$TMP"
    SWEEP_ENV="${SWEEP_ENV:-}" PROF="${PROF:-1}" timeout -k 20 560 bash $R/run_one.sh "$R/$f" "q_${dll}_${set_name}" "$DRV" "$set_name" 40 "$opti" > "$TMP" 2>&1
    [ "$cycle" = 0 ] && continue
    FT=$(field frametime); FPS=$(field fps); MS=$(field ms_per_s)
    NF=$(field frames);    ST=$(field status)
    PF=$(gawk -v a="${MS:-0}" -v b="${FPS:-0}" 'BEGIN{if(b>0)printf "%.2f",a/b}')
    REP=$(grep -ac " -> replaced" /data/tmp/v_q_${dll}_${set_name}.log 2>/dev/null)
    echo "$cycle,$dll,$set_name,$FT,$FPS,$PF,${NF:-0},${REP:-0},${ST:-timeout}" | tee -a "$OUT"
  done
done
rm -f "$TMP"
echo "=== $OUT done"
gawk -F, 'NR>1 && $4!="" {k=$2":"$3; s[k]+=$4; u[k]+=$6; n[k]++}
     END{printf "\n%-26s %10s %10s %5s\n","config","frametime","upscaler","runs";
         for(k in s) printf "%-26s %10.2f %10.2f %5d\n", k, s[k]/n[k], u[k]/n[k], n[k]}' "$OUT" | sort -k2 -n
