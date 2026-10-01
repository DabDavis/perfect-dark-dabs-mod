#!/usr/bin/env python3
"""The offset map between a mission's GoldenEye AI lists and the converted ones.

    aimap.py dam              # summary; --json OUT writes the map

GoldenEye's bytecode and ours differ in every length (253 commands mapped onto
482, tools/geconvert/geaitable.py), so a trace line "list 1036 +37" means one
command in GoldenEye and another in ours. The map is taken from the
converter's own walk, not rebuilt beside it: gesolo.convert_ailist() is
recompiled here with one line added at the top of its loop that notes
(GoldenEye offset, our offset) before each command is converted. If that loop
changes shape the instrumentation refuses to load rather than guess.

The map's null runs every time: the lists rebuilt with the instrumented walk
must equal, byte for byte, the lists in the converted setup the game actually
loads (mods/GoldenEye Arenas/files/Usetupgs<key>Z in the run directory), so a
map from a converter that no longer matches the shipped conversion is refused.
"""
import argparse, inspect, json, os, struct, sys, textwrap, zlib
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(TREE, 'tools', 'geconvert'))
sys.path.insert(0, os.path.join(HERE, '..', 'common'))
import levels  # noqa: E402

RUNDIR = os.path.expanduser(os.environ.get('GF_RUNDIR', '~/wt/gefidelity-run'))
if 'GE_ROM' not in os.environ:
    for cand in (os.path.join(RUNDIR, 'added-content', 'GoldenEye 007 (U) [!].n64'),):
        if os.path.exists(cand):
            os.environ['GE_ROM'] = cand

import gefiles, gesolo, geaitable  # noqa: E402

# GoldenEye's mission setups, in mission order (geconvert.py's MISSIONS)
MISSION_KEYS = ['dam', 'ark', 'run', 'sevx', 'sev', 'silo', 'dest', 'sevxb', 'sevb', 'stat',
                'arch', 'pete', 'depo', 'tra', 'jun', 'arec', 'cave', 'crad', 'azt', 'cryp']
MISSION_SETUPS = ['UsetupdamZ', 'UsetuparkZ', 'UsetuprunZ', 'UsetupsevxZ', 'UsetupsevbunkerZ',
                  'UsetupsiloZ', 'UsetupdestZ', 'UsetupsevxbZ', 'UsetupsevbZ', 'UsetupstatueZ',
                  'UsetuparchZ', 'UsetuppeteZ', 'UsetupdepoZ', 'UsetuptraZ', 'UsetupjunZ',
                  'UsetupcontrolZ', 'UsetupcaveZ', 'UsetupcradZ', 'UsetupaztZ', 'UsetupcrypZ']

_HOOK_AT = '        op = d[at]\n'


def _instrumented_convert_ailist():
    src = textwrap.dedent(inspect.getsource(gesolo.convert_ailist))
    if src.count(_HOOK_AT) != 1:
        raise SystemExit('aimap: gesolo.convert_ailist() no longer has one "op = d[at]" at the top of its '
                         'loop - re-read it and move the hook')
    src = src.replace('def convert_ailist(', 'def _gf_convert_ailist(_gfmap, ', 1)
    src = src.replace(_HOOK_AT, _HOOK_AT + '        _gfmap.append((at, len(out)))\n', 1)
    ns = dict(vars(gesolo))
    exec(compile(src, gesolo.__file__ + ' (instrumented)', 'exec'), ns)
    return ns['_gf_convert_ailist']


_conv = None


def pd_command_names():
    """Our command names, from g_CommandPointers in src/game/chrai.c."""
    names = {}
    import re
    for line in open(os.path.join(TREE, 'src', 'game', 'chrai.c'), errors='replace'):
        m = re.match(r'\s*/\*0x([0-9a-fA-F]{4})\*/\s*(\w+)', line)
        if m:
            names[int(m.group(1), 16)] = m.group(2)
        if line.startswith('};') and names:
            break
    return names


def _inflate_1173(b):
    """Our converted setups (geconvert.py rzip1173): 11 73, a 3-byte length, raw deflate."""
    assert b[:2] == b'\x11\x73', b[:4].hex()
    return zlib.decompressobj(-15).decompress(b[5:])


