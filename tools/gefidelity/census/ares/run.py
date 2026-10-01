#!/usr/bin/env python3
"""The census's cartridge column: which of the fields the converter drops (and
copies unread) GoldenEye itself reads, on the real cartridge in ares.

    run.py sweep [--missions dam,facility] [--frames 3600] [--jobs 3]
    run.py join                       # census + cartridge maps -> cart.json, then census.py --reuse

`sweep` runs readpass.py on the oracle host (n64twin's readwatch-* commands,
ares core hook in ../../ares/ares-readhook.patch) for each mission: armed at
the level's load, SPEC frames of play with the guards alive (no input after
the opening still), then the read map over the setup, bg, stan and every model
the level loaded, located by GoldenEye's own pointers. Outputs under
~/wt/gefidelity-run/census-ares/<mission>/.

`join` places each dump on the file it covers (a range whose memory agrees
with the file's own bytes on fewer than half its non-zero bytes is refused),
walks it with the census's own segmenters, and writes census-ares/cart.json:
per record kind and field, the records observed, those whose field the
cartridge read during the load and during play, and the functions that read
it. census.py --cart reads that into report.md's cartridge column.

Null, every join: (1) Dam's pads - every PadRecord.plink must be read by the
cartridge (prop.c's relocation walk) and at least half the positions; (2)
Dam's pad-name strings (debug only, osSyncPrintf) must show no read at all;
(3) Archives' DOOR_SCALE scale must be read (GoldenEye's g_DoorScale is 0.75
there) - the read the recompiler's fast-path loads hid until the watch forced
its slow path. Otherwise join exits 4 and writes nothing.
"""
import collections, json, os, struct, subprocess, sys, time
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
GF = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(GF, 'census'))
sys.path.insert(0, os.path.join(GF, 'common'))
import levels

HOST = os.environ.get('GF_HOST', 'sdg@10.8.0.3')
ELF = '~/claude-007/007/build/u/ge007.u.elf'
REMOTE = 'census-ares'
OUT = os.path.expanduser('~/wt/gefidelity-run/census-ares')
CENSUS_OUT = os.path.expanduser('~/wt/gefidelity-run/census-out')

# GoldenEye's mission -> (its setup, its bg/stan stem)
FILES = {'dam': ('UsetupdamZ', 'dam'), 'facility': ('UsetuparkZ', 'ark'), 'runway': ('UsetuprunZ', 'run'),
         'surface': ('UsetupsevxZ', 'sevx'), 'bunker': ('UsetupsevbunkerZ', 'sev'), 'silo': ('UsetupsiloZ', 'silo'),
         'frigate': ('UsetupdestZ', 'dest'), 'surface2': ('UsetupsevxbZ', 'sevx'), 'bunker2': ('UsetupsevbZ', 'sevb'),
         'statue': ('UsetupstatueZ', 'stat'), 'archives': ('UsetuparchZ', 'arch'), 'streets': ('UsetuppeteZ', 'pete'),
         'depot': ('UsetupdepoZ', 'depo'), 'train': ('UsetuptraZ', 'tra'), 'jungle': ('UsetupjunZ', 'jun'),
         'control': ('UsetupcontrolZ', 'arec'), 'caverns': ('UsetupcaveZ', 'cave'), 'cradle': ('UsetupcradZ', 'crad'),
         'aztec': ('UsetupaztZ', 'azt'), 'egyptian': ('UsetupcrypZ', 'cryp')}
SYMS = ('ptr_bgdata_offsets', 'standTileStart', 'PitemZ_entries', 'c_item_entries', 'bcopy', 'memcpy',
        '_inflateSegmentStart', '_inflateSegmentEnd', 'decompressdata', 'zlib_inflate')


def decompressors(syms):
    """The code that inflates files: the boot segment's inflate, which the level's
    files are not read by, and the game's own (decompressdata .. zlib_inflate),
    which inflates the setup, stan and model files into place and reads back its
    own window as it goes - not a use of the data."""
    return [[syms['_inflateSegmentStart'][0], syms['_inflateSegmentEnd'][0]],
            [syms['decompressdata'][0], syms['zlib_inflate'][0] + syms['zlib_inflate'][1]]]


def ssh(cmd, **kw):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', HOST, cmd], **kw)


def symbols():
    out = ssh('nm -S %s' % ELF, capture_output=True, text=True, check=True).stdout
    s = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[-1] in SYMS:
            s[p[-1]] = (int(p[0], 16), int(p[1], 16) if len(p) == 4 else 0)
    missing = [k for k in SYMS if k not in s]
    if missing:
        raise SystemExit('symbols missing from %s: %s' % (ELF, missing))
    return s


