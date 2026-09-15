# The three upscaler DLLs

FSR4's upscaler is one DLL. This project gives you three of them, and the same four tier names work
on each. They differ in whose shaders run inside, so the same tier name maps to a different shader
set on each one. `FSR4_DLL` tells the launcher which one you installed.

    VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
    FSR4_DLL=hybrid FSR4_SET=balanced fsr4-run %command%

`install.sh` in the release archive writes that `radeon_icd.x86_64.json`, so run it first. It does
not exist before that.

Pick the DLL first, then the tier. `FSR4_SET=list` prints the tiers for whichever DLL you named.

## stock

AMD's own FSR4 4.1.1b DLL, unmodified. The Vulkan layer rewrites its packed dot products, and a
shader set replaces some of its network shaders.

Use it if you want the least moving parts, or if the other two misbehave in your game. It is the
only one that needs no extra download, and on every tier the three share it is the slowest.

It runs under OptiScaler 0.9.4 with `Dx12Upscaler=fsr31`, `UpscalerIndex=0`, `Fsr4Update=true` and
`Fsr4ForceEnableInt8=true`.

## bc250

The `daniel-h-0/bc250-fsr4-fork` DLL at `v4.0.0-rc10`, rebuilt to run on GCN4. Every one of its 348
shaders carries its weights as constants instead of reading them from a buffer, which is faster and
changes no result.

Use it if you want speed at no cost in quality. Nothing here approximates. Every shader does the
arithmetic AMD's does, and the one place the result differs it is more accurate, not less: the
prepass computes in 32-bit floats where AMD's uses 16-bit, which keeps 24 bits of each product
instead of 11. GCN4 runs both at the same rate, so that accuracy is free.

It replaces its own shaders for every role but one. pass11 still reaches the driver as AMD wrote it,
so a set replaces that one shader and nothing else. On this build that changes nothing worth
measuring: 15.01 ms of frametime against 15.00 ms. The three tiers are wired up so the names work,
and they give you a different pass11 shader, but do not expect them to do much here. `lossless`
leaves even that one alone, because the `exact` set has no pass11 shader to offer.

It runs under OptiScaler 10.0.0-pre1 or newer with `Dx12Upscaler=ffx`, `UpscalerIndex=0` and
`Fsr4ForceModel=2`.

## hybrid

The same rebuild, with ten of the twelve model passes left to AMD's own shaders on purpose. That
sounds backwards and it is the fastest arrangement measured. The fork clearly wins on the prepass,
the postpass, pass6 and pass7, and those it keeps. Everywhere else a tuned set beats it, and a set
can only replace a shader the fork has not already replaced.

Use it unless a game misbehaves with it. It is the only DLL where all four tiers do something, and
its `lossless` tier costs nothing in quality for the same reason `bc250` does not.

It runs under the same OptiScaler 10 configuration as `bc250`.

## What each costs

Frametime on an RX 570 in Pragmata, 1280x720 upscaled to 1920x1080, FSR 4.1.1b, on one scene. Every
row comes from the same batch, measured one after another and then again, so the differences between
rows are trustworthy even though the absolute numbers belong to that scene. Two cycles of each row,
which agreed to 0.03 ms.

| DLL | tier | shader set | frametime ms | fps | upscaler ms |
|---|---|---|---:|---:|---:|
| `stock` | none | `off` | 17.41 | 57.5 | 15.23 |
| `stock` | `lossless` | `exact` | 17.01 | 58.8 | 14.86 |
| `stock` | `quality` | `fin15` | 14.78 | 67.7 | 12.73 |
| `stock` | `balanced` | `fin25` | 13.07 | 76.5 | 11.09 |
| `stock` | `speed` | `prune16` | 10.89 | 91.8 | 8.99 |
| `bc250` | `lossless` | `off` | 15.04 | 66.5 | 12.84 |
| `hybrid` | `lossless` | `off` | 14.84 | 67.4 | 12.56 |
| `hybrid` | `quality` | `fin15` | 12.88 | 77.7 | 10.83 |
| `hybrid` | `balanced` | `fin25` | 11.31 | 88.4 | 9.14 |
| `hybrid` | `speed` | `prune16` | 9.62 | 104.0 | 7.53 |

The upscaler column needs GPU timestamps around every dispatch. Running the same ten rows again with
the timestamps switched off moved no frametime by more than 0.05 ms, so the cost of measuring is not
in these numbers.

The `balanced` row above uses `fin25`. The tier ships `fin25_pack39`, which is `fin25` with a pass9
shader added, and that is a further 0.19 ms: 11.12 ms against 11.31 ms. Side by side, plain `fin25`
is slightly the cleaner of the two, so name it directly if you would rather have the picture than
the 0.19 ms.

Read the differences, not the absolutes. Your scene, your card and your resolution all move the
whole table.

## Getting the DLLs

All three ship as a separate download, because they are large and most of the archive is useful
without them. Unpack it and copy the one you want, renamed to
`amd_fidelityfx_upscaler_dx12.dll`. `bc250` and `hybrid` go in the `OptiScaler/` folder of the game
directory; `stock` goes in the game directory itself, because OptiScaler 0.9.4 looks for it there.

You can also build either rebuilt DLL yourself from the fork plus AMD's SDK DLL. `bc250/README.md`
gives the steps and `bc250/hybrid.md` the one extra setting that makes the hybrid.
`bc250/REDOING_THE_HYBRID.md` is the procedure for re-deriving the split when the fork publishes a
new version, which has to be measured again rather than carried over.

Set `PROTON_FSR4_UPGRADE=0` whichever you use, or Proton replaces the DLL on every launch and
silently undoes your choice.

## Builds for other cards

`amd_fidelityfx_upscaler_dx12.vega.dll` and any other DLL here named after a card is a one-shot port:
measured once on that card, built once, and shipped as it stood. None of them is maintained, and
none applies to a GCN4 card. `other_archs/` in the main archive says what each one is, what it
costs and how it was built.

## Checking which one is running

`FSR4_DEBUG=1` makes the layer print one line per shader. Count the lines saying `replaced`: the
stock DLL with a tier replaces about eleven, the hybrid about ten, and `bc250` exactly one on any
tier but `lossless`, which replaces none.

`FSR4_PROFILE=1` reports the GPU time the network costs and the dispatch rate. FSR4 runs a few dozen
network dispatches per frame. A build that runs a handful is not upscaling, whatever its frame rate
says.
