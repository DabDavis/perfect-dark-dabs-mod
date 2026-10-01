#!/usr/bin/env python3
"""A virtual Xbox 360 pad for driving Xenia headlessly.

Creates the pad through /dev/uinput with the xpad driver's layout (which SDL's
controller database maps as an Xbox 360 controller), then reads commands from a
FIFO, one per line:

    press A|B|X|Y|LB|RB|BACK|START|GUIDE|LS|RS [seconds]
    stick LX|LY|RX|RY value seconds      value -32768..32767, released after
    trigger LT|RT value seconds          value 0..255, released after
    dpad UP|DOWN|LEFT|RIGHT [seconds]
    quit
"""
import os
import sys
import time
from evdev import UInput, AbsInfo, ecodes as e

FIFO = sys.argv[1]

BUTTONS = {
    'A': e.BTN_A, 'B': e.BTN_B, 'X': e.BTN_X, 'Y': e.BTN_Y,
    'LB': e.BTN_TL, 'RB': e.BTN_TR, 'BACK': e.BTN_SELECT, 'START': e.BTN_START,
    'GUIDE': e.BTN_MODE, 'LS': e.BTN_THUMBL, 'RS': e.BTN_THUMBR,
}
STICKS = {'LX': e.ABS_X, 'LY': e.ABS_Y, 'RX': e.ABS_RX, 'RY': e.ABS_RY}
TRIGGERS = {'LT': e.ABS_Z, 'RT': e.ABS_RZ}
DPAD = {'UP': (e.ABS_HAT0Y, -1), 'DOWN': (e.ABS_HAT0Y, 1),
        'LEFT': (e.ABS_HAT0X, -1), 'RIGHT': (e.ABS_HAT0X, 1)}

stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
trig = AbsInfo(0, 0, 255, 0, 0, 0)
hat = AbsInfo(0, -1, 1, 0, 0, 0)
cap = {
    e.EV_KEY: list(BUTTONS.values()),
    e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_RX, stick), (e.ABS_RY, stick),
               (e.ABS_Z, trig), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
}

ui = UInput(cap, name='Microsoft X-Box 360 pad', vendor=0x045e, product=0x028e,
            version=0x110, bustype=e.BUS_USB)
print('pad at', ui.device.path, flush=True)

if not os.path.exists(FIFO):
    os.mkfifo(FIFO)


def emit(kind, code, value):
    ui.write(kind, code, value)
    ui.syn()


while True:
    with open(FIFO) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            cmd = w[0]
            try:
                if cmd == 'quit':
                    ui.close()
                    sys.exit(0)
                elif cmd == 'press':
                    secs = float(w[2]) if len(w) > 2 else 0.15
                    emit(e.EV_KEY, BUTTONS[w[1]], 1)
                    time.sleep(secs)
                    emit(e.EV_KEY, BUTTONS[w[1]], 0)
                elif cmd == 'stick':
                    emit(e.EV_ABS, STICKS[w[1]], int(w[2]))
                    time.sleep(float(w[3]))
                    emit(e.EV_ABS, STICKS[w[1]], 0)
                elif cmd == 'trigger':
                    emit(e.EV_ABS, TRIGGERS[w[1]], int(w[2]))
                    time.sleep(float(w[3]))
                    emit(e.EV_ABS, TRIGGERS[w[1]], 0)
                elif cmd == 'dpad':
                    code, val = DPAD[w[1]]
                    secs = float(w[2]) if len(w) > 2 else 0.15
                    emit(e.EV_ABS, code, val)
                    time.sleep(secs)
                    emit(e.EV_ABS, code, 0)
                print('done', line.strip(), flush=True)
            except Exception as ex:
                print('bad', line.strip(), ex, flush=True)
