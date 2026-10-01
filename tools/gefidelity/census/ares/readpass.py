#!/usr/bin/env python3
"""The cartridge's half of the census read pass - runs on the oracle host.

    python3 readpass.py SPEC.json

One mission in n64twin (common/aresge.py) with the read watch armed at the
level's own load (bossSetLoadedStage, Dam's id - the same entry aresge swaps
the level at): every CPU data read of physical RDRAM is marked per byte, bit 0
during the load, bit 1 during play, bit 2 from a copy routine (bcopy, memcpy),
bit 3 from the inflate segment (the decompressor reading its own window and
input - not a use of the data). After SPEC['frames'] video frames of play,
GoldenEye's own pointers say where its files are - the setup
(g_CurrentSetup.pads less the file's pad offset), the bg file
(ptr_bgdata_offsets), the stan file (standTileStart + 0x80 less the first
tile's offset) and every model whose ModelFileHeader.Switches is set - and the
map and the memory over each are dumped for census/ares/run.py to judge.
"""
import json, os, struct, sys, time
SPEC = json.load(open(sys.argv[1]))
sys.path.insert(0, SPEC['common'])
import aresge as G

OUT = SPEC['out']
os.makedirs(OUT, exist_ok=True)
S = SPEC['syms']
tw = G.twin()
for lo, hi in SPEC['exclude']:
    tw('readwatch-exclude 0x%08x 0x%08x' % (lo, hi))
for lo, hi in SPEC['copy']:
    tw('readwatch-copy 0x%08x 0x%08x' % (lo, hi))
tw('readwatch-arm-at 0x%08x if 4 %d' % (G.SYM['bossSetLoadedStage'], G.L['levelids']['LEVELID_DAM']))
t0 = time.time()
G.boot(SPEC['levelid'], SPEC.get('difficulty', 0))
armed, reads, _ = tw('readwatch-stats').split()
if armed != '1':
    raise SystemExit('the read watch never armed')
tw('readwatch-phase 1')
done = 0
while done < SPEC['frames']:
    k = min(600, SPEC['frames'] - done)
    tw('frames %d' % k)
    done += k
meta = {'mission': SPEC['mission'], 'levelid': SPEC['levelid'], 'boot_reads': int(reads),
        'reads': int(tw('readwatch-stats').split()[1]), 'frames': done, 'tick': G.tick(),
        'seconds': round(time.time() - t0, 1), 'ranges': []}


def cstr(addr):
    b = G.peek(addr, 40)
    return b.split(b'\0')[0].decode('latin-1')


def dump(stem, kind, base, length):
    path = os.path.join(OUT, stem + '.rw')
    tw('readwatch-dump %s 0x%08x %d' % (path, base, length))
    open(os.path.join(OUT, stem + '.mem'), 'wb').write(G.peek(base, length))
    meta['ranges'].append({'stem': stem, 'kind': kind, 'base': base, 'len': length})


setup = G.Rec('stagesetup', G.SYM['g_CurrentSetup'])
meta['setup_ptrs'] = {f: setup.ptr(f) for f in ('pathwaypoints', 'waypointgroups', 'intro', 'propDefs', 'patrolpaths',
                                                 'ailists', 'pads', 'boundpads', 'padnames', 'boundpadnames')}
f = SPEC['files']
dump(f['setup']['stem'], 'setup', setup.ptr('pads') - f['setup']['pads_off'], f['setup']['len'])
dump(f['bg']['stem'], 'bg', G.u32(S['ptr_bgdata_offsets']), f['bg']['len'])
dump(f['stan']['stem'], 'stan', G.u32(S['standTileStart']) + 0x80 - f['stan']['first'], f['stan']['len'])
# the data segment (fog and level tables, prop and character rows, global AI lists): where it ran
dump('data', 'data', 0x80020d90, SPEC.get('data_len', 247120))
models = f['models']
for table, rows, size in ((S['PitemZ_entries'], 340, 12), (S['c_item_entries'], 80, 20)):
    blob = G.peek(table, rows * size)
    for k in range(rows):
        hdr, name = struct.unpack_from('>II', blob, k * size)
        if not hdr or not name:
            continue
        sw = G.u32(hdr + 8)
        stem = cstr(name)
        if sw and stem in models:
            dump(stem, 'model', sw, models[stem])
json.dump(meta, open(os.path.join(OUT, 'meta.json'), 'w'), indent=1)
print('READPASS %s %d ranges %d reads %.0f s' % (SPEC['mission'], len(meta['ranges']), meta['reads'],
                                                 time.time() - t0), flush=True)
G.finish()
