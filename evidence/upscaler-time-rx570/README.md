# Upscaler GPU time on an RX 570

The raw output behind the tables in `docs/SETS.md`, taken on 2026-09-14 on the gfx803 test machine.
The rig is an RX 570 with patched RADV from Mesa 26.2.2, stock vkd3d-proton, OptiScaler 0.9.4 and
FSR 4.1.1b. Pragmata runs at 1280x720 upscaled to 1920x1080, one scene, 60 seconds scored after a
150 second warm-up.

The numbers come from the Vulkan layer. `FSR4_PROFILE=1` writes a GPU timestamp before and after
every dispatch of a shader over 40 KB. That size is what separates FSR4's network passes from a
game's own compute work. Both timestamps are bottom of pipe, so a dispatch is measured from the
completion of the work before it.

| file | what it holds |
|---|---|
| `matrix_prof.csv` | one row per configuration, with the measurement running |
| `frametime.csv` | the same configurations without it, so the frametimes carry no overhead |
| `control_profile_lines.txt` | the raw report lines for the control |
| `speed_profile_lines.txt` | the raw report lines for `speed` |
| `bc250_profile_lines.txt` | the raw report lines for the BC-250 fork's DLL |

The control is the layer loaded and neutral, `FSR4_SET=off FSR4_NO_SDOT_EXPAND=1`. It sets the same
vkd3d-proton options as every other row and changes nothing else, so it is the baseline.
`FSR4_SET=none` is not a baseline. It takes the layer out of the process, which drops those options
as well.

Each report line reads `upscaler <ms> ms/s over <seconds>, <n> dispatches/s`. Divide the first number
by the frame rate for the cost per frame. Divide the dispatch rate by the same number for the passes
per frame. Across these runs that lands between 29 and 45, and it moves with the set, because a
replacement changes which shaders clear the size threshold. Anything near zero means the network is
not running.

The BC-250 rows are the counter-example that makes the dispatch rate worth reading. That build
reports 1.6 ms and 121 fps, and runs four network dispatches per frame against 29 and more here. It
is not upscaling. `notes/BC250_DLL.md` has the detail.
