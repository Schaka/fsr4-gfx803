# FSR4 INT8 on Polaris, release RELEASE_TAG

Everything needed to run AMD's FSR4 upscaler on GCN4 cards, and to make it fast enough to use. The
cards are the RX 470, RX 480, RX 570 and RX 580.

    tar xzf fsr4-gfx803-RELEASE_TAG.tar.gz
    cd fsr4-gfx803-RELEASE_TAG
    ./install.sh

`install.sh` installs the driver under `~/.local/share/radv-fsr4` and builds the layer. It changes
nothing system-wide, and it prints the two steps it cannot do for you.

## What is in here

| directory | what |
|---|---|
| `radv/` | RADV from Mesa 26.2.2 with the `v_mad_i32_i24` patch, so an int8 multiply-add costs one instruction |
| `layer/` | the Vulkan layer, a launcher, and every measured shader set |
| `drirc/` | the driver option that enables fp16 on GFX8, which FSR4 needs |

Your normal Proton is used as it is. Nothing else is patched.

## Launching

    VK_DRIVER_FILES=$HOME/.local/share/radv-fsr4/radeon_icd.x86_64.json \
    FSR4_SET=balanced /path/to/layer/fsr4-run %command%

In Heroic, put `fsr4-run` in Settings, Advanced, Wrapper command, and add the variables in the same
panel.

`FSR4_SET` takes `lossless`, `quality`, `balanced` or `speed`. Nineteen sets ship, and any of them
can be named directly. `FSR4_SET=list` prints them, and `layer/SETS.md` says what each one costs and
how it looks.

## What the layer does

FSR4's upscaler spends almost all of its time on int8 multiply-accumulate chains.

The layer rewrites every packed dot product into four byte extracts, four 32-bit multiplies and three
adds. That is exact. The driver patch then issues one `v_mad_i32_i24` per multiply. This runs in
every set, including `off`, and it asks the device first, so it does nothing on a card with `dp4a`.

On top of that, a set replaces FSR4's network shaders with tuned ones. Packing quantizes the weights
of two output channels and puts them in one operand, so one `v_mad_i32_i24` serves both. Nothing is
dropped. Pruning removes the smallest weights and scales up the survivors. Every variant was timed on
the card and scored against the exact result on captured gameplay data.

Upscaler time only, at 1280x720 to 1920x1080 with FSR 4.1.1b:

| shaders | Vega 56 | RX 570 | picture |
|---|---:|---:|---|
| FSR4's own | 7.5 | about 14.5 | the reference |
| `lossless` | 7.5 | 14 | bit-identical |
| `quality` | 4.0 to 4.5 | 13 | hard to tell from stock |
| `balanced` | 3.5 | 11 | very close to stock |
| `speed` | 3 to 5 | 9.5 | visibly softer |

Whole frames on an RX 570 in Pragmata, with the patched driver in every row:

| layer | mean ms |
|---|---:|
| out of the process | 25.99 |
| dot product rewrite alone | 23.50 |
| rewrite plus `balanced` | 19.12 |

The output head is never rewritten. Its errors reach the history buffer and return frame after frame
as flicker, so every set leaves it alone.

## Notes

A set replaces the SPIR-V that vkd3d-proton compiled, so it only matches the Proton builds it was
measured against. On another build the layer replaces nothing and behaves like `FSR4_SET=off`. That
is harmless. `tools/fsr4_tune/make_keys.py` in the repository adds your build in one step, and
`tools/fsr4_tune` rebuilds the sets for your card.

A correct run shows `FSR31FeatureDx12` with `FSR4ModelSelection` in `OptiScaler.log`. If you see
`FSR2FeatureDx12_212`, FSR4 is not running at all.

`FSR4_DEBUG=1` makes the layer print one line per shader it replaces or rewrites.

Verify the download with `md5sum -c MD5SUMS`.
