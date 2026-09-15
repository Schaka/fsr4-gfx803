# Redoing the hybrid against a new fork release

The hybrid is not a patch you can rebase. It is a split between two sets of shaders, and the split
was chosen by measurement. When `daniel-h-0/bc250-fsr4-fork` publishes a new version, the split has
to be re-derived, because the fork may have changed the very shaders the split was drawn around.

This is the procedure. It takes about four hours of machine time, most of it unattended.

## What you need

* The new fork release, cloned.
* The SDK DLL it pins. This repository carries `fsr4_dlls/4.1.1-stock/`, and the fork's
  `manifest.json` names the sha256 it expects. If they no longer match, the fork moved to a new SDK
  and everything below still works, but the shader sets no longer will: they are keyed to the
  SPIR-V that one SDK DLL compiles. See "When the SDK DLL changes" at the end.
* `libdxcompiler.so` from the DXC release the fork pins.
* A gfx803 card, the patched RADV from `radv/`, and a game that runs FSR4. Everything here was done
  in Pragmata at 1280x720 upscaled to 1920x1080.

## Step 0: prove the toolchain before changing anything

    cd <fork>/dll && python3 build.py --sdk <pinned SDK dll> --dxcompiler <libdxcompiler.so> ...

It must reproduce the fork's published DLL byte for byte. Until it does, no measurement below means
anything, because you cannot tell your change from a toolchain difference.

## Step 1: check whether the GCN4 fixes still apply

Two scripts in this directory fix the fork for GCN4. Neither is guaranteed to be needed forever, and
each one reports what it changed, so run them and read the count.

    python3 wave64_fix.py work/shaders      # must report a non-zero count
    python3 fp32_prepass.py work/shaders

`wave64_fix.py` is the one that decides whether anything works at all. The fork's model passes ask
for a wave of 32 lanes, GCN4 has only 64, and vkd3d-proton then refuses every model pipeline without
saying so. If it reports zero changes, check by hand whether the wave size metadata is still there:

    grep -c 'i32 11, !\|i32 23, !' work/shaders/<a pass shader>.ll

A zero here means the fork dropped the requirement itself, which is good news. A zero because the
metadata moved to a shape the script does not match is a silent disaster, so look before believing.

`fp32_prepass.py` moves the prepass off 16-bit floats. It was worth 0.05 ms, which is small enough
that it is worth re-measuring rather than assuming.

## Step 2: build the full DLL and prove it upscales

    cd work && python3 build_variant.py --sdk <sdk> --dxcompiler <dxc> --output /tmp/full

`build_variant.py` exists because the fork's own `build.py` refuses modified sources by design.

Then run the game with `FSR4_PROFILE=1` and divide the dispatch rate by the frame rate. FSR4 runs a
few dozen network dispatches per frame. A build that runs a handful is not upscaling, whatever its
frame rate says, and that is exactly the failure the wave size causes.

## Step 3: find which roles a shader set can still reach

A set replaces SPIR-V by hash, so it can only touch a shader the fork has not replaced. The layer
prints what it sees:

    FSR4_DEBUG=1 ... 2>&1 | grep -c ' -> replaced'

Run this on the full build with a tier named. On `v4.0.0-rc10` the answer was one: the fork's DLL
still serves AMD's own pass11. If the new release replaces that too, the full build has no tiers at
all, and if it leaves more alone, there are more roles to play with.

## Step 4: derive the split by measuring one role at a time

`SKIP_ENTRIES` keeps the SDK's own shader for a role instead of the fork's, which is what lets a
tuned set stand there. Build one DLL per role, each letting the fork keep exactly one model pass:

    for keep in pass1 pass2 pass3 pass4 pass5 pass8 pass9 pass10 pass11 pass12; do
        skip=$(echo $ALL | tr ' ' '\n' | grep -vx "$keep" | paste -sd,)
        SKIP_ENTRIES=$skip python3 build_variant.py ... --output /tmp/role_$keep
    done

Then measure them against each other with `../bench/sweep.sh`, all in one interleaved batch. Each
row says what the fork's version of that one pass is worth against AMD's plus a tuned set.

Do the same for the prepass, the postpass, pass6 and pass7 by adding each to the skip list in turn.
On rc10 the postpass was the whole reason to do this, at 1.5 ms, and the prepass was worth 0.05 ms.

The winning split keeps the fork's shader wherever the fork wins and gives the rest to AMD:

    SKIP_ENTRIES=pass1,pass2,pass3,pass4,pass5,pass8,pass9,pass10,pass11,pass12

## Step 5: re-derive the tiers

The fastest set is not the same on every DLL, so each one carries its own table in
`../fsr4_layer/sets/aliases.<dll>`. Measure the candidates for each tier in one batch and write the
winner in. Two results from rc10 that are worth checking again rather than assuming:

* The tiers had no pass9 shader at all, so the most expensive shader in the pipeline ran exactly as
  AMD wrote it while every other role was replaced. Check every tier for a gap like that.
* A coarser set can be the faster one. Between the packing variants of that pass9 shader the 3-bit
  beat the 4-bit by 0.21 ms and the 5-bit by 0.66 ms, because fewer bits in a weight means more of
  them fit the inline constant range the vector instructions encode directly.

## Measuring without fooling yourself

Five things cost real time here before they were understood.

**Interleave, never repeat.** Run the whole list once, then again. Measuring one configuration twice
in a row hides drift, because anything that changes slowly between batches lands entirely on one
configuration. `sweep.sh` does this.

**Throw away the first measured cycle.** A DLL that has just changed can still be compiling shaders
during it. That once showed up as a 2 ms error on a configuration whose later cycles agreed to
0.01 ms, and it was nearly published. `../fsr4_tune/table_from_sweeps.py` drops it by default.

**Only compare rows from the same batch.** Rows inside one interleaved batch are trustworthy to
about 0.03 ms. Between batches, do not assume better than a few tenths.

**Measure the window, not the run.** Averaging every frame since launch buries the difference under
the loading screen. `run_one.sh` waits for the game to render, lets it settle, and then averages
only the frames inside its window.

**The picture is not the number.** Every set here was ranked by speed and by a measured error
against the exact result, and neither predicted what a person sees. Pruning at a measured error that
looks fine still makes hair unstable, because it biases every sum rather than spreading an unbiased
error. Look at the thing before shipping it.

## When the SDK DLL changes

The shader sets in `../fsr4_layer/sets/` replace SPIR-V by hash, so they are keyed to one SDK DLL
compiled by one vkd3d-proton build. If the fork moves to a new SDK DLL, every set stops matching and
the hybrid loses its point until the sets are rebuilt. `../fsr4_tune/` does that, and
`../fsr4_tune/make_keys.py` teaches the existing sets a new vkd3d-proton build, which is the cheaper
half of the problem.
