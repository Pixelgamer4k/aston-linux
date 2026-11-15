#!/usr/bin/env python3
"""Inject a one-finger swipe through a temporary virtual touchscreen (test tool).

Coordinates are in landscape screen pixels (2780x1264).
Usage: touchinject.py X1 Y1 X2 Y2 [STEPS] [STEP_MS]
"""
import fcntl
import os
import struct
import sys
import time

W, H = 2780, 1264
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_TOUCH = 0x14a
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
INPUT_PROP_DIRECT = 1
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_DEV_SETUP, UI_ABS_SETUP = 0x405c5503, 0x401c5504
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e


def emit(fd, etype, code, value):
    os.write(fd, struct.pack("qqHHi", 0, 0, etype, code, value))


def main():
    x1, y1, x2, y2 = (int(a) for a in sys.argv[1:5])
    steps = int(sys.argv[5]) if len(sys.argv) > 5 else 30
    step_ms = int(sys.argv[6]) if len(sys.argv) > 6 else 12
    fd = os.open("/dev/uinput", os.O_WRONLY)
    for bit in (EV_KEY, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, bit)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
    fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
    for code, hi in ((ABS_X, W - 1), (ABS_Y, H - 1), (ABS_MT_SLOT, 9), (ABS_MT_POSITION_X, W - 1),
                     (ABS_MT_POSITION_Y, H - 1), (ABS_MT_TRACKING_ID, 65535)):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        # struct uinput_abs_setup { __u16 code; struct input_absinfo { value, min, max, fuzz, flat, resolution } }
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack("HHiiiiii", code, 0, 0, 0, hi, 0, 0, 0))
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 0x06, 0x1234, 0x5678, 1, b"aston-test-touch", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.5)                     # let the compositor pick the device up

    def point(x, y):
        emit(fd, EV_ABS, ABS_MT_POSITION_X, x)
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, y)
        emit(fd, EV_ABS, ABS_X, x)
        emit(fd, EV_ABS, ABS_Y, y)
        emit(fd, EV_SYN, 0, 0)

    emit(fd, EV_ABS, ABS_MT_SLOT, 0)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1)
    emit(fd, EV_KEY, BTN_TOUCH, 1)
    point(x1, y1)
    for i in range(1, steps + 1):
        time.sleep(step_ms / 1000)
        point(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
    time.sleep(0.15)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    emit(fd, EV_KEY, BTN_TOUCH, 0)
    emit(fd, EV_SYN, 0, 0)
    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)


if __name__ == "__main__":
    main()
