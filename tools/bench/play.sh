#!/bin/bash
# Start Pragmata on the screen with one upscaler DLL and one quality tier, to look at.
#
#   play.sh <dll> <tier>
#
#     dll    stock   AMD's own FSR4 4.1.1b DLL, under OptiScaler 0.9.4
#            bc250   the fork's rc10 build rebuilt for GCN4, under OptiScaler 10
#            hybrid  that build with ten model passes left to AMD's shaders, under OptiScaler 10
#     tier   lossless, quality, balanced or speed, or any set by its own name, or off, or none
#
#   play.sh list            print the tiers each DLL has, and every set
#   play.sh <dll> list      the same, for one DLL
#
# Quit the game with Alt+Shift+E, which also ends the sway session.
set -u
DLL=${1:-hybrid}
TIER=${2:-balanced}
# FSR4_LAYER_DIR lets a different build of the layer be tried, to tell a layer fault from a
# game fault. /data/tmp/fsr4re/layer_old holds the build from before the last set of changes.
L=${FSR4_LAYER_DIR:-/home/user/.local/share/fsr4}
. /data/tmp/fsr4re/gpu_env.sh
R=/data/tmp/fsr4re
H=/home/user/Games/Heroic/Prefixes/Pragmata/drive_c
G="$H/Program Files (x86)/Pragmata"
P=/home/user/.config/heroic/tools/proton/proton-cachyos-11.0-20260703-slr/files/lib/wine/vkd3d-proton/x86_64-windows

if [ "$DLL" = list ]; then
    for d in stock bc250 hybrid; do
        echo "== $d"
        FSR4_DLL=$d FSR4_SET=list FSR4_SETS=$L/sets $L/fsr4-run 2>&1 | sed 's/^/   /'
    done
    exit 0
fi
if [ "$TIER" = list ]; then
    FSR4_DLL=$DLL FSR4_SET=list FSR4_SETS=$L/sets $L/fsr4-run
    exit 0
fi

# Which DLL file, and which OptiScaler it needs. The two versions keep the upscaler DLL in
# different places, and they need different Wine overrides.
#
# The rebuilt DLLs reach the game through OptiScaler's FFX path, which only takes over when the
# real amdxcffx64 is disabled. Leaving that override out renders the menus and then a black frame,
# because the upscaler runs and writes nothing anyone sees.
case $DLL in
  stock)  FILE=$R/stock_411b_upscaler.dll;   OPTI=094; OVERRIDES="dxgi=n" ;;
  bc250)  FILE=$R/bc250_full_upscaler.dll;   OPTI=10;  OVERRIDES="dxgi=n,b;amdxcffx64=" ;;
  hybrid) FILE=$R/bc250_hybrid_upscaler.dll; OPTI=10;  OVERRIDES="dxgi=n,b;amdxcffx64=" ;;
  *) echo "play.sh: unknown DLL '$DLL'. Use stock, bc250 or hybrid." >&2; exit 1 ;;
esac
[ -f "$FILE" ] || { echo "play.sh: $FILE is missing" >&2; exit 1; }

# A launch left behind by an earlier attempt holds the prefix, and the next one then waits forever
# with nothing on screen. Clear it out before starting.
if pgrep -x sway > /dev/null || pgrep -f "PRAGMA""TA.exe" > /dev/null; then
    echo "clearing a previous session" >&2
    pkill -f "PRAGMA""TA.exe" 2>/dev/null
    pkill -f umu-shim 2>/dev/null
    pkill -f wineserver 2>/dev/null
    pkill -x sway 2>/dev/null
    sleep 4
    pkill -9 -f "PRAGMA""TA.exe" 2>/dev/null
    pkill -9 -x sway 2>/dev/null
    sleep 2
fi

bash $R/use_opti.sh "$OPTI" >&2
if [ "$OPTI" = 10 ]; then
    cp "$FILE" "$G/OptiScaler/amd_fidelityfx_upscaler_dx12.dll"
    # Benchmarking swaps these, so put the stock pair back rather than trusting what is there.
    for f in d3d12core.dll d3d12.dll; do
        cp "$R/stock_$f" "$P/$f"
        cp "$R/stock_$f" "$H/windows/system32/$f"
    done
else
    cp "$FILE" "$G/amd_fidelityfx_upscaler_dx12.dll"
fi
echo "dll $DLL ($(md5sum "$FILE" | cut -c1-8)), tier $TIER, OptiScaler $OPTI, on $GPU_NAME" >&2

rm -rf "$H/ovr"
LOG=/data/tmp/play_${DLL}_${TIER}.log
cat > $R/run_play.sh <<EOS
#!/bin/bash
export WINEPREFIX=/home/user/Games/Heroic/Prefixes/Pragmata
export PROTONPATH=/home/user/.config/heroic/tools/proton/proton-cachyos-11.0-20260703-slr
export GAMEID=0 MESA_VK_DEVICE_SELECT=$GPU_PCI
export PROTON_FSR4_UPGRADE=0 PROTON_USE_OPTISCALER=0 PROTON_USE_XALIA=0
export WINEDLLOVERRIDES="$OVERRIDES"
export VK_DRIVER_FILES=/data/radv_262/patched/radeon_icd.x86_64.json
cd "$G" || exit 1
rm -f OptiScaler.log
FSR4_DLL=$DLL FSR4_SET=$TIER FSR4_SETS=$L/sets FSR4_LAYER=$L FSR4_DEBUG=1 \\
    $L/fsr4-run umu-run "PRAGMATA.exe" > $LOG 2>&1
swaymsg exit
EOS
chmod +x $R/run_play.sh

# gpu_env.sh decided this: the AMD card alone when a monitor is on it, otherwise the AMD card plus
# whichever card has one, so the compositor can copy across.
CARD=$DRM_DEVICES
printf "output * resolution 1920x1080\nxwayland enable\nexec %s/run_play.sh\nbindsym Mod1+Shift+e exit\n" "$R" > /data/tmp/swaycfg/play.cfg
export XDG_RUNTIME_DIR=/run/user/1000 WLR_DRM_DEVICES=$CARD
echo "sway on $CARD, log $LOG"
sway -c /data/tmp/swaycfg/play.cfg