def converted_lists(key, rundir=RUNDIR):
    """{id: bytes} of the converted setup's AI lists as the game loads them."""
    p = os.path.join(rundir, 'mods', 'GoldenEye Arenas', 'files', 'Usetupgs%sZ' % key)
    if not os.path.exists(p):
        return None
    d = _inflate_1173(open(p, 'rb').read())
    h = struct.unpack_from('>8I', d, 0)
    at = h[6]
    rows = []
    while True:
        ptr, lid = struct.unpack_from('>Ii', d, at)
        if not ptr and not lid:
            break
        rows.append((lid, ptr))
        at += 8
    starts = sorted(set(p for _, p in rows))
    out = {}
    for lid, ptr in rows:
        nxt = [s for s in starts if s > ptr]
        out[lid] = d[ptr:nxt[0]] if nxt else d[ptr:]
    return out


def build(mission, rundir=RUNDIR, check=True):
    """{'lists': {id: {...}}, 'checked': n} for one mission (index, key or title)."""
    global _conv
    if _conv is None:
        _conv = _instrumented_convert_ailist()
    m = levels.mission(mission)
    key, stem = MISSION_KEYS[m[0]], MISSION_SETUPS[m[0]]
    d = gefiles.rom_file(stem)
    numpads = len(_read_pads(d))
    data = gefiles.rom().data
    vehicles = gesolo.vehicle_lists(d)
    h = struct.unpack_from('>10I', d, 0)
    rows, o = [], h[5]
    while h[5]:
        ptr, lid = struct.unpack_from('>Ii', d, o)
        if not ptr and not lid:
            break
        rows.append((lid, d, ptr, lid in vehicles, False))
        o += 8
    for lid, off in gesolo.global_lists(data):
        rows.append((lid, data, off, False, True))
    rows = renumber_bg_duplicates(rows)
    seen, lists = set(), {}
    stats = {'ai_kept': 0, 'ai_dropped': {}, 'ai_unknown': 0, 'anims': set(), 'models': set()}
    for lid, buf, ptr, isveh, isglobal in rows:
        pid = gesolo.global_ai_id(lid) if isglobal else lid
        if pid in seen:
            continue            # GoldenEye takes the first of a duplicate id; so does the conversion
        seen.add(pid)
        gmap = []
        # offset=None: only GE_IFBONDY's value depends on it, never a length
        pdbytes = _conv(gmap, buf, ptr, stats, numpads, isveh, None)
        cmds = []
        for i, (gat, pdoff) in enumerate(gmap):
            op = buf[gat]
            nxt = gmap[i + 1][1] if i + 1 < len(gmap) else len(pdbytes)
            row = geaitable.TABLE[op] if op < len(geaitable.TABLE) else ('?', None, [], None, (), '')
            # our opcode as written, not the table's: IfBondY, a vehicle's
            # PlayAnimation and StartPatrol are written by hand in the walk
            pdop = struct.unpack_from('>H', pdbytes, pdoff)[0] if nxt > pdoff else None
            cmds.append({'ge': gat - ptr, 'op': op, 'name': row[0], 'pd': pdoff, 'pdlen': nxt - pdoff,
                         'pdop': pdop})
        lists[pid] = {'geid': lid, 'global': isglobal, 'vehicle': isveh, 'cmds': cmds,
                      'pdlen': len(pdbytes), 'pd': pdbytes,
                      'gebytes': bytes(buf[ptr:ptr + (cmds[-1]['ge'] + (gesolo.ai_length(buf, ptr + cmds[-1]['ge']) or 1) if cmds else 0)])}
    checked = 0
    if check:
        conv = converted_lists(key, rundir)
        if conv is None:
            raise SystemExit('aimap: no converted setup for %s in %s - boot the run directory once' % (key, rundir))
        bad = []
        for pid, L in lists.items():
            got = conv.get(pid)
            if got is None:
                bad.append('list %d is not in the converted setup' % pid)
                continue
            mine = L['pd']
            # IfBondY carries the level's height offset, the one value the map
            # built without the offset; compare everything else
            if len(got) < len(mine) or not _same_but_bondy(mine, got[:len(mine)], L):
                bad.append('list %d differs from the converted setup' % pid)
            else:
                checked += 1
        for pid in sorted(set(conv) - set(lists)):
            bad.append('list %d is in the converted setup and not in the map' % pid)
        if bad:
            raise SystemExit('aimap NULL FAILED for %s: %d of %d lists: %s' % (key, len(bad), len(lists), '; '.join(bad[:5])))
    for L in lists.values():
        L['pdhex'] = L.pop('pd').hex()
    return {'mission': m[0], 'key': key, 'lists': lists, 'checked': checked}