def census_files():
    """{stem: bytes} of every file the converter read (the census's own copy of
    the ROM's files), the model stems, and the census's info."""
    import census
    roots, info, ok, msg = census.census(reuse=True, out=CENSUS_OUT)
    files = {k[5:]: r.data for k, r in roots.items() if k.startswith('file:')}
    files['data'] = roots['data'].data
    models = {name for name, _, _ in info['props'] + info['chrs']}
    return files, models, info


def spec_for(m, files, models, syms, frames):
    setup, lvl = FILES[m[1]]
    d = files[setup]
    stan = files['Tbg_%s_all_p_stanZ' % lvl]
    return {'mission': m[1], 'levelid': m[3], 'difficulty': 0, 'frames': frames,
            'common': '~/%s/common' % REMOTE, 'out': '~/%s/out/%s' % (REMOTE, m[1]),
            'syms': {k: v[0] for k, v in syms.items()},
            'exclude': decompressors(syms),
            'copy': [[syms['bcopy'][0], syms['bcopy'][0] + syms['bcopy'][1]],
                     [syms['memcpy'][0], syms['memcpy'][0] + syms['memcpy'][1]]],
            'files': {'setup': {'stem': setup, 'len': len(d), 'pads_off': struct.unpack_from('>I', d, 24)[0]},
                      'bg': {'stem': 'bg_%s_all_p' % lvl, 'len': len(files['bg_%s_all_p' % lvl])},
                      'stan': {'stem': 'Tbg_%s_all_p_stanZ' % lvl, 'len': len(stan),
                               'first': struct.unpack_from('>I', stan, 4)[0]},
                      'models': {s: len(files[s]) for s in models if s in files}}}


def sweep(argv):
    opts = dict(zip(argv[::2], argv[1::2]))
    want = opts.get('--missions')
    ms = [levels.mission(k) for k in want.split(',')] if want else levels.MISSIONS
    frames, jobs = int(opts.get('--frames', 3600)), int(opts.get('--jobs', 3))
    files, models, _ = census_files()
    syms = symbols()
    os.makedirs(OUT, exist_ok=True)
    ssh('mkdir -p ~/%s/specs ~/%s/out' % (REMOTE, REMOTE), check=True)
    subprocess.run(['rsync', '-a', '--delete', '--exclude', '__pycache__', os.path.join(GF, 'common') + '/',
                    '%s:%s/common/' % (HOST, REMOTE)], check=True)
    subprocess.run(['rsync', '-a', os.path.join(HERE, 'readpass.py'), '%s:%s/' % (HOST, REMOTE)], check=True)
    for m in ms:
        sp = spec_for(m, files, models, syms, frames)
        for k in ('common', 'out'):
            sp[k] = sp[k].replace('~', '/home/' + HOST.split('@')[0])
        p = os.path.join(OUT, 'spec-%s.json' % m[1])
        json.dump(sp, open(p, 'w'), indent=1)
        subprocess.run(['rsync', '-a', p, '%s:%s/specs/' % (HOST, REMOTE)], check=True)
    pending, running = list(ms), []
    while pending or running:
        while pending and len(running) < jobs:
            m = pending.pop(0)
            log = open(os.path.join(OUT, 'readpass-%s.log' % m[1]), 'w')
            p = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', HOST,
                                  'cd ~/%s && rm -rf out/%s && timeout -k 10 3600 python3 readpass.py specs/spec-%s.json'
                                  % (REMOTE, m[1], m[1])], stdout=log, stderr=subprocess.STDOUT)
            running.append((m, p, log))
        time.sleep(5)
        for r in list(running):
            if r[1].poll() is not None:
                r[2].close()
                running.remove(r)
                tail = open(os.path.join(OUT, 'readpass-%s.log' % r[0][1])).read().strip().splitlines()[-1:]
                print('%-10s exit %d %s' % (r[0][1], r[1].returncode, tail[0] if tail else ''), flush=True)
                subprocess.run(['rsync', '-a', '%s:%s/out/%s/' % (HOST, REMOTE, r[0][1]),
                                os.path.join(OUT, r[0][1]) + '/'], check=False)
                ssh('rm -rf ~/%s/out/%s' % (REMOTE, r[0][1]))


# ------------------------------------------------------------------ join

def load_dump(path, n):
    b = open(path, 'rb').read()
    flags = b[:n]
    first = struct.unpack('<%dI' % n, b[n:5 * n])
    last = struct.unpack('<%dI' % n, b[5 * n:9 * n])
    return flags, first, last


