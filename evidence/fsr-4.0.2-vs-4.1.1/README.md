# FSR 4.0.2 against FSR 4.1.1 in Pragmata

Measured 2026-09-13 on an AMD RX 470 (Polaris10, GCN4), with `tools/bench_pragmata_dll_matrix.sh`.

## Result

Real gameplay, loaded from a save, one identical scene. The upscaler runs on every frame. Each run
samples a 100-second window, 1280x720 upscaled to 1920x1080. The runs happened back to back, and
only the upscaler DLL and the driver changed.

| run | upscaler DLL | md5 | RADV | frames | mean ms | median ms | fps |
|---|---|---|---|---:|---:|---:|---:|
| 1 | FSR4 4.0.2 INT8 | `0ca99991ce3669d1d5320eb011aadb25` | 26.2.2 + patch | 3294 | 30.44 | 30.31 | 32.9 |
| 2 | FSR4 4.1.1b INT8 | `15103d8c121636b1c3ef1184357b677e` | 26.2.2 + patch | 3623 | 27.78 | 27.76 | 36.0 |
| 3 | FSR4 4.1.1b INT8 | `15103d8c121636b1c3ef1184357b677e` | 26.2.2 stock | 1593 | 63.35 | 63.35 | 15.8 |
| 4 | FSR4 4.1.1 stock | `429d308876434a1247f40bd543efdddb` | 26.2.2 + patch | 3670 | 27.32 | 27.32 | 36.6 |
| 5 | FSR4 4.0.2 INT8, repeat of run 1 | `0ca99991ce3669d1d5320eb011aadb25` | 26.2.2 + patch | 3316 | 30.32 | 30.20 | 33.0 |

Runs 1 and 5 differ by 0.4 percent. The rig did not drift during the series.

## What it shows

* FSR 4.1.1 is faster than 4.0.2 on this GPU. With the patched driver, 4.1.1b takes 27.78 ms
  against 30.38 ms, the mean of runs 1 and 5. That is 2.6 ms less per frame, or 1.09 times the fps.
* The patch matters more with 4.1.1. On stock Mesa 26.2.2, 4.1.1b takes 63.35 ms. The patched driver
  makes it 2.28 times faster. With 4.0.2 the same comparison gave 1.75 times, in
  `../mesa-26.2.2-vs-our-patch/`.
* The stock 4.1.1 DLL and the 4.1.1b INT8 build perform the same within 2 percent. Both ran the INT8
  model.
* 4.1.1 on stock Mesa is slower than 4.0.2 on stock Mesa, 63.35 ms against 52.93 ms. So 4.1.1 is only
  the faster choice together with the patch.

## Proof that FSR4 ran

`logs/log_excerpts.txt` carries, for each run, the upscaler path, the render resolution, the vsync
state and the frame count. Every run shows `FSR31FeatureDx12` and `FSR4ModelSelection`, and none shows
`FSR2FeatureDx12_212`, the internal FSR 2.1.2 copy OptiScaler falls back to.

The OptiScaler log does not keep the FSR version string, because OptiScaler truncates its log when
the level loads. The driver comparison shows the network ran instead. FSR 3.1.5 costs about 1 ms and
does not use int8 multiplies, so a driver patch for int8 multiplies cannot change it. Run 3 against
run 2 is a 35.6 ms difference caused by the patch alone.

The photo in `../../docs/` shows the FSR4 watermark `FSR4-I8 UPSCALE 4.1.1` on this card.