"""Play a PAD_SCRIPT schedule on a virtual Xbox 360 controller (ViGEmBus), so
RPCS3 -- the oracle -- gets the same scripted input as the native build.

    work\\venv\\Scripts\\python.exe -I tools/vpad_script.py "<script>" [--scale 1.0] [--offset 0] [--seconds N]

<script> uses native PAD_SCRIPT's syntax: comma-separated "<sec>:<mask>[:<hold sec>]"
with CELL_PAD masks (DIGITAL2 << 8 | DIGITAL1) and the stick bits:
  START 0x0008  SELECT 0x0001  UP 0x0010 RIGHT 0x0020 DOWN 0x0040 LEFT 0x0080
  L2 0x0100 R2 0x0200 L1 0x0400 R1 0x0800 TRIANGLE 0x1000 CIRCLE 0x2000 CROSS 0x4000 SQUARE 0x8000
  left stick  0x10000 up, 0x20000 down, 0x40000 left, 0x80000 right
  right stick 0x100000 up, 0x200000 down, 0x400000 left, 0x800000 right
Times are seconds from when this script starts, times --scale, plus --offset (RPCS3 boots
slower than the native build). A press without a hold lasts 0.35 s. The pad is created at
once and stays plugged until the script ends (--seconds, default: last event + 5 s), so start
it BEFORE RPCS3: an XInput pad that appears later is still seen, but this way port 1 is it.
"""
import argparse
import sys
import time

import vgamepad as vg

B = vg.XUSB_BUTTON
BUTTONS = {
    0x0001: B.XUSB_GAMEPAD_BACK, 0x0002: B.XUSB_GAMEPAD_LEFT_THUMB, 0x0004: B.XUSB_GAMEPAD_RIGHT_THUMB,
    0x0008: B.XUSB_GAMEPAD_START, 0x0010: B.XUSB_GAMEPAD_DPAD_UP, 0x0020: B.XUSB_GAMEPAD_DPAD_RIGHT,
    0x0040: B.XUSB_GAMEPAD_DPAD_DOWN, 0x0080: B.XUSB_GAMEPAD_DPAD_LEFT,
    0x0400: B.XUSB_GAMEPAD_LEFT_SHOULDER, 0x0800: B.XUSB_GAMEPAD_RIGHT_SHOULDER,
    0x1000: B.XUSB_GAMEPAD_Y, 0x2000: B.XUSB_GAMEPAD_B, 0x4000: B.XUSB_GAMEPAD_A, 0x8000: B.XUSB_GAMEPAD_X,
}


def parse(script):
    ev = []
    for item in script.split(","):
        item = item.strip()
        if not item:
            continue
        parts = item.split(":")
        t = float(parts[0].lstrip("w"))
        mask = int(parts[1], 0)
        hold = float(parts[2]) if len(parts) > 2 else 0.35
        ev.append((t, mask, hold))
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--offset", type=float, default=0.0)
    ap.add_argument("--seconds", type=float, default=0.0)
    a = ap.parse_args()
    ev = [(t * a.scale + a.offset, m, h) for t, m, h in parse(a.script)]
    end = a.seconds or (max((t + h for t, _m, h in ev), default=0) + 5)
    pad = vg.VX360Gamepad()
    pad.reset(); pad.update()
    print(f"[vpad] virtual X360 pad up; {len(ev)} events over {end:.0f} s", flush=True)
    t0 = time.time()
    said = set()
    while True:
        now = time.time() - t0
        if now > end:
            break
        btn, lt, rt = 0, 0, 0
        lx = ly = rx = ry = 0.0
        for i, (t, m, h) in enumerate(ev):
            if t <= now < t + h:
                if i not in said:
                    said.add(i)
                    print(f"[vpad] t={now:6.1f}s mask 0x{m:06X} for {h:.2f}s", flush=True)
                for bit, xb in BUTTONS.items():
                    if m & bit:
                        btn |= int(xb)
                if m & 0x0100: lt = 255
                if m & 0x0200: rt = 255
                if m & 0x10000: ly = 1.0
                if m & 0x20000: ly = -1.0
                if m & 0x40000: lx = -1.0
                if m & 0x80000: lx = 1.0
                if m & 0x100000: ry = 1.0
                if m & 0x200000: ry = -1.0
                if m & 0x400000: rx = -1.0
                if m & 0x800000: rx = 1.0
        pad.reset()
        for xb in BUTTONS.values():
            if btn & int(xb):
                pad.press_button(button=xb)
        pad.left_trigger(value=lt); pad.right_trigger(value=rt)
        pad.left_joystick_float(x_value_float=lx, y_value_float=ly)
        pad.right_joystick_float(x_value_float=rx, y_value_float=ry)
        pad.update()
        time.sleep(0.01)
    pad.reset(); pad.update()
    print("[vpad] done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
