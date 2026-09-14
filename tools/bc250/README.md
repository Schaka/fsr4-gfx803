# Building the BC-250 FSR4 DLL for GCN4

`daniel-h-0/bc250-fsr4-fork` rebuilds AMD's FSR4 upscaler DLL with its shaders rewritten by hand. It
targets the BC-250, which is gfx1013. One change makes it run on gfx803, and it is then faster than
stock FSR4 with no loss of picture quality.

`../../notes/BC250_ON_GFX803.md` has the measurements, and the list of approaches that did not work.

## What you need

* The fork, cloned anywhere.
* The SDK DLL it builds from. This repository carries it already, at
  `fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll`, and its sha256 matches the one the fork
  pins.
* `libdxcompiler.so` from DXC 1.9.2607, the release the fork pins. Get it from the
  `linux_dxc` archive of that release.

Check the toolchain first by reproducing the fork's published DLL with its own `build.py`. It should
come out byte for byte identical. Only then is a change to the sources worth anything.

## Building a working DLL

    cp -r <fork>/dll ./work
    python3 wave64_fix.py work/shaders
    cp build_variant.py work/
    cd work && python3 build_variant.py \
        --sdk  /path/to/fsr4_dlls/4.1.1-stock/amd_fidelityfx_upscaler_dx12.dll \
        --dxcompiler /path/to/libdxcompiler.so \
        --output /tmp/out

`wave64_fix.py` removes the wave size requirement, which is the change that matters. Without it
vkd3d-proton refuses every model pipeline. The network then runs four passes per frame instead of
about twenty six, and says nothing.

`build_variant.py` is needed because the fork's own `build.py` refuses modified sources by design.
That script exists to prove the published DLL reproduces. This one does the same work and takes the
sources as they are, keeping every structural check in the fork's repacker.

`SKIP_ENTRIES` keeps the SDK's own shader for a role instead of the fork's. `SKIP_ENTRIES=pass11` is
worth about 0.4 ms of frametime, because that is the one role where AMD's shader is faster here.

## Installing it

The fork wants OptiScaler 10.0.0-pre1 or newer, with `Dx12Upscaler=ffx`, `UpscalerIndex=0` and
`Fsr4ForceModel=2`. Put the built DLL at `OptiScaler/amd_fidelityfx_upscaler_dx12.dll` in the game
directory. `Fsr4ForceModel` takes 0 for no override, 1 for FP8 and 2 for INT8; a card without FP8
gets no picture at all on 1.

Set `PROTON_FSR4_UPGRADE=0`, or Proton replaces the DLL on every launch and silently undoes this.

## Checking it works

Run with the Vulkan layer's `FSR4_PROFILE=1`. Divide the dispatch rate by the frame rate. FSR4 runs a
few dozen network dispatches per frame, and a build that runs a handful is not upscaling, whatever
its frame rate says.

## prune_weights.py

Drops the smallest weights of each accumulator chain, by share of that chain's total magnitude, and
scales the rest to compensate. It is kept for the method and the measurements, not because the result is
usable. At any share that saves real time the picture is unstable around hair. Pruning removes a part
of every sum, which biases it, and a biased sum shifts the temporal accumulation rather than
averaging out between frames.
