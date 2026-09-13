#!/bin/bash
# Pragmata benchmark. Loads the save with the virtual gamepad and measures one window.
#   usage: bench_pragmata.sh <label> <seconds> [icd_path]
#   emits: label,frames,mean_ms,median_ms,min_ms,fps
#
# Proof that the run is real gameplay comes from the OptiScaler log.
LABEL=${1:-run}; SECS=${2:-100}; ICD=$3
G="/home/user/Games/Heroic/Prefixes/Pragmata/drive_c/Program Files (x86)/Pragmata"
E=/data/tmp/evidence; mkdir -p "$E"
FIFO=/data/tmp/vgamepad.fifo
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-1
export WINEPREFIX=/home/user/Games/Heroic/Prefixes/Pragmata
export PROTONPATH=/home/user/.config/heroic/tools/proton/proton-cachyos-11.0-20260703-slr
[ -p "$FIFO" ] || { echo "$LABEL: no gamepad FIFO" >&2; exit 1; }

pkill -f "PRAGMA""TA.exe" 2>/dev/null; sleep 3
pkill -x -9 wineserver 2>/dev/null
for i in $(seq 1 30); do pgrep -x wineserver >/dev/null || break; sleep 2; done
sleep 5

cd "$G" || exit 1
rm -f OptiScaler.log
export GAMEID=0 MESA_VK_DEVICE_SELECT=1002:67df WINEDLLOVERRIDES=dxgi=n
export FSR4_DOT_MODE=i32
[ -n "$ICD" ] && export VK_DRIVER_FILES="$ICD"
nohup umu-run "PRAGMATA.exe" > "/data/tmp/pragmata_${LABEL}.log" 2>&1 &
disown

sleep 85
for i in $(seq 1 12); do echo A > "$FIFO"; sleep 4; done
sleep 45
pgrep -f "PRAGMA""TA.exe" >/dev/null || { echo "$LABEL: GAME NOT RUNNING" >&2; exit 1; }

T1=$(date +%H:%M:%S); sleep "$SECS"; T2=$(date +%H:%M:%S)
cp OptiScaler.log "$E/${LABEL}.log" 2>/dev/null
gawk -v t1="$T1" -v t2="$T2" -v l="$LABEL" "
  /Frametime/ { ts=substr(\$0,2,8)
    if (ts>=t1 && ts<=t2 && match(\$0,/Frametime: [0-9]+\.[0-9]+/)) {
      v=substr(\$0,RSTART+11,RLENGTH-11)+0; a[++n]=v; t+=v } }
  END { if(n<50){printf \"%s: only %d frames, rejected\n\",l,n > \"/dev/stderr\"; exit 1}
    asort(a); printf \"%s,%d,%.2f,%.2f,%.2f,%.1f\n\", l,n,t/n,a[int(n/2)+1],a[1],1000/(t/n) }" OptiScaler.log
echo "$LABEL: window $T1..$T2" >&2
exit 0
