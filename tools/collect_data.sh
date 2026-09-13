#!/bin/bash
# Full data collection sweep. Runs ON the gfx803 box, writes to /data/tmp/collected/.
# Pull the results to data/ with scp from the workstation.OUT=/data/tmp/collected; mkdir -p $OUT; cd "$(dirname "$0")" || exit 1
echo "dot_mode,skip_n,frames,mean_ms,median_ms,min_ms,fps" > $OUT/benchmarks.csv
for m in mad16 i16 i32 fp32; do ./bench_fsr4.sh $m 1 20 >> $OUT/benchmarks.csv; done
# occupancy / register / throughput stats for every shader
./bench_fsr4.sh mad16 1 20 stats >/dev/null
cp /data/tmp/bench_mad16_1.log $OUT/shaderstats_mad16.log
# full ISA for instruction-mix analysis
./bench_fsr4.sh mad16 1 20 asm >/dev/null
cp /data/tmp/bench_mad16_1.log $OUT/isa_mad16.log
./bench_fsr4.sh i16 1 20 asm >/dev/null
cp /data/tmp/bench_i16_1.log $OUT/isa_i16.log
# dispatch trace (shape + count per frame)
source ./fsr4_env.sh; cd "$FSR4_TESTDIR"
rm -f vkd3d-proton.cache* OptiScaler.log
VKD3D_DEBUG=err VKD3D_LOG_FILE=$OUT/dispatch_trace.log \
  timeout 20 umu-run FidelityFX_FSR.exe >/dev/null 2>&1
pkill -f 'Fidelity''FX_FSR.exe' 2>/dev/null
# SPIR-V for every shader the app compiles
rm -rf $OUT/spirv; mkdir -p $OUT/spirv
rm -f vkd3d-proton.cache*
VKD3D_SHADER_DUMP_PATH=Z:$OUT/spirv timeout 22 umu-run FidelityFX_FSR.exe >/dev/null 2>&1
pkill -f 'Fidelity''FX_FSR.exe' 2>/dev/null
echo "collected -> $OUT"; ls -la $OUT
