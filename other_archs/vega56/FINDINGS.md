# FSR4 on a Vega 56 (gfx900)

Same box, same game, same scene, same resolution as the GCN4 work: Pragmata, 1280x720 upscaled to
1920x1080, FSR 4.1.1b. The RX 570 was swapped out for the Vega, so nothing here shares a batch with
a GCN4 number and the two are not directly comparable.

## What the card is

gfx900. Packed 16-bit arithmetic at double rate, which GCN4 has none of. No packed int8 dot product:
`integerDotProduct4x8BitPackedSignedAccelerated` reads false, so the layer's rewrite still applies.
`shaderFloat16` is true without the `~/.drirc` option GCN4 needs. Its own HDMI output drives the
monitor, so the compositor scans out directly rather than copying to the onboard chip.

## Settled

| build | upscaler ms | frametime ms |
|---|---:|---:|
| the fork's build, wave64 fix and fp32 prepass | 4.85 | 9.37 |
| the same with the fork's own fp16 prepass kept | 5.07 | 9.56 |
| the same with the float chains lifted to integers | 6.05 | 10.34 |
| AMD's own DLL, no set | 8.34 | 12.16 |

**The fp32 prepass is a small win here too, not a loss.** Vega runs packed fp16 at double rate, so
the expectation was that the fork's original 16-bit prepass would win. It does not: 4.85 ms against
5.07. Whatever the prepass costs, it is not the multiplies.

**The integer lift loses on both cards.** 6.05 ms against 4.85, matching the 1.1 to 1.65 ms it lost
on GCN4. It is a dead end everywhere, not a GCN4 quirk.

**The dot product rewrite is nearly free and nearly worthless on the fork's build.** Three modules
contain one, and turning the rewrite off moves nothing: 9.34 ms against 9.37. On AMD's own DLL,
which has thirteen, it is worth about 0.2 ms. On GCN4 the same rewrite was worth 4 ms. The card
lowers a packed dot product far better than GCN4 does.

## Measuring on this card

The Vega runs this scene at about 107 fps, which does not load it: `gpu_busy_percent` reads 62 to 85
and the clock bounces between 852 and 1663 MHz, where the RX 570 sat at 99.6 percent and full
clocks. `power_dpm_force_performance_level` is root-owned, so the clock cannot be pinned. Run to run
spread is still about 0.06 ms, which is small against the differences that matter, but it is worth
knowing that this card is not saturated and a change that only removes arithmetic may not show.

## The answer is the opposite of the GCN4 one

On GCN4 the hybrid won: the fork kept four roles and AMD's shaders took the other ten, because a
tuned set beat the fork there. On this card the fork wins everywhere and nothing should be given
away.

| build | upscaler ms | frametime ms |
|---|---:|---:|
| the fork everywhere | 5.03 | 9.37 |
| the fork, but AMD keeps the prepass | 5.05 | 9.68 |
| the fork, but AMD keeps the postpass | 5.86 | 10.09 |
| the hybrid, ten roles to AMD | 7.28 | 11.24 |

AMD's own DLL is not competitive here at all, with or without a set:

| build | upscaler ms | frametime ms |
|---|---:|---:|
| AMD's DLL, `fin15` | 6.30 | 10.37 |
| AMD's DLL, no set | 8.66 | 12.19 |
| AMD's DLL, `prune16` | 8.64 | 12.21 |
| AMD's DLL, `exact` | 8.76 | 12.39 |
| AMD's DLL, `fin25` | 10.56 | 14.19 |

Two things in that table are worth keeping. `fin25` is the best set on an RX 570 and the worst
thing measured here, well behind doing nothing. And the repository's older Vega figures, which put
`fin25` at 3.5 ms against 7.5 for no set, do not reproduce: the ranking is inverted. A set is tuned
against one card's instruction mix and does not carry.

So a shader set has no part to play on this card. The fork replaces every shader it ships, none of
the hot ones is reachable by a set, and the layer replaces nothing and expands three dot products
for no measurable gain.

## What is left to try

Instruction counts on this card, from RADV, for the fork's own shaders:

| shader | instructions | VGPRs | waves per SIMD |
|---|---:|---:|---:|
| pass9 | 43,555 | 256 | 1 |
| pass6 | 41,469 | 128 | 2 |
| pass7 | 30,517 | 256 | 1 |
| postpass | 8,182 | 64 | 4 |

