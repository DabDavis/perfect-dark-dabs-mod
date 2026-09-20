#!/usr/bin/env python3
"""GoldenEye level files out of the ROM: bg, stan, setups, inflated.

Read from the ROM alone (gerom.py). bg files are stored whole (their room
blobs are 1172-compressed inside); stan and setup files are 1172 over the
whole file.
"""
import os, struct, zlib
import gerom

# the GoldenEye decomp: only the offline HD fit tools read it (gemodelconv.prop_names())
GE = '/home/sdg/perfect-dark/claude-007/007'
ROM = os.path.join(GE, 'baserom.u.z64')

# level key: (bg name, stan name, solo setup, mp setup or None, levelscale)
LEVELS = {
    'dam':  ('bg_dam',  'Tbg_dam',  'UsetupdamZ',    None,               0.23363999),
    'run':  ('bg_run',  'Tbg_run',  'UsetuprunZ',    None,               0.089571431),
    'stat': ('bg_stat', 'Tbg_stat', 'UsetupstatueZ', 'Ump_setupstatueZ', 0.107202865),
    'tra':  ('bg_tra',  'Tbg_tra',  'UsetuptraZ',    None,               0.15019713),
    'pete': ('bg_pete', 'Tbg_pete', 'UsetuppeteZ',   None,               0.34187999),
    'jun':  ('bg_jun',  'Tbg_jun',  'UsetupjunZ',    None,               0.094662853),
    'oat':  ('bg_oat',  'Tbg_oat',  None,            'Ump_setupoatZ',    0.14142857),
    # GoldenEye's other multiplayer levels (setup_text_pointers in chraidata.c:
    # Library, Basement and Stack are three setups on one level)
    'dish': ('bg_dish', 'Tbg_dish', None,            'Ump_setupdishZ',   0.47142857),
    'ref':  ('bg_ref',  'Tbg_ref',  None,            'Ump_setuprefZ',    0.94285715),
    'lib':  ('bg_ame',  'Tbg_ame',  None,            'Ump_setupameZ',    0.65999997),
    'base': ('bg_ame',  'Tbg_ame',  None,            'Ump_setupimpZ',    0.65999997),
    'stack': ('bg_ame', 'Tbg_ame',  None,            'Ump_setupashZ',    0.65999997),
    'ark':  ('bg_ark',  'Tbg_ark',  None,            'Ump_setuparkZ',    1.20648),
    'sevb': ('bg_sevb', 'Tbg_sevb', None,            'Ump_setupsevbZ',   0.53931433),
    'arch': ('bg_arch', 'Tbg_arch', None,            'Ump_setuparchZ',   0.50678575),
    'cave': ('bg_cave', 'Tbg_cave', None,            'Ump_setupcaveZ',   0.26824287),
    'cryp': ('bg_cryp', 'Tbg_cryp', None,            'Ump_setupcrypZ',   0.25608),
    'crad': ('bg_crad', 'Tbg_crad', None,            'Ump_setupcradZ',   0.23571429),
    # and its solo levels with no multiplayer setup
    'sevx': ('bg_sevx', 'Tbg_sevx', 'UsetupsevxZ',   None,               0.45445713),
    'sevxb': ('bg_sevx', 'Tbg_sevx', 'UsetupsevxbZ', None,               0.45445713),
    'silo': ('bg_silo', 'Tbg_silo', 'UsetupsiloZ',   None,               0.47256002),
    'dest': ('bg_dest', 'Tbg_dest', 'UsetupdestZ',   None,               0.44757429),
    'depo': ('bg_depo', 'Tbg_depo', 'UsetupdepoZ',   None,               0.21847887),
    'arec': ('bg_arec', 'Tbg_arec', 'UsetupcontrolZ', None,              0.49886572),
    'sev':  ('bg_sev',  'Tbg_sev',  'UsetupsevbunkerZ', None,            0.53931433),
    'azt':  ('bg_azt',  'Tbg_azt',  'UsetupaztZ',    None,               0.35300568),
}


def inflate1172(b):
    assert b[:2] == b'\x11\x72', b[:4].hex()
    return zlib.decompressobj(-15).decompress(b[2:])


_rom = None


def rom():
    """The player's GoldenEye ROM (US): $GE_ROM, else the decomp's baserom."""
    global _rom
    if _rom is None:
        _rom = gerom.Rom(os.environ.get('GE_ROM', ROM))
    return _rom


def rom_file(stem):
    if stem.startswith('Tbg_'):
        stem += '_all_p_stanZ'
    elif stem.startswith('bg_'):
        stem += '_all_p'
    return rom().file(stem)


class Bg:
    """A bg file: rooms {pos, vertices (16-byte Vtx, room-relative), primary and
    secondary display lists}, portals, vis commands. Pointers are 0x0f segment."""

    def __init__(self, data):
        self.d = d = data
        u = lambda o: struct.unpack_from('>I', d, o)[0]
        seg = lambda a: a & 0xffffff
        rooms_at, portals_at, vis_at = seg(u(4)), seg(u(8)), seg(u(12))
        self.rooms = []
        entries = []
        i = 0
        while True:
            o = rooms_at + 24 * i
            pt, pri, sec = u(o), u(o + 4), u(o + 8)
            pos = struct.unpack_from('>3f', d, o + 12)
            entries.append((pt, pri, sec, pos))
            if i > 0 and pri == 0:
                break
            i += 1
        # blob sizes run to the next non-zero offset in the file
        offs = sorted(set(seg(x) for e in entries for x in e[:3] if x))

        def blob(addr):
            if not addr:
                return None
            a = seg(addr)
            nxt = [x for x in offs if x > a]
            end = nxt[0] if nxt else len(d)
            return inflate1172(d[a:end])
        # entry 0 is empty, the last two are the end sentinel and the zero entry
        self.numrooms = len(entries) - 3
        for pt, pri, sec, pos in entries[1:1 + self.numrooms]:
            self.rooms.append(dict(pos=pos, vtx=blob(pt), pri=blob(pri), sec=blob(sec)))
        self.portals = []
        o = portals_at
        while u(o):
            p = seg(u(o))
            n = d[p]
            pts = [struct.unpack_from('>3f', d, p + 4 + 12 * k) for k in range(n)]
            self.portals.append(dict(points=pts, room1=d[o + 4], room2=d[o + 5], ctrl=struct.unpack_from('>H', d, o + 6)[0],
                                     vtxptr=u(o)))
            o += 8
        self.vis = []
        o = vis_at
        while vis_at and o + 8 <= len(d):
            t, ln = d[o], d[o + 1]
            arg = struct.unpack_from('>i', d, o + 4)[0]
            self.vis.append((t, ln, arg))
            o += 8
            if t == 0 and ln == 0 and arg == 0:
                break

    def world_vertices(self):
        out = []
        for r, room in enumerate(self.rooms, 1):
            v = room['vtx'] or b''
            for k in range(len(v) // 16):
                x, y, z = struct.unpack_from('>3h', v, 16 * k)
                out.append((room['pos'][0] + x, room['pos'][1] + y, room['pos'][2] + z, r))
        return out


if __name__ == '__main__':
    for key, (bg, stan, solo, mp, ls) in LEVELS.items():
        b = Bg(rom_file(bg))
        vs = b.world_vertices()
        st = rom_file(stan)
        print('%-5s rooms %3d portals %3d vis %3d vertices %6d stan %6d bytes solo %s mp %s' % (
            key, b.numrooms, len(b.portals), len(b.vis), len(vs), len(st),
            len(rom_file(solo)) if solo else '-', len(rom_file(mp)) if mp else '-'))
