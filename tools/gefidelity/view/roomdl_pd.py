"""Ours: dump the xlu (and with GF_OPA=1 the opa) display lists of rooms GF_ROOMS
after standing at SPOT, so the rooms are loaded."""
import os, sys, struct
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbpd as lib
import gdb
lib.boot(None, 0)
s = [float(v) for v in os.environ['SPOT'].split(',')]
lib.hold(s[0], s[1], s[2], s[3], s[4], n=10, room=int(s[5]) if len(s) > 5 else None)
inf = gdb.selected_inferior()
out = open(os.path.join(os.environ['GF_OUT'], 'roomdl.txt'), 'w')
def rd(a, n):
    return bytes(inf.read_memory(a, n))
def walk(b, depth=0):
    while b:
        t = rd(b, 1)[0]
        nxt, u0, u1, u2 = struct.unpack('<QQQQ', rd(b + 8, 32))
        if t == 0:
            out.write('  leaf gdl 0x%x vtx 0x%x col 0x%x\n' % (u0, u1, u2))
            out.write('    cols ' + rd(u2, 64).hex() + '\n')
            out.write('    vtx ' + rd(u1, 48).hex() + '\n')
            g = u0
            for k in range(4000):
                w0, w1 = struct.unpack('<QQ', rd(g + 16 * k, 16))
                op = (w0 >> 24) & 0xff
                out.write('    %02x %016x %016x\n' % (op, w0, w1))
                if op == 0xfd and os.environ.get('GF_TEXDUMP'):
                    open(os.path.join(os.environ['GF_OUT'], 'tex_%x.bin' % w1), 'wb').write(rd(w1, 4096))
                if op == 0xdf or op == 0xb8:
                    break
        elif t == 1:
            walk(u0, depth + 1)
        b = nxt
for r in [int(v) for v in os.environ['GF_ROOMS'].split(',')]:
    g = int(lib.ev('(long)g_Rooms[%d].gfxdata' % r))
    out.write('room %d gfxdata 0x%x\n' % (r, g))
    if not g:
        continue
    xl = int(lib.ev('(long)g_Rooms[%d].gfxdata->xlublocks' % r))
    op = int(lib.ev('(long)g_Rooms[%d].gfxdata->opablocks' % r))
    out.write(' xlu\n')
    walk(xl)
    if os.environ.get('GF_OPA') == '1':
        out.write(' opa\n')
        walk(op)
out.close()
lib.finish()
