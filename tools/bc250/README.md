# Building the BC-250 FSR4 DLL for GCN4

[`daniel-h-0/bc250-fsr4-fork`](https://github.com/daniel-h-0/bc250-fsr4-fork) rebuilds AMD's FSR4
upscaler DLL with its shaders rewritten by hand. It targets the BC-250, which is gfx1013. Two
changes make it run well on gfx803, and it is then faster than stock FSR4 with no loss of picture
quality. Everything in this directory sits on top of that project: it wrote the shaders, and it
made them verifiable by proving its DLL reproduces byte for byte. If you have a BC-250, use theirs
directly rather than any of this.

`../../notes/BC250_ON_GFX803.md` has the measurements, and the list of approaches that did not work.
`hybrid.md` describes the faster arrangement, which combines this build with the tuned shader sets.

## What you need

* The fork at tag `v4.0.0-rc10`, cloned anywhere.
* The SDK DLL it builds from. This repository carries it already, at
  `fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll`, and its sha256 matches the one the fork
  pins.
* `libdxcompiler.so` from DXC 1.9.2607, the release the fork pins. Get it from the
  `linux_dxc` archive of that release.

Check the toolchain first by reproducing the fork's published DLL with its own `build.py`. It comes
out byte for byte identical. Only then is a change to the sources worth anything.

## Building a working DLL

    cp -r <fork>/dll ./work
    python3 wave64_fix.py work/shaders
    python3 fp32_prepass.py work/shaders
    cp build_variant.py work/
    cd work && python3 build_variant.py \
        --sdk  /path/to/fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll \
        --dxcompiler /path/to/libdxcompiler.so \
        --output /tmp/out

Run the two scripts in that order. Each one reports what it changed, and each leaves a shader it
does not fully understand untouched rather than half converted.

`wave64_fix.py` removes the wave size requirement. This is the change that decides whether anything
works at all. Without it vkd3d-proton refuses every model pipeline, the network runs four passes per
frame instead of about twenty six, and nothing says so.

`fp32_prepass.py` moves the prepass off 16-bit floats. GCN4 has no packed 16-bit arithmetic, so the
fork's packed form scalarises and each multiply-accumulate costs two instructions instead of one.

`build_variant.py` is needed because the fork's own `build.py` refuses modified sources by design.
That script exists to prove the published DLL reproduces. This one does the same work and takes the
sources as they are, keeping every structural check in the fork's repacker.

`SKIP_ENTRIES` keeps the SDK's own shader for a role instead of the fork's. It is what makes the
hybrid possible, and `hybrid.md` gives the list that wins.

## Installing it

The fork wants OptiScaler 10.0.0-pre1 or newer, with `Dx12Upscaler=ffx`, `UpscalerIndex=0` and
`Fsr4ForceModel=2`. Put the built DLL at `OptiScaler/amd_fidelityfx_upscaler_dx12.dll` in the game
directory. `Fsr4ForceModel` takes 0 for no override, 1 for FP8 and 2 for INT8. A card without FP8
gets no picture at all on 1.

Set `PROTON_FSR4_UPGRADE=0`, or Proton replaces the DLL on every launch and silently undoes this.

## Checking it works

Run with the Vulkan layer's `FSR4_PROFILE=1`. Divide the dispatch rate by the frame rate. FSR4 runs
a few dozen network dispatches per frame, and a build that runs a handful is not upscaling, whatever
its frame rate says.

## int24_postpass.py

Moves the postpass, and every other shader built the same way, from floating point back to integers.
Their weights are whole numbers and their activations are signed bytes, so the sums are integer
arithmetic written in floating point. In integer form the RADV patch in this repository issues one
`v_mad_i32_i24` for each multiply-accumulate.

It is kept for the method and the measurements. It is slower, by about 1.1 ms of frametime in both
the lossless and the balanced arrangement, so the build recipe above leaves it out. The float form
already costs one instruction per multiply-accumulate, because `v_mac_f32` takes its weight as a
32-bit literal. `v_mad_i32_i24` cannot: it is a VOP3 instruction, and GCN4 allows no literal there.
Only a quarter of the weights fall outside the inline constant range of -16 to 64, but loading those
costs more than the byte extracts the change saves.

## prune_weights.py

Drops the smallest weights of each accumulator chain, by share of that chain's total magnitude, and
scales the rest to compensate. It is kept for the method and the measurements, not because the
result is usable. At any share that saves real time the picture is unstable around hair. Pruning
removes a part of every sum, which biases it, and a biased sum shifts the temporal accumulation
rather than averaging out between frames.