def resident_bg(d):
    """The bytes of a bg file before its first room blob."""
    rooms = struct.unpack_from('>I', d, 4)[0] & 0xffffff
    offs = []
    i = 0
    while rooms + 24 * i + 24 <= len(d):
        pt, pri, sec = struct.unpack_from('>III', d, rooms + 24 * i)
        offs += [x & 0xffffff for x in (pt, pri, sec) if x]
        if i > 0 and pri == 0:
            break
        i += 1
    return min(offs) if offs else len(d)


def agreement(mem, data):
    nz = [i for i in range(0, len(data), 7) if data[i]]
    if not nz:
        return 1.0
    return sum(1 for i in nz if mem[i] == data[i]) / len(nz)


def funcs():
    """[(addr, size, name)] of the ELF's functions, for naming a reader PC."""
    out = ssh('nm -S -n %s' % ELF, capture_output=True, text=True, check=True).stdout
    fs = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'Tt':
            fs.append((int(p[0], 16), int(p[1], 16), p[3]))
    return fs


def namer(fs):
    import bisect
    starts = [f[0] for f in fs]

    def name(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and fs[i][0] <= pc < fs[i][0] + max(fs[i][1], 4):
            return fs[i][2]
        return '%08x' % pc
    return name


def join(argv):
    import census
    files, models, info = census_files()
    sys.path.insert(0, census.CONV)
    name = namer(funcs())
    dec = decompressors(symbols())
    indec = lambda pc: any(lo <= pc < hi for lo, hi in dec)
    roots = {}          # 'file:stem#mission' -> Root over the file with the cartridge's map
    maps = {}           # key -> (load map, play map, copy map)
    pcs = {}            # key -> (first, last)
    refused, observed = [], collections.Counter()
    datamaps = []
    for m in levels.MISSIONS:
        d = os.path.join(OUT, m[1])
        if not os.path.exists(os.path.join(d, 'meta.json')):
            continue
        meta = json.load(open(os.path.join(d, 'meta.json')))
        for rg in meta['ranges']:
            stem, n = rg['stem'], rg['len']
            data = files.get(stem)
            mem = open(os.path.join(d, stem + '.mem'), 'rb').read()
            if rg['kind'] == 'bg':
                # only the primary part stays in memory: GoldenEye reads each
                # room's compressed blob from the ROM when the room loads
                n = resident_bg(data)
                data, mem = data[:n], mem[:n]
            a = agreement(mem, data)
            if a < 0.5:
                refused.append('%s/%s %.2f' % (m[1], stem, a))
                continue
            flags, first, last = load_dump(os.path.join(d, stem + '.rw'), rg['len'])
            flags, first, last = flags[:n], first[:n], last[:n]
            # runs armed before the game's own inflate was excluded: a byte only
            # the decompressor read (its window) was not read
            flags = bytes((f & ~3) if (f & 3) and indec(first[i]) and indec(last[i]) else f
                          for i, f in enumerate(flags))
            if rg['kind'] == 'data':
                # the data segment: one root over every mission, its reads OR'ed
                datamaps.append((m[1], flags, first, last))
                observed['data'] += 1
                continue
            key = 'file:%s#%s' % (stem, m[1])
            r = census.Root(key, data)
            r.map = bytearray(1 if f & 3 else 0 for f in flags)
            r.how = bytearray(1 if f & 3 else (2 if f & 4 else 0) for f in flags)
            roots[key] = r
            maps[key] = (bytearray(f & 1 for f in flags), bytearray((f >> 1) & 1 for f in flags),
                         bytearray((f >> 2) & 1 for f in flags))
            pcs[key] = (first, last)
            observed[rg['kind']] += 1
    if not roots:
        raise SystemExit('join: no cartridge dumps under %s' % OUT)

    # the null
    key = 'file:UsetupdamZ#dam'
    if key not in roots:
        print('CART NULL FAILED: no Dam dump'); return 4
    r = roots[key]
    d = r.data
    o = struct.unpack_from('>I', d, 24)[0]
    pads = []
    while o + 0x2c <= len(d) and struct.unpack_from('>I', d, o + 36)[0]:
        pads.append(o)
        o += 0x2c
    plink = sum(1 for o in pads if any(r.map[o + 36:o + 40]))
    pos = sum(1 for o in pads if any(r.map[o:o + 12]))
    names_at = struct.unpack_from('>I', d, 32)[0]
    strs = []
    o = names_at
    while names_at and o + 4 <= len(d) and struct.unpack_from('>I', d, o)[0]:
        p = struct.unpack_from('>I', d, o)[0]
        e = d.find(b'\0', p)
        if 0 < p < len(d) and e > p:
            strs.append((p, e))
        o += 4
    strread = sum(sum(r.map[a:b]) for a, b in strs)
    strbytes = sum(b - a for a, b in strs)
    # (3) a read the recompiler's fast path once hid (93% of all reads): Archives'
    # door scale, which GoldenEye certainly reads - g_DoorScale is 0.75 there
    ds = 'not run'
    akey = 'file:UsetuparchZ#archives'
    if akey in roots:
        a = roots[akey]
        o = struct.unpack_from('>I', a.data, 12)[0]
        seen = []
        while o + 4 <= len(a.data) and a.data[o + 3] != 48:
            t = a.data[o + 3]
            if t == 2:
                seen.append(all(a.map[o + 4:o + 8]))
            o += 4 * census.PROPDEFS.get(t, ('', 1, ''))[1]
        ds = '%d of %d DOOR_SCALE scales read' % (sum(seen), len(seen))
        ok_ds = bool(seen) and all(seen)
    else:
        ok_ds = False
    ok = plink == len(pads) and pos * 2 >= len(pads) and strbytes > 0 and strread == 0 and ok_ds
    nullmsg = ('cartridge null: Dam, %d pads: plink read for %d, position for %d; %d bytes of pad-name strings, '
               '%d read; Archives: %s' % (len(pads), plink, pos, strbytes, strread, ds))
    print(nullmsg)
    if not ok:
        print('CART NULL FAILED: the cartridge map cannot be trusted; nothing written')
        return 4

    # census walks of each map: all reads, load only, play only
    info2 = dict(info)

    def walk(which):
        rs = {}
        for k, r0 in roots.items():
            r = census.Root(k, r0.data)
            r.map = r0.map if which is None else maps[k][which]
            r.how = r0.how
            rs[k] = r
        return census.analyse(info2, rs, files_only=True, pcs=pcs if which is None else None,
                              recids=wanted if which is None else None)
    rep = os.path.join(os.path.dirname(census.__file__), 'report.json')
    wanted = {'%s|%s' % (x['kind'], x['field']) for x in json.load(open(rep))['dropped']} if os.path.exists(rep) else set()
    call, cload, cplay = walk(None), walk(0), walk(1)
    if datamaps:
        # the data segment's tables (fog and level rows, prop and character rows,
        # model headers, global AI lists): every mission's reads together
        n = len(files['data'])
        merged = [bytearray(n), bytearray(n), bytearray(n)]
        firstpc = [0] * n
        for _, flags, first, last in datamaps:
            for i in range(min(n, len(flags))):
                f = flags[i]
                if f & 3:
                    merged[0][i] = 1
                    if not firstpc[i]:
                        firstpc[i] = first[i]
                if f & 1:
                    merged[1][i] = 1
                if f & 2:
                    merged[2][i] = 1
        for idx, target in ((0, call), (1, cload), (2, cplay)):
            r = census.Root('data', files['data'])
            r.map = merged[idx]
            c = census.analyse(info2, {'data': r}, pcs={'data': (firstpc, firstpc)} if idx == 0 else None,
                               recids=wanted if idx == 0 else None)
            for kind, fields in c.k.items():
                for fname, st in fields.items():
                    target.k[kind][fname] = st
    out = {}
    for kind, fields in call.k.items():
        for fname, st in fields.items():
            lo = cload.k[kind][fname]
            pl = cplay.k[kind][fname]
            out['%s|%s' % (kind, fname)] = dict(
                records=st['n'], nonzero=st['nonzero'], read=st['rd_any'], read_nz=st['rd_nz'],
                read_load=lo['rd_any'], read_play=pl['rd_any'], files=sorted(st['rd_files']),
                readers=[[name(pc), n] for pc, n in collections.Counter(st['pcs']).most_common(4)],
                recs=sorted(st['rd_recs']))
    stems = sorted({k.split('#')[0][5:] for k in roots} | ({'data'} if datamaps else set()))
    json.dump(dict(null=nullmsg, observed=dict(observed), refused=refused, stems=stems, fields=out),
              open(os.path.join(OUT, 'cart.json'), 'w'), indent=1)
    print('join: %d dumps (%s), %d refused, %d fields -> %s' % (
        len(roots), dict(observed), len(refused), len(out), os.path.join(OUT, 'cart.json')))
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('sweep', 'join'):
        print(__doc__)
        sys.exit(2)
    sys.exit(sweep(sys.argv[2:]) or 0 if sys.argv[1] == 'sweep' else join(sys.argv[2:]))
