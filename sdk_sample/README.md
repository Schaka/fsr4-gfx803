# The FSR SDK sample executable

`FidelityFX_FSR.exe` is AMD's FSR sample application from the FidelityFX SDK release. The project
used it as a repeatable benchmark before a real game was available.

| file | md5 | what it is |
|---|---|---|
| `stock/FidelityFX_FSR.exe` | `546eb03b61c408fb65b387d68a77f89a` | The unchanged executable from the SDK release. |
| `patched/FidelityFX_FSR.exe` | `7f68c091536c9839878abe2e39347aa6` | The same executable with two byte patches, described below. `../testkit/` uses this one. |
| `patched/FidelityFX_FSR.pdb` | `60e7b501c9783f24f1156cf551feaa16` | The debug symbols from the SDK release. The patch offsets came from this file. |

## The two patches

The sample exposes both settings only as checkboxes in its user interface. No command-line option
or configuration key sets them, so the constructor defaults are patched instead.

| field | offset | VMA | original | patched |
|---|---|---|---|---|
| `m_FrameInterpolation` | 169 (`0xa9`) | `0x14000e495` | `movl $0x1010101,0xa8(%rbx)` | `movl $0x1010001,0xa8(%rbx)` |
| `m_overrideVersion` | 220 (`0xdc`) | `0x14000e4db` | `mov %dil,0xdc(%rbx)` | `movb $0x1,0xdc(%rbx)` |

The first patch turns frame interpolation off. The second
turns on the FSR version override, so the sample uses the version that `m_FsrVersionIndex` selects.
Both replacements keep the instruction length. The field offsets were read with
`llvm-pdbutil dump -types FidelityFX_FSR.pdb`.

To see the changed bytes:

```bash
cmp -l stock/FidelityFX_FSR.exe patched/FidelityFX_FSR.exe
```
