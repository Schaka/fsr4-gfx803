# fsr4_layer: a Vulkan layer that makes FSR4 fast on GCN4

The layer sits under the game and does two things to the SPIR-V it sees on the way to the driver.

**It rewrites the packed dot products.** FSR4 drives every multiply-accumulate through DXIL's
`dot4add_i8packed`, and vkd3d-proton compiles that into one `OpSDot`. A card without a hardware instruction for it
makes the driver lower it in software. The layer instead expands each `OpSDot` into four
sign-extending byte extracts, four multiplies and three adds, all in 32 bits. The expansion is exact.
A signed 4x8 dot product is the sum of the four signed byte products. NIR then proves the operands
fit in 24 bits. ACO issues one `v_mad_i32_i24` per multiply, which is what the RADV patch in this
repository is for. The layer asks the device whether it accelerates the packed signed dot product.
On a card that answers yes, such as one with `dp4a`, the layer leaves the dot products alone.

**It swaps the network shaders.** The layer hashes every module the application creates. If the
chosen set holds a replacement of that name, the driver gets the replacement. If the driver rejects
a replacement, the original runs, so a bad shader cannot break a game.

It works on SPIR-V, so it covers any path that reaches Vulkan: vkd3d-proton, DXVK, or a native
Vulkan game. Nothing in the game directory and nothing in the FSR4 DLL is modified.

The shaders themselves come from `../fsr4_tune`, which rewrites FSR4's int8 multiply chains and times
every variant on your card.

## Build

    gcc -O2 -fPIC -shared -o libfsr4_layer.so fsr4_layer.c -lpthread

## Use it

`fsr4-run` sets everything and puts the layer in front of a game.

**Steam.** Launch options of the game:

    FSR4_SET=balanced /path/to/fsr4-run %command%

**Heroic.** Settings, Advanced, Wrapper command:

    /path/to/fsr4-run

Add `FSR4_SET=balanced` to the environment variables in the same panel.

`FSR4_SET` takes one of four aliases, `lossless`, `quality`, `balanced` or `speed`. It also takes
the name of any set in `sets/`. `off` loads the layer for the dot product rewrite and replaces no
shader. `none` keeps the layer out of the process. `FSR4_SET=list` prints every set, and
`../../docs/SETS.md` says what each one costs and how it looks.

By hand, without the wrapper:

    VK_LAYER_PATH=/path/to/layer VK_INSTANCE_LAYERS=VK_LAYER_FSR4_tune \
    VKD3D_CONFIG=pipeline_library_ignore_spirv <game>

`VKD3D_CONFIG=pipeline_library_ignore_spirv` matters. vkd3d-proton serves pipelines from its own
cache without handing SPIR-V to the driver, and the layer can only change what it sees. The wrapper
sets it for you.

## Environment

| variable | values | meaning |
|---|---|---|
| `FSR4_SET` | a set name or alias | which shaders to use. `off` and `none` are described above |
| `FSR4_SETS` | a directory | where the sets live. Default: the `sets/` next to `fsr4-run` |
| `FSR4_LAYER_CACHE` | a directory | a flat directory of replacements, instead of a set |
| `FSR4_LAYER_DUMP` | a directory | write every module the game creates, named by its hash |
| `FSR4_LAYER_DEBUG` | `1` | print one line per module, and say when one is replaced or rewritten |
| `FSR4_NO_SDOT_EXPAND` | `1` | leave the packed dot products alone |
| `DISABLE_FSR4_LAYER` | `1` | turn the layer off without removing it |

## How a set is matched

A set holds the SPIR-V that replaces what vkd3d-proton compiled. The layer therefore has to
recognise the build in front of it. Each file in a set carries the name of the DXIL blob it came
from. That name is a property of the FSR4 DLL, so it is the same everywhere. `sets/keys.txt` maps the
hash of the SPIR-V the layer sees to that name, one line per Proton build.

If a run replaces nothing, the build is not in `keys.txt`. `../fsr4_tune/make_keys.py` adds it from a
single `VKD3D_SHADER_DUMP_PATH` run. A set you build yourself with `../fsr4_tune/tune.py` needs none
of this. Its files carry the name of the SPIR-V the layer saw, which is what the layer looks for when
`keys.txt` has no line.

## What to expect

The gain depends on the card, so pick the set with your own eyes. Two measured examples, upscaler
time only, at 1280x720 to 1920x1080 with FSR 4.1.1b:

| set | Vega 56 | RX 570 |
|---|---|---|
| stock FSR4 | 7.5 | about 14.5 |
| weights baked, exact, no quality change | 7.5 | 14 |
| 5-bit packing on all network passes | 6 | 14.6 |
| per-pass mix, tight error budget | 4.0 to 4.5 | 13 |
| per-pass mix, medium budget | 3.5 | 11 |
| pruning at threshold 12 or 16 | 3 to 5 | 9.5 to 11 |

The Vega gains far more, because Polaris already runs FSR4's own code at one instruction per
multiply. Try several sets. The measured error cannot see flicker, and at equal measured error
pruning looks worse than quantization.

The dot product rewrite is worth having on its own. Whole frames on an RX 570 in Pragmata, with the
patched RADV throughout: 25.99 ms with the layer out of the process, 23.50 ms with the rewrite and no
shader replaced, and 19.12 ms with the `balanced` set on top.
