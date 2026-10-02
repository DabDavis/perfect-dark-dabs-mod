#!/usr/bin/env python3
"""Print a GoldenEye AI list (raw bytes, ai/listbytes.py) command by command,
with GoldenEye's command names and lengths from port/include/geaitable.h."""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', '..', 'port', 'include', 'geaitable.h')
CMDS = {}
for line in open(TABLE):
    m = re.match(r'\s*/\* ([0-9a-f]{2}) (\w+)\s*\*/ \{\s*(\d+),', line)
    if m:
        CMDS[int(m.group(1), 16)] = (m.group(2), int(m.group(3)))


def decode(b, limit=400):
    at, out = 0, []
    while at < len(b) and len(out) < limit:
        op = b[at]
        name, n = CMDS.get(op, ('?%02x' % op, 1))
        if n == 0:   # PRINT: text to a NUL
            n = b.index(0, at + 1) - at + 1
        out.append((at, name, b[at + 1:at + n].hex()))
        if op == 0x04:
            break
        at += n
    return out


if __name__ == '__main__':
    for path in sys.argv[1:]:
        print('==', path)
        for at, name, args in decode(open(path, 'rb').read()):
            print('  %04x %-34s %s' % (at, name, args))
