# FSR4 INT8 on Polaris, release RELEASE_TAG

Everything needed to run AMD's FSR4 upscaler on GCN4 cards, and to make it fast enough to use. The
cards are the RX 470, RX 480, RX 570 and RX 580.

    tar xzf fsr4-gfx803-RELEASE_TAG.tar.gz
    cd fsr4-gfx803-RELEASE_TAG
    ./install.sh

`install.sh` installs the driver under `~/.local/share/radv-fsr4`. It changes nothing system-wide, and
it prints the two steps it cannot do for you.

The driver and the Vulkan layer both ship built, so nothing is compiled on your machine. Both need
glibc 2.38 or newer. If your distribution is older, `install.sh` says so and rebuilds the layer, and
that needs gcc and the Vulkan headers:

```bash
sudo apt install build-essential libvulkan-dev   # Ubuntu, Debian
sudo dnf install gcc vulkan-headers              # Fedora
sudo pacman -S gcc vulkan-headers                # Arch
```

The driver itself cannot be rebuilt from this archive. To build it, see the repository README.

## Choose a DLL, then a tier

FSR4's upscaler is one DLL, and this release gives you three of them. `layer/DLLS.md` describes each
in full. The short version:

* `stock` is AMD's own DLL. Nothing to build, works with OptiScaler 0.9.4, slowest of the three.
* `bc250` is the `daniel-h-0/bc250-fsr4-fork` rebuild at `v4.0.0-rc10`, made to run on GCN4. It
  approximates nothing, and where its result differs from AMD's it is more accurate. Needs
  OptiScaler 10.0.0-pre1.
* `hybrid` is that rebuild with ten model passes handed back to AMD's shaders, so the tuned sets can
  replace them. Fastest, and the only one where all four tiers do something.

All three come as a separate download, `fsr4-gfx803-RELEASE_TAG-dlls.tar.gz`. You can also build
the two rebuilt ones yourself: see `bc250/README.md`, and `bc250/REDOING_THE_HYBRID.md` for redoing
the hybrid against a future version of the fork.

Each DLL takes the same four tier names, and maps each to its own best shader set:

    VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
    PROTON_FSR4_UPGRADE=0 FSR4_DLL=hybrid FSR4_SET=balanced \
    /path/to/layer/fsr4-run %command%

In Heroic, put `fsr4-run` in Settings, Advanced, Wrapper command, and add the variables in the same
panel.

`FSR4_SET=list` prints the tiers for whichever `FSR4_DLL` you named, plus every set by its own name.
`layer/SETS.md` says what each set costs and how it was made.

Set `PROTON_FSR4_UPGRADE=0`, or Proton replaces the FSR4 DLL on every launch and quietly undoes
whichever one you installed.

## What each costs

Frametime and upscaler GPU time on an RX 570 in Pragmata, 1280x720 upscaled to 1920x1080, FSR 4.1.1b,
one scene, two cycles of each row in one interleaved batch. `layer/DLLS.md` explains the three DLLs.

| DLL | tier | frametime ms | fps | upscaler ms |
|---|---|---:|---:|---:|
| `stock` | none | 17.41 | 57.5 | 15.23 |
| `stock` | `lossless` | 17.01 | 58.8 | 14.86 |
| `stock` | `quality` | 14.78 | 67.7 | 12.73 |
| `stock` | `balanced` | 13.07 | 76.5 | 11.09 |
| `stock` | `speed` | 10.89 | 91.8 | 8.99 |
| `bc250` | `lossless` | 15.04 | 66.5 | 12.84 |
| `hybrid` | `lossless` | 14.84 | 67.4 | 12.56 |
| `hybrid` | `quality` | 12.88 | 77.7 | 10.83 |
| `hybrid` | `balanced` | 11.31 | 88.4 | 9.14 |
| `hybrid` | `speed` | 9.62 | 104.0 | 7.53 |

The `balanced` row uses `fin25`. The tier ships `fin25_pack39`, which is `fin25` with a pass9 shader
added, and that is a further 0.19 ms: 11.12 ms against 11.31 ms.

Read the differences, not the absolutes: your scene, your card and your resolution move the whole
table. Running the same ten rows with the GPU timestamps switched off moved no frametime by more
than 0.05 ms, so the cost of measuring is not in these numbers.

## What the layer does

FSR4's upscaler spends almost all of its time on int8 multiply-accumulate chains.

The layer rewrites every packed dot product into four byte extracts, four 32-bit multiplies and three
adds. That is exact. The driver patch then issues one `v_mad_i32_i24` per multiply. This runs on
every DLL and every tier, including `off`, and it asks the device first, so it does nothing on a card
with `dp4a`.

On top of that, a set replaces FSR4's network shaders with tuned ones. Packing quantizes the weights
of two output channels and puts them in one operand, so one `v_mad_i32_i24` serves both. Nothing is
dropped. Pruning removes the smallest weights and scales up the survivors. Every variant was timed on
the card and scored against the exact result on captured gameplay data.

The output head is never rewritten. Its errors reach the history buffer and return frame after frame
as flicker, so every set leaves it alone.

## Checking it works

A correct run on the stock DLL shows `FSR31FeatureDx12` with `FSR4ModelSelection` in
`OptiScaler.log`. If you see `FSR2FeatureDx12_212`, FSR4 is not running at all.

`FSR4_DEBUG=1` makes the layer print one line per shader it replaces or rewrites. `FSR4_PROFILE=1`
reports the GPU time the network really costs, and the dispatch rate with it. FSR4 runs a few dozen
network dispatches per frame, and a build that runs a handful is not upscaling, whatever its frame
rate says.

Verify the download with `md5sum -c MD5SUMS`.