pass9 needs 158 registers before scheduling and ends on 256, which buys latency hiding at the cost
of a second wave. On GCN4 that trade did not matter, because the card was issue bound at 99.6
percent busy and a second wave had nothing to add. This card runs at 62 to 85 percent on the same
scene, so it is stalling rather than issuing, and the trade may be the wrong way round here.

Reaching 3 ms from 4.85 needs about 40 percent fewer instructions. Nothing in the arithmetic offers
that without removing multiplies: the chains are integer valued, and neither of this card's packed
forms can carry them. Packed 16-bit integers overflow after two terms, because a byte times a weight
already reaches 16,256 of the 32,767 an int16 holds. Packed 16-bit floats are exact only to 2,048,
which a single product exceeds. Vega 10 has no int8 dot product. So the packed hardware that makes
this card fast cannot be pointed at this network without losing the result.

## Why 3 ms is not reachable without dropping multiplies

pass9 is the biggest shader and its budget on this card is:

| what | instructions | share |
|---|---:|---:|
| the multiply-accumulates | 28,736 | 66% |
| turning bytes into floats | about 4,850 | 11% |
| register copies the scheduler added | 3,104 | 7% |
| loads, addressing, control | about 6,900 | 16% |
| total, measured by RADV | 43,555 | |

Two ideas die on that table.

**Cheaper byte conversion.** GCN has `v_cvt_f32_ubyte0` through `3`, which turn one byte of a dword
into a float in a single instruction, where the fork spends a shift, a shift and a convert. It can
be made exact by reading the byte unsigned and folding the 128 back into the chain's bias, which is
a compile-time constant. But the fork already converts each byte once and uses it 17.8 times on
average, so the whole conversion is 11 percent of the shader and this would recover about 7 percent
of it. Somewhere near 0.15 ms on a 4.85 ms upscaler, for a rewrite that touches every chain.

**Free conversion through the texture unit.** A typed buffer load converts int8 to float in the
texture unit at no cost, which is how AMD's own shaders avoid this entirely. The fork reads raw
byte-address buffers instead. Changing that means changing the resource type the DLL binds, not the
shader, so it is not reachable by editing shader code.

Two thirds of the shader is the multiplies themselves. Reaching 3 ms from 4.85 means roughly 40
percent fewer instructions, and nothing short of doing fewer multiplies gets there. That is pruning,
and pruning is the knob that was rejected on GCN4 for making hair unstable.

## Why packing cannot be carried over to the fork's shaders

The packing trick behind the shader sets needs one activation multiplied by two weights, so that one
`v_mad_i32_i24` serves two output channels. The structure it needs does exist in rc10, contrary to
the note from the GCN4 work: pass9 converts 1,616 activations and uses each in 17.8 multiplies. It
still cannot be used, and the reason is how the two instructions are encoded rather than anything
about the network.

In the sets, packing pays because AMD's shaders stream their weights from a buffer, so the packed
value is a register that was going to be loaded anyway. In the fork's shaders every weight is a
constant baked into the code, and the two instructions carry a constant differently. `v_mac_f32` is
VOP2 and takes a 32-bit literal, so a float multiply-accumulate against a constant is one
instruction. `v_mad_i32_i24` is VOP3, where GCN allows no literal, so the constant has to be moved
into a register first. Packing two weights into one operand produces a value that is almost never in
the inline constant range, and each packed pair is used once, so the move cannot be hoisted or
shared. One move plus one packed multiply-accumulate serves two multiplies, which is one instruction
per multiply: exactly what the float form already costs.

That also explains the 1.2 ms the integer lift lost here, and the 1.1 to 1.65 ms it lost on GCN4. It
adds the move and gets nothing back.

So there is no untried arithmetic left. Two thirds of the work is multiplies, each already costs one
instruction, and the only way down is to do fewer of them.

## Corrections from the fuller sweeps

Three things I concluded early were wrong, and the per-role data says so.

**The fork does not win every role.** Handing all ten model passes to AMD is bad, at 7.24 ms against
5.03, and that is what led me to say the fork wins everywhere. Measured one role at a time, two of
them are better from AMD's shaders:

