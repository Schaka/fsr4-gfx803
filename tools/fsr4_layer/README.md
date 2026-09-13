# fsr4_layer: a Vulkan layer that swaps FSR4's shaders for tuned ones

The layer sits under the game. It hashes every SPIR-V module the application creates. If a
replacement of that name sits in its cache, the layer hands that to the driver instead. The original
is used whenever the driver rejects a replacement, so a bad shader cannot break a game.

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

`FSR4_SET` takes `quality`, `balanced`, `speed` or `off`. The sets live in `sets/`, and `FSR4_SETS`
points somewhere else. `FSR4_DEBUG=1` prints one line per replaced shader.

By hand, without the wrapper:

    VK_LAYER_PATH=/path/to/layer VK_INSTANCE_LAYERS=VK_LAYER_FSR4_tune \
    VKD3D_CONFIG=pipeline_library_ignore_spirv <game>

`VKD3D_CONFIG=pipeline_library_ignore_spirv` matters. vkd3d-proton serves pipelines from its own
cache without handing SPIR-V to the driver, and the layer can only replace what it sees. The wrapper
sets it for you.

## Environment

| variable | values | meaning |
|---|---|---|
| `FSR4_LAYER_CACHE` | a directory | where replacements live. Default `~/.cache/fsr4_opt/spirv` |
| `FSR4_LAYER_DUMP` | a directory | write every module the game creates, named by its hash |
| `FSR4_LAYER_DEBUG` | `1` | print one line per module, and say when one is replaced |
| `DISABLE_FSR4_LAYER` | `1` | turn the layer off without removing it |

## Filling the cache

Run `../fsr4_tune/tune.py` to build and time the variants, then install a set:

    python3 ../fsr4_tune/install_layer.py <shader_dump_dir> <set_dir> ~/.cache/fsr4_opt/spirv

`install_layer.py` hashes the dumped original of each shader and stores the replacement under that
name. That is the bridge between the tuner's naming and what the layer sees.

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