def show(built, pid, pdnames=None):
    """One list, GoldenEye's command and bytes beside the converted one's."""
    pdnames = pdnames or pd_command_names()
    L = built['lists'][pid]
    gb, pb = L['gebytes'], bytes.fromhex(L['pdhex'])
    out = ['list %d (GoldenEye %s%d)%s' % (pid, 'global ' if L['global'] else '', L['geid'],
                                           ' vehicle' if L['vehicle'] else '')]
    for i, c in enumerate(L['cmds']):
        ln = (L['cmds'][i + 1]['ge'] if i + 1 < len(L['cmds']) else len(gb)) - c['ge']
        g = gb[c['ge']:c['ge'] + ln].hex(' ')
        p = pb[c['pd']:c['pd'] + c['pdlen']].hex(' ') if c['pdlen'] else '(left out)'
        out.append('  +%-4d %-34s %-24s | +%-4d %-28s %s' % (
            c['ge'], c['name'], g, c['pd'], pdnames.get(c['pdop'], '') if c['pdop'] is not None else '', p))
    return '\n'.join(out)


BG_FIRST = 0x1000


def renumber_bg_duplicates(rows):
    """geconvert.c (the converter the game runs) gives a second row of a
    background list id (0x1000 and up) the next id no row has, because
    GoldenEye makes a chr of every such row, a duplicate too (Surface's second
    4106 is its fan trigger and the grate's clank, F3 20260930-005449).
    gesolo.convert_ailists() - the Python twin - still drops it; the map
    follows the game. rows: [(id, buf, ptr, vehicle, global)] in table order."""
    nextbg = BG_FIRST
    for r in rows:
        if not r[4] and BG_FIRST <= r[0] < 0xffff:
            nextbg = max(nextbg, r[0] + 1)
    out, seen = [], set()
    for r in rows:
        if not r[4] and BG_FIRST <= r[0] < 0xffff and r[0] in seen and nextbg < 0xffff:
            r = (nextbg,) + r[1:]
            nextbg += 1
        seen.add(r[0])
        out.append(r)
    return out


def _same_but_bondy(a, b, L):
    if a == b:
        return True
    a, b = bytearray(a), bytearray(b)
    for c in L['cmds']:
        if c['pdlen'] and c['name'] == 'IFBondYPosLessThan':
            a[c['pd'] + 2:c['pd'] + 6] = b[c['pd'] + 2:c['pd'] + 6]
    return a == b


def _read_pads(d):
    h = struct.unpack_from('>10I', d, 0)
    pads, o = [], h[6]
    while struct.unpack_from('>I', d, o + 36)[0]:
        pads.append(o)
        o += 0x2c
    return pads


class Map:
    """Lookups over one mission's map, both directions."""

    def __init__(self, built):
        self.lists = {int(k): v for k, v in built['lists'].items()}
        self.ge_to_cmd, self.pd_to_cmd, self.gid_to_pid = {}, {}, {}
        for pid, L in self.lists.items():
            self.gid_to_pid[(L['geid'], L['global'])] = pid
            for c in L['cmds']:
                self.ge_to_cmd[(pid, c['ge'])] = c
                if c['pdop'] is not None and c['pdlen'] > 0:
                    self.pd_to_cmd[(pid, c['pd'])] = c

    def pid_of_ge(self, geid):
        """Our id for a GoldenEye list id (a level's own first, then a global)."""
        if (geid, False) in self.gid_to_pid:
            return self.gid_to_pid[(geid, False)]
        return self.gid_to_pid.get((geid, True), gesolo.global_ai_id(geid))


def load(mission, rundir=RUNDIR):
    return Map(build(mission, rundir))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('missions', nargs='+')
    ap.add_argument('--json')
    ap.add_argument('--rundir', default=RUNDIR)
    ap.add_argument('--show', type=int, action='append', default=[], help='print one list (our id) side by side')
    a = ap.parse_args()
    out = {}
    for k in a.missions:
        b = build(k, a.rundir)
        n = sum(len(L['cmds']) for L in b['lists'].values())
        dropped = sum(1 for L in b['lists'].values() for c in L['cmds'] if c['pdop'] is None)
        print('%-10s %3d lists (%d checked against the converted setup), %5d commands, %d left out by the conversion' % (
            b['key'], len(b['lists']), b['checked'], n, dropped))
        for pid in a.show:
            print(show(b, pid))
        for L in b['lists'].values():
            L['gebytes'] = L['gebytes'].hex()
        out[b['key']] = b
    if a.json:
        json.dump(out, open(a.json, 'w'))


if __name__ == '__main__':
    main()
