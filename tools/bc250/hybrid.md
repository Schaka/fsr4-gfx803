# The hybrid: BC-250 where it wins, our tuned shaders everywhere else

The BC-250 build and the shader sets in this repository optimise different parts of FSR4, and they
can be combined. The result is faster than either on its own.

## Why they combine

The BC-250 build replaces 348 shaders. Where it replaces one, our sets no longer match it. A set is
keyed to the SPIR-V the stock DLL compiles. `SKIP_ENTRIES` decides which roles the build leaves
alone. The SDK's own shader then stands there, which is what our sets are tuned for.

The split that works keeps BC-250 for the prepass, the postpass, pass6 and pass7, where it clearly
wins. It leaves the SDK's own shaders for the rest, so our sets can replace them.

    SKIP_ENTRIES=pass1,pass2,pass3,pass4,pass5,pass8,pass9,pass10,pass11,pass12

Nine of our tuned shaders then take effect, against one on the unmodified BC-250 build.

## What it measures

Upscaler GPU time and frametime on an RX 570 in Pragmata, 1280x720 to 1920x1080. Each row is the
mean of two scored runs after a discarded warm-up. The first run after a change compiles shaders and
is not representative.

| build | set | upscaler ms | frametime ms | fps |
|---|---|---:|---:|---:|
| FSR4 4.1.1b | none | 15.18 | 21.08 | 47.4 |
| BC-250, all its own shaders | none | 12.4 | 18.3 | 54.6 |
| hybrid | `balanced` | 9.7 | 15.5 | 64.5 |
| hybrid | `speed` | 8.2 | 13.9 | 72.0 |

The hybrid with `balanced` is 2.6 ms of upscaler time and 2.8 ms of frametime better than the BC-250
build alone. It beats every earlier result on either path.

## Building it

    python3 wave64_fix.py work/shaders          # required, or nothing upscales
    python3 fp32_prepass.py work/shaders        # the prepass in fp32 rather than fp16
    cd work && SKIP_ENTRIES=pass1,pass2,pass3,pass4,pass5,pass8,pass9,pass10,pass11,pass12 \
        python3 build_variant.py --sdk <pinned SDK dll> --dxcompiler <libdxcompiler.so> --output <dir>

Then run the game with the layer and a set, for example `FSR4_SET=balanced`.
