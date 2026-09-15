# The hybrid: the fork where it wins, our tuned shaders everywhere else

The BC-250 build and the shader sets in this repository optimise different parts of FSR4, and they
combine. The result is faster than either alone.

## Why they combine

The fork replaces all 348 shaders. Where it replaces one, our sets no longer match it, because a set
is keyed to the SPIR-V the stock DLL compiles. `SKIP_ENTRIES` decides which roles the build leaves
alone. The SDK's own shader then stands there, which is what our sets are tuned for.

The split that wins keeps the fork's prepass, postpass, pass6 and pass7, and leaves the ten other
model passes to AMD's shaders:

    SKIP_ENTRIES=pass1,pass2,pass3,pass4,pass5,pass8,pass9,pass10,pass11,pass12

The postpass is why this is worth doing at all. Giving that one role back to AMD costs 1.5 ms of
frametime, on the lossless and the balanced tier alike. The prepass is worth 0.05 ms, which is at
the edge of what these runs resolve. Handing pass6 and pass7 to our sets as well is slower, and so
is letting the fork keep pass9: 11.42 ms against 11.32 ms.

## Building it

    python3 wave64_fix.py work/shaders          # required, or nothing upscales
    python3 fp32_prepass.py work/shaders        # the prepass in fp32 rather than fp16
    cd work && SKIP_ENTRIES=pass1,pass2,pass3,pass4,pass5,pass8,pass9,pass10,pass11,pass12 \
        python3 build_variant.py --sdk <pinned SDK dll> --dxcompiler <libdxcompiler.so> --output <dir>

Then run the game with `FSR4_DLL=hybrid` and a tier, for example `FSR4_SET=balanced`.

## The hole the tiers had

pass9 is the most expensive shader in the whole pipeline, and the `fin25` set has no shader for it,
so it ran as AMD wrote it while every other role was replaced. The `pack3` set does have one, and
grafting it on is worth 0.14 ms of frametime. The shipped `fin25_pack39` set is `fin25` with that one
shader added.

Which pass9 shader matters more than it should. The 3-bit packing beats the 4-bit by 0.21 ms and the
5-bit by 0.66 ms, in that order and with no exception, even though coarser packing is the less
accurate one. Fewer bits in a weight means more of them fit the inline constant range the vector
instructions encode directly, so the code is both shorter and smaller, and this shader is over half a
megabyte of SPIR-V.

## The pure build is not set-proof either

The fork's DLL serves AMD's own pass11 at runtime, whichever way it is built, so a set replaces that
one shader even there. It is worth 0.21 ms. That is why `aliases.bc250` names a real set rather than
`off`.
