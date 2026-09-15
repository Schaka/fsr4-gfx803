#!/bin/bash
# One measured run of Pragmata with a chosen upscaler DLL and shader set.
#
#   run_one.sh <dll path> <tag> <driver icd> <set> [measure seconds]
#
# It differs from a fixed sleep in two ways that matter. It waits for the game to actually render
# before it starts counting, so a slow load does not eat the measurement window, and it averages
# only the frames inside that window, so the loading screen and the shader compilation do not
# dilute the result. Every stage has a deadline, so a hung run ends by itself and reports a
# failure rather than blocking the queue.
set -u
DLL=$1; TAG=$2; DRV=$3; SET=$4; SECS=${5:-40}; OPTI=${6:-10}
# Extra environment for one sweep, such as turning the dot product rewrite off.
SWEEP_ENV=${SWEEP_ENV:-}
# PROF=0 turns the GPU timestamps off, so a frametime carries no measurement cost.
PROF=${PROF:-1}
R=/data/tmp/fsr4re
L=/home/user/.local/share/fsr4
G="/home/user/Games/Heroic/Prefixes/Pragmata/drive_c/Program Files (x86)/Pragmata"
FAIL () { echo "$TAG,frametime=,fps=,ms_per_s=,status=$1"; cleanup; exit 0; }
# Ask the game to close, and only force it if it refuses. Killing it outright leaves the game's
# shader cache and vkd3d-proton's pipeline cache half written, and a few hundred runs of that put
# the machine into a state where the upscaler renders black until a reboot.
cleanup () {
  pkill -f "PRAGMA""TA.exe" 2>/dev/null
  for _ in $(seq 1 15); do
    pgrep -f "PRAGMA""TA.exe" > /dev/null || break
    sleep 1
  done
  if pgrep -f "PRAGMA""TA.exe" > /dev/null; then
    echo "game did not exit in 15s, forcing it" >&2
    pkill -9 -f "PRAGMA""TA.exe" 2>/dev/null
    sleep 2
  fi
  pkill -x wineserver 2>/dev/null
  sleep 2
  pkill -9 -x wineserver 2>/dev/null
  pkill -x -9 sway 2>/dev/null
  sleep 1
}

[ -f "$DLL" ] || FAIL "no-dll"
bash $R/use_opti.sh "$OPTI" > /dev/null
# The two OptiScaler versions keep the upscaler DLL in different places.
if [ "$OPTI" = 10 ]; then
  cp "$DLL" "$G/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" || FAIL "copy"
else
  cp "$DLL" "$G/amd_fidelityfx_upscaler_dx12.dll" || FAIL "copy"
fi
cleanup

export XDG_RUNTIME_DIR=/run/user/1000 WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
printf "output HEADLESS-1 resolution 1920x1080\nxwayland enable\n" > /data/tmp/swaycfg/hl.cfg
nohup sway -c /data/tmp/swaycfg/hl.cfg > /data/tmp/sway_hl.log 2>&1 &
sleep 8
export WAYLAND_DISPLAY=$(cd /run/user/1000 && ls wayland-[0-9] 2>/dev/null | head -1)
[ -n "${WAYLAND_DISPLAY:-}" ] || FAIL "no-wayland"
export WINEPREFIX=/home/user/Games/Heroic/Prefixes/Pragmata
export PROTONPATH=/home/user/.config/heroic/tools/proton/proton-cachyos-11.0-20260703-slr
export GAMEID=0 MESA_VK_DEVICE_SELECT=1002:67df
export PROTON_FSR4_UPGRADE=0 PROTON_USE_OPTISCALER=0 PROTON_USE_XALIA=0
export WINEDLLOVERRIDES="dxgi=n,b;amdxcffx64="
export VK_DRIVER_FILES=$DRV
P=$PROTONPATH/files/lib/wine/vkd3d-proton/x86_64-windows
H=$WINEPREFIX/drive_c
for f in d3d12core.dll d3d12.dll; do cp "$R/stock_$f" "$P/$f"; cp "$R/stock_$f" "$H/windows/system32/$f"; done
cd "$G" || FAIL "no-game-dir"
rm -f OptiScaler.log

# The layer turns profiling on when the variable exists at all, whatever its value, so PROF=0
# has to leave the variables unset rather than set them to zero.
PROFENV=""
[ "$PROF" = 1 ] && PROFENV="FSR4_PROFILE=1 FSR4_PROFILE_EACH=1 FSR4_PROFILE_EVERY=30"
# shellcheck disable=SC2086
env ${SWEEP_ENV:-IGNORE=1} $PROFENV FSR4_SET=$SET FSR4_SETS=$L/sets FSR4_LAYER=$L FSR4_DEBUG=1 \
  nohup $L/fsr4-run umu-run "PRAGMATA.exe" > /data/tmp/v_$TAG.log 2>&1 &

# Stage one: wait until the game renders at all. A cold shader cache makes this slow.
for i in $(seq 1 60); do
  [ "$(grep -ac Frametime OptiScaler.log 2>/dev/null)" -ge 200 ] && break
  sleep 5
done
[ "$(grep -ac Frametime OptiScaler.log 2>/dev/null)" -ge 200 ] || FAIL "never-rendered"

# Stage two: let the clocks and the pipeline cache settle before the window opens.
sleep 45
MARK=$(grep -ac Frametime OptiScaler.log)
sleep "$SECS"
END=$(grep -ac Frametime OptiScaler.log)
[ "$END" -gt "$((MARK + 100))" ] || FAIL "stalled"

# Average only the frames inside the window.
READ=$(grep -a Frametime OptiScaler.log | sed -n "$((MARK + 1)),${END}p" |
  gawk '{if(match($0,/Frametime: [0-9]+\.[0-9]+/)){v=substr($0,RSTART+11,RLENGTH-11)+0;n++;t+=v}}
        END{if(n)printf "%.2f %.3f %d", t/n, 1000/(t/n), n}')
FT=$(echo "$READ" | cut -d" " -f1)
FPS=$(echo "$READ" | cut -d" " -f2)
N=$(echo "$READ" | cut -d" " -f3)
MSPS=$(grep -a "upscaler .* ms/s" /data/tmp/v_$TAG.log | tail -3 |
       gawk '{s+=$3;n++} END{if(n)printf "%.1f",s/n}')
echo "$TAG,frametime=${FT:-},fps=${FPS:-},ms_per_s=${MSPS:-},frames=${N:-0},status=ok"
cleanup