| build | upscaler ms | frametime ms |
|---|---:|---:|
| fork, AMD keeps pass11 | 4.62 | 9.11 |
| fork, AMD keeps pass9 | 4.71 | 9.16 |
| fork, AMD keeps pass7 | 4.98 | 9.41 |
| fork everywhere | 5.01 | 9.56 |
| fork, AMD keeps pass6 | 4.97 | 9.59 |

**rc9 is faster than rc10 here.** The opposite of the GCN4 preference, and worth more than most of
what was tried on purpose.

| build | upscaler ms | frametime ms |
|---|---:|---:|
| rc9, wave64 fix and fp32 prepass | 4.59 | 9.11 |
| rc9, wave64 fix only | 4.83 | 9.35 |
| rc10, wave64 fix and fp32 prepass | 4.93 | 9.36 |
| rc10, wave64 fix only | 4.96 | 9.53 |

**Occupancy is worth something on this card.** On GCN4 capping the register file changed nothing,
because that card was issue bound at 99.6 percent busy. This one stalls, and the cap buys 0.33 ms:

| register cap | upscaler ms | frametime ms |
|---|---:|---:|
| none | 5.06 | 9.68 |
| 128 | 4.91 | 9.34 |
| 96 | 4.99 | 9.35 |
| 80 | 4.83 | 9.36 |

## What pruning is actually worth

| share | upscaler ms | frametime ms |
|---|---:|---:|
| none | 5.13 | 9.60 |
| 0.03 | 4.85 | 9.31 |
| 0.05 | 4.50 | 9.02 |
| 0.08 | 4.46 | 8.84 |
| 0.12 | 4.54 | 8.85 |
| 0.15 | 4.07 | 8.51 |

Dropping 58 percent of every multiply outside the postpass buys 1.06 ms. That is a poor trade
against three lossless changes worth about the same together, and it is the knob that made hair
unstable on GCN4. Pruning is not the way to a faster Vega.

## Both of those levers evaporated when measured in one batch

The rc9 advantage and the register cap were each measured against a control in their own batch. Put
into a single interleaved batch with the split, neither survives.

| build | upscaler ms | frametime ms |
|---|---:|---:|
| rc9, pass9 and pass11 to AMD | 4.42 | 8.90 |
| rc10, pass9 and pass11 to AMD | 4.39 | 8.91 |
| rc10, the fork everywhere | 5.04 | 9.51 |
| rc9, the fork everywhere | 4.89 | 9.52 |

rc9 and rc10 are the same build to within 0.01 ms, with the split and without it. The earlier
0.34 ms was cross-batch drift, which is exactly the thing the interleaving exists to defeat and the
thing I said not to trust across batches.

The register cap tells the same story once the split is in place:

| build | no cap | cap at 128 |
|---|---:|---:|
| rc10, the fork everywhere | 9.51 | 9.34 |
| rc10, with the split | 8.91 | 8.92 |
| rc9, with the split | 8.90 | 9.08 |

The cap helps the unsplit build and does nothing for the split one. Both were treating the same
problem: pass9 and pass11 are the shaders that hold 256 registers for one wave per SIMD, and giving
those two roles to AMD's shaders removes the pressure that the cap was working around. Only one of
the two fixes is needed, and the split is the better one because it also removes the work.

## Where that leaves it

One change accounts for the whole gain, and it is exact: give pass9 and pass11 back to AMD's
shaders and keep the fork everywhere else.

    SKIP_ENTRIES=pass9,pass11

5.04 ms down to 4.39, and 4.33 with `fin15` on top, which reaches one of the two roles. No pruning,
no rc9, no register cap, no driver flag.

## Confirmed by eye

Measured by hand on the card, with the picture looked at:

| build | upscaler ms | picture |
|---|---:|---|
| the fork everywhere | about 4.7 | the reference |
| pass9 and pass11 to AMD | 4.3 | no difference anyone could see |
| the same, plus `fin15` | 4.2 | possibly slightly worse |

So the answer for this card is one change, and it costs nothing:

    SKIP_ENTRIES=pass9,pass11

A shader set is not worth adding on top. `fin15` reaches one of the two swapped roles, buys 0.1 ms
and may cost picture, which is the wrong side of that trade. Every shader in the shipped build is
either the fork's or AMD's own, and both are exact, so this tier has no approximation in it at all.
