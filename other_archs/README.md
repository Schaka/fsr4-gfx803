# Other architectures

One-shot ports. Each folder here is a card that was measured once, built once, and shipped as it
stood on that day. None of them is maintained.

That is a deliberate limit rather than an apology. The answer for a card is a split between two sets
of shaders, and it is chosen by measurement on that card. When the upstream fork publishes a new
version the split has to be derived again, and there is no card here to derive it on any more. A
folder in here is a record of what won, with the recipe that produced it, so someone with the
hardware can redo the work rather than guess at it.

The main path in this repository, for GCN4, is maintained. These are not.

| folder | card | what it is |
|---|---|---|
| `vega56/` | Vega 56, gfx900 | the fork's build with two roles left to AMD's shaders |
| `navi10/` | RX 5700 XT, gfx1010 | the fork's build unchanged, and nothing else at all |

The two entries so far disagree about almost everything, which is the point of measuring each card
rather than carrying an answer across. On the Vega two of AMD's shaders beat the fork's and the
patched driver is worth 3.9x. On the Navi neither is true: no role is worth taking, and the driver
patch and the Vulkan layer are both worth nothing for the DLL you would run.
