#!/bin/bash
# Headless sway for the Pragmata benchmark. Input comes from tools/vgamepad.py.
# Match the process by executable name only.
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=gles2
export XDG_RUNTIME_DIR=/run/user/1000
pgrep -x sway >/dev/null && { pkill -x -9 sway; sleep 2; }
mkdir -p /data/tmp/swaycfg
printf "output HEADLESS-1 resolution 1920x1080\nxwayland enable\n" > /data/tmp/swaycfg/config
nohup sway -c /data/tmp/swaycfg/config > /data/tmp/sway.log 2>&1 &
disown
sleep 6
for s in /run/user/1000/wayland-1 /run/user/1000/wayland-0; do
  [ -S "$s" ] && echo "sway socket: $(basename $s)"
done
pgrep -xc sway | sed "s/^/sway processes: /"
