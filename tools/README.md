# Test machine scripts

These scripts run on the test machine, from `/data/fsr4_tools`. Copy this directory there.
They contain the test machine's own paths, for example the Pragmata prefix under
`/home/user/Games/Heroic`. Change the paths before you use them anywhere else.

| script | what it does |
|---|---|
| `fsr4_env.sh` | Environment for the FSR SDK sample: test directory, prefix, Proton, GPU selection. Sourced by `bench_fsr4.sh`. |
| `restart_compositor.sh` | Starts weston with the VNC and headless backends. The SDK sample runs on this compositor. |
| `bench_fsr4.sh` | Benchmarks the SDK sample. Arguments: `FSR4_DOT_MODE` value, skip factor, seconds. Prints one CSV row. |
| `collect_data.sh` | Runs `bench_fsr4.sh` in several modes and collects the files in `../data/`. |
| `start_sway.sh` | Starts headless sway. Pragmata runs on this compositor. |
| `vgamepad.py` | Creates a virtual Xbox 360 gamepad over uinput and presses buttons named on a FIFO. Run as root. |
| `bench_pragmata.sh` | Starts Pragmata, loads the save with the virtual gamepad, and measures frametimes over a fixed window. Prints one CSV row. |
| `bench_pragmata_dll_matrix.sh` | Runs `bench_pragmata.sh` once per upscaler DLL and driver pair and writes `matrix.csv`. |
| `build_proxies.sh` | Runs on the workstation, not the test machine. Builds the frame-generation passthrough proxy in `../proxies/`. |

`../notes/HEADLESS_GAME.md` explains why the game needs sway and a virtual gamepad.

## Process matching

The scripts stop processes with `pgrep -x` and `pkill -x`, or with a split string such as
`"PRAGMA""TA.exe"`. Do not change this. If you grep `ps` for a process name over ssh, the ssh command
line matches too, and the kill ends your own session.
