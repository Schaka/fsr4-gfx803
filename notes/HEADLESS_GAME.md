# Benchmarking Pragmata headlessly

The test machine runs Pragmata with no display and no operator. A virtual gamepad loads the save,
and the OptiScaler log provides the frametimes.

## The rig

| part | what it does |
|---|---|
| headless sway | The compositor. `tools/start_sway.sh` starts it on `wayland-1` with `WLR_BACKENDS=headless` and `WLR_LIBINPUT_NO_DEVICES=1`. |
| `tools/vgamepad.py` | Creates a virtual Microsoft Xbox 360 pad (`045e:028e`) over uinput and presses the buttons written to `/data/tmp/vgamepad.fifo`. Wine reads the pad directly from `/dev/input` through evdev. |
| `tools/bench_pragmata.sh` | Starts the game, walks the menus into the save, and measures one window. |
| `tools/bench_pragmata_dll_matrix.sh` | Runs `bench_pragmata.sh` once per upscaler DLL and driver pair. |

## Run it

```bash
/data/fsr4_tools/start_sway.sh
sudo python3 /data/fsr4_tools/vgamepad.py &
sudo chmod 0755 /dev/input
sudo chmod 0664 /dev/input/eventN        # the gamepad node that vgamepad.py prints
/data/fsr4_tools/bench_pragmata.sh <label> <seconds> [icd_path]
```

The user who runs the game must be able to read the gamepad node. After a reboot, repeat both
`chmod` commands.

`bench_pragmata.sh` waits 85 seconds for the title screen. It then presses `A` twelve times, four
seconds apart, which continues from the save. It waits another 45 seconds and samples frametimes by
log timestamp over the window. It prints `label,frames,mean_ms,median_ms,min_ms,fps`.

Quote the mean. With the upscaler on every frame, mean and median agree closely, which confirms the
run.

## Confirm that the run is real

```bash
G="$HOME/Games/Heroic/Prefixes/Pragmata/drive_c/Program Files (x86)/Pragmata"
grep -aoE "FSR2FeatureDx12_212|FSR31FeatureDx12|FSR4ModelSelection" "$G/OptiScaler.log" | sort | uniq -c
grep -a "Input Resolution" "$G/OptiScaler.log" | tail -2
```

A correct run shows `FSR31FeatureDx12` and `FSR4ModelSelection`, no `FSR2FeatureDx12_212`, and an
input resolution of 1280x720.

## Results, 2026-09-13

Real gameplay from one save, upscaler on every frame, 100-second window, 1280x720 upscaled to
1920x1080.

| upscaler DLL | RADV | mean ms | fps |
|---|---|---:|---:|
| 4.1.1 stock | Mesa 26.2.2 + patch | 27.32 | 36.6 |
| 4.1.1b INT8 | Mesa 26.2.2 + patch | 27.78 | 36.0 |
| 4.1.1b INT8 | Mesa 26.2.2 stock | 63.35 | 15.8 |
| 4.0.2 INT8 | Mesa 26.2.2 + patch | 30.38 | 32.9 |
| 4.0.2 INT8 | Mesa 26.1.6 + patch | 30.27 | 33.0 |
| 4.0.2 INT8 | Mesa 26.2.2 stock | 52.93 | 18.9 |

The 4.0.2 figure on Mesa 26.2.2 with the patch is the mean of three runs: 30.38, 30.44 and 30.32 ms.
Details and logs: `../evidence/fsr-4.0.2-vs-4.1.1/` and `../evidence/mesa-26.2.2-vs-our-patch/`.

## Process matching

Stop processes with `pgrep -x` and `pkill -x`, or with a split string such as `"PRAGMA""TA.exe"`.
A plain `grep` of `ps` output over ssh also matches the ssh command line, and the kill then ends the
ssh session.

## Game settings

`config.ini` in the game directory has `VSync=OFF`, `FrameRateSetting=Variable`,
`Resolution=1920x1080` and `UpscalingAlgorithm=FSR3`, which OptiScaler upgrades to FSR4.

## Packages

`sway` and the Python module `evdev`.
