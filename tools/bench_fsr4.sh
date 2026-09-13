#!/bin/bash
# Benchmark harness. Runs ON the gfx803 box. Prints a CSV row.
#   usage: bench_fsr4.sh <dot_mode> <skip_n> <seconds> [asm|stats]
# Emits: dot_mode,skip_n,frames,mean_ms,median_ms,min_ms,fps
# NOTE: never put the literal exe name in a command that also runs pkill -- pkill -f
# matches the remote shell's OWN command line and kills the script. Hence the split string.
MODE=${1:-mad16}; SKIP=${2:-1}; SECS=${3:-20}; EXTRA=$4
pkill -f 'Fidelity''FX_FSR.exe' 2>/dev/null; sleep 1
source "$(dirname "$0")/fsr4_env.sh"
cd "$FSR4_TESTDIR" || exit 1
# BOTH cache files must go, or shaders are reused and translation never runs.
rm -f vkd3d-proton.cache* OptiScaler.log
rm -rf "$WINEPREFIX/shadercache"
export FSR4_DOT_MODE=$MODE FSR4_SKIP_N=$SKIP
LOG=/data/tmp/bench_${MODE}_${SKIP}.log
# NOTE: Mesa caches compiled pipelines in ~/.cache/mesa_shader_cache. On a cache hit ACO
# never runs, so RADV_DEBUG=asm/shaderstats prints NOTHING. Must disable it for those runs.
[ "$EXTRA" = "asm" ]   && { export RADV_DEBUG=asm;          export MESA_SHADER_CACHE_DISABLE=true; }
[ "$EXTRA" = "stats" ] && { export RADV_DEBUG=shaderstats;  export MESA_SHADER_CACHE_DISABLE=true; }
nohup umu-run FidelityFX_FSR.exe > "$LOG" 2>&1 &
sleep "$SECS"
grep -a 'Frametime' OptiScaler.log 2>/dev/null | tail -60 | grep -oE '[0-9]+\.[0-9]+ ms' \
  | sort -n \
  | awk -v m="$MODE" -v s="$SKIP" '{a[NR]=$1; t+=$1}
      END{ if(NR) printf "%s,%s,%d,%.2f,%.2f,%.2f,%.1f\n", m, s, NR, t/NR, a[int(NR/2)+1], a[1], 1000/(t/NR) }'
pkill -f 'Fidelity''FX_FSR.exe' 2>/dev/null
exit 0
