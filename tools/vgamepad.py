#!/usr/bin/env python3
"""Virtual Xbox-style gamepad over uinput.

Wine reads gamepads straight from /dev/input through evdev, bypassing the
compositor. That is why this works where a virtual keyboard did not: keyboard
events have to travel through the compositor, and that path drops them.

Run as root. Press a button by writing its name to the FIFO:
    echo A > /data/tmp/vgamepad.fifo
"""
import os, sys, time
from evdev import UInput, ecodes as e, AbsInfo

FIFO = "/data/tmp/vgamepad.fifo"

BUTTONS = {
    "A": e.BTN_SOUTH, "B": e.BTN_EAST, "X": e.BTN_NORTH, "Y": e.BTN_WEST,
    "LB": e.BTN_TL, "RB": e.BTN_TR,
    "START": e.BTN_START, "SELECT": e.BTN_SELECT, "BACK": e.BTN_SELECT,
    "THUMBL": e.BTN_THUMBL, "THUMBR": e.BTN_THUMBR,
}

abs_axis = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
trigger  = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
hat      = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)

cap = {
    e.EV_KEY: sorted(set(BUTTONS.values())),
    e.EV_ABS: [
        (e.ABS_X, abs_axis), (e.ABS_Y, abs_axis),
        (e.ABS_RX, abs_axis), (e.ABS_RY, abs_axis),
        (e.ABS_Z, trigger), (e.ABS_RZ, trigger),
        (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat),
    ],
}

# Identify as a Microsoft Xbox 360 pad so Wine and SDL apply their normal mapping.
ui = UInput(cap, name="Microsoft X-Box 360 pad", vendor=0x045e,
            product=0x028e, version=0x0110, bustype=e.BUS_USB)
print("virtual gamepad up:", ui.device.path, flush=True)

def press(name, hold=0.08):
    code = BUTTONS.get(name.upper())
    if code is None:
        print("unknown button", name, flush=True); return
    ui.write(e.EV_KEY, code, 1); ui.syn()
    time.sleep(hold)
    ui.write(e.EV_KEY, code, 0); ui.syn()
    print("pressed", name.upper(), flush=True)

if os.path.exists(FIFO):
    os.remove(FIFO)
os.mkfifo(FIFO, 0o666)
os.chmod(FIFO, 0o666)

try:
    while True:
        with open(FIFO) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                if line.upper() == "QUIT":
                    raise SystemExit(0)
                press(line)
finally:
    ui.close()
