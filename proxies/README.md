# Frame-generation passthrough proxy

`fg_passthrough_proxy.dll` is used by the FSR SDK sample in `../testkit/`. Pragmata does not need it.

It takes the place of `amd_fidelityfx_framegeneration_dx12.dll`. It turns the frame-generation
prepare dispatch (type `0x2000C`) into a no-op and passes every other call to the real DLL, which it
loads as `amd_fidelityfx_framegeneration_dx12.dll.real`. The sample then runs with frame generation
off.

| file | md5 |
|---|---|
| `fg_passthrough_proxy.c` | source |
| `fg_passthrough_proxy.dll` | `37df1eea87c5cb5f923852972647e05b` |

Build it with `../tools/build_proxies.sh`, which needs `mingw64-gcc`.
