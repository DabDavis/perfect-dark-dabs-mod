#!/usr/bin/env python3
"""What the GoldenEye XBLA release draws, read from Xenia's draw log
(PD_DRAWLOG, tools/xblaintro/xenia-drawlog.patch), matched to Bean files' draws.

The release runs a file's stream as it is, so a draw it makes is one of
beanref.py's draws: the same Xenos primitive (4 list, 5 fan, 13 quad list),
the same index count, a vertex fetch constant at the file's .gpu base plus the
vertex buffer's offset and as long as the buffer, and the material's pictures
in the pixel shader's texture fetches (width, height, format). One draw of a
file names a guest base (fetch address - buffer offset); the file's other
draws in that frame must name the same base, which is what settles two files
with a draw of the same shape.

    xdraws.py index OUT/beanindex.pkl [kinds...]       Bean files' draw signatures
    xdraws.py match LOG OUT/beanindex.pkl OUT/drawn.json [--first F --last L]
    xdraws.py merge OUT/release-drawn.json NAME=drawn.json ...   one reference from captures
    xdraws.py frames LOG                                frames and draws per frame

drawn.json: {source: {'frames': [first, last, count], 'base': [...],
'draws': {pc hex: frames seen}}} - every Bean draw the release made, with how
many frames it was seen in.
"""
import collections, json, os, pickle, struct, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import beanref  # noqa: E402

_BASE = [('magic', '<u4'), ('frame', '<u4'), ('init', '<u4'), ('vs', '<u8'), ('ps', '<u8'),
         ('vsc', '<f4', 256), ('psc', '<f4', 64), ('r21', '<u4', 32), ('r22', '<u4', 12)]
REC1 = np.dtype(_BASE)
REC2 = np.dtype(_BASE + [('fetch', '<u4', 192), ('vsfetch', 'u1', 8), ('psfetch', 'u1', 8)])
REC3 = np.dtype(REC2.descr + [('psc192', '<f4', 32)])
MAGIC = {b'PDDL': (REC1, 0x4C444450), b'PDD2': (REC2, 0x32444450), b'PDD3': (REC3, 0x33444450)}


def load(path):
    raw = np.memmap(path, dtype=np.uint8, mode='r')
    rec, magic = MAGIC[bytes(raw[:4])]
    if 'fetch' not in rec.names:
        raise SystemExit('%s: a PDDL log has no fetch constants; capture with the PDD2/PDD3 hook' % path)
    n = len(raw) // rec.itemsize
    recs = np.frombuffer(raw[:n * rec.itemsize], dtype=rec)
    return recs, magic


# ------------------------------------------------------------- the files' side

def _texinfo(c):
    """Per texture index (file order, as the stream numbers them): (w, h, xenos format)."""
    out = []
    for f in c.files:
        b = c.blob(f)
        if len(b) >= 0x28 and b[:8] == b'texture\0':
            w, h = struct.unpack_from('>HH', b, 0x24)
            out.append((w, h, b[0x1b] & 0x3f))
    return out


def build_index(kinds):
    """{(prim, count): [(source, pc, vb size in words, vb offset, frozenset((w, h, fmt)))]}"""
    idx = collections.defaultdict(list)
    files = {}
    root = beanref.FILES
    for kind in kinds:
        d = os.path.join(root, kind)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            src = '%s/%s' % (kind, name)
            if not os.path.exists(os.path.join(d, name, 'default.bin')):
                continue
            try:
                f = beanref.BeanFile(src)
            except Exception:  # noqa: BLE001
                continue
            tex = _texinfo(f.caff)
            n = 0
            for dr in f.draws:
                if dr['vb'] is None or dr['vb'] + 16 > len(f.data):
                    continue
                stride, _, off, size = struct.unpack_from('>4I', f.data, dr['vb'])
                pics = frozenset(tex[t] for t in (dr['tex'] or []) if t < len(tex))
                idx[(dr['prim'], dr['count'])].append((src, dr['pc'], size // 4, off, pics))
                n += 1
            files[src] = {'draws': n, 'tris': sum(x['tris'] for x in f.draws)}
    return {'index': dict(idx), 'files': files}


# -------------------------------------------------------------- the log's side

# Bean's vertex shaders read their stream through fetch constant 95 (D3D's
# stream 0; every binding in the .vb side logs of boot, Dam and Facility), and
# fetch constants keep what an earlier draw left in them, so only the top slots
# are taken - a stale slot would name a buffer this draw never reads
VSLOTS = range(92, 96)


def _fetches(fetch):
    """The record's vertex fetches {(address bytes, size words)} in the stream slots."""
    v = []
    for k in VSLOTS:
        d0, d1 = int(fetch[2 * k]), int(fetch[2 * k + 1])
        if d0 & 3 == 3:
            v.append(((d0 >> 2) << 2, (d1 >> 2) & 0xffffff))
    return v


def _texfetch(fetch, k):
    d = fetch[6 * k:6 * k + 6]
    if int(d[0]) & 3 != 2:
        return None
    fmt = int(d[1]) & 0x3f
    w = (int(d[2]) & 0x1fff) + 1
    h = ((int(d[2]) >> 13) & 0x1fff) + 1
    return (w, h, fmt)


def plant_fake(index, expect):
    """The null's fake file: two of the expected file's draws, their buffers
    moved by different page counts, so no one guest base explains both - it
    must not come out drawn."""
    idx = {k: list(v) for k, v in index['index'].items()}
    mine = [(k, e) for k, v in idx.items() for e in v if e[0] == expect]
    picked, keys = [], set()
    for k, e in mine:
        if k not in keys:
            picked.append((k, e))
            keys.add(k)
        if len(picked) == 2:
            break
    for n, (k, (src, pc, words, off, pics)) in enumerate(picked):
        idx[k].append(('null/fake', pc, words, off + (0x1000 if n == 0 else 0x5000), pics))
    files = dict(index['files'], **{'null/fake': {'draws': 2, 'tris': 0}})
    return {'index': idx, 'files': files}, len(picked) == 2


def match(log, index, first=0, last=1 << 30, chunk=200000):
    recs, magic = load(log)
    idx = index['index']
    keys = set(idx)
    prim = (recs['init'] & 0x3f).astype(np.int64)
    count = (recs['init'] >> 16).astype(np.int64)
    frames = recs['frame']
    keep = np.nonzero((frames >= first) & (frames <= last) & (recs['magic'] == magic))[0]
    # (frame, source, base) -> {pc: hits}; and per record its candidates
    seen = collections.defaultdict(lambda: collections.defaultdict(int))
    scores = collections.Counter()
    ambiguous = 0
    for lo in range(0, len(keep), chunk):
        sel = keep[lo:lo + chunk]
        for i in sel:
            key = (int(prim[i]), int(count[i]))
            if key not in keys:
                continue
            r = recs[i]
            vf = _fetches(r['fetch'])
            pics = set()
            for k in r['psfetch']:
                if k != 0xff:
                    t = _texfetch(r['fetch'], int(k))
                    if t:
                        pics.add(t)
            cands = []
            for src, pc, words, off, want in idx[key]:
                for addr, size in vf:
                    # a file's .gpu sits on a 4 KB page in physical memory
                    if size == words and addr > off and (addr - off) & 0xfff == 0:
                        score = len(want & pics) if want else 0
                        if want and pics and not (want & pics):
                            continue
                        cands.append((score, src, pc, addr - off))
            if not cands:
                continue
            best = max(c[0] for c in cands)
            cands = [c for c in cands if c[0] == best]
            if len(cands) > 1:
                ambiguous += 1
            fr = int(frames[i])
            for score, src, pc, base in cands:
                seen[(fr, src, base)][pc] += 1
                scores[(fr, src, base)] += score
    # A (frame, file, base) with one draw that another file explains better is
    # a coincidence of shape: keep a file's base where it drew two or more
    # draws, or its only draw if no other candidate took that record.
    # One file lives at a base: where several files explain the draws at one
    # (frame, base) - every gun's four flash quads are alike - the file that
    # explains the most draws (then the most pictures) is the one there
    best = {}
    for (fr, src, base), pcs in seen.items():
        k = (fr, base)
        cand = (len(pcs), scores[(fr, src, base)])
        if k not in best or cand > best[k][0]:
            best[k] = (cand, {src})
        elif cand == best[k][0]:
            best[k][1].add(src)
    drawn = collections.defaultdict(lambda: {'frames': set(), 'bases': set(), 'draws': collections.Counter()})
    for (fr, src, base), pcs in seen.items():
        if src not in best[(fr, base)][1]:
            continue
        if len(pcs) < 2 and index['files'].get(src, {}).get('draws', 0) > 1:
            continue
        e = drawn[src]
        e['frames'].add(fr)
        e['bases'].add(base)
        for pc in pcs:
            e['draws'][pc] += 1
    # files the release's draws cannot tell apart (one base, the same draws:
    # the fur hats' three colours), each listed with the others
    tied = collections.defaultdict(set)
    for (fr, base), (cand, srcs) in best.items():
        if len(srcs) > 1:
            for x in srcs:
                tied[x] |= srcs - {x}
    out = {}
    for src, e in sorted(drawn.items()):
        fs = sorted(e['frames'])
        out[src] = {'frames': [fs[0], fs[-1], len(fs)], 'bases': sorted('%x' % b for b in e['bases'])[:8],
                    'draws': {'%x' % pc: n for pc, n in sorted(e['draws'].items())},
                    'tied': sorted(tied.get(src, ()))}
    return out, {'records': int(len(keep)), 'ambiguous': ambiguous,
                 'frames': [int(frames[keep[0]]), int(frames[keep[-1]])] if len(keep) else [0, 0]}


def main():
    cmd = sys.argv[1]
    if cmd == 'index':
        kinds = sys.argv[3:] or ['new/gun', 'new/prop', 'new/char', 'new/head', 'new/background', 'new/skydome',
                                 'original/gun', 'original/prop', 'original/char', 'original/head']
        ix = build_index(kinds)
        pickle.dump(ix, open(sys.argv[2], 'wb'))
        print('%d files, %d draw shapes' % (len(ix['files']), len(ix['index'])))
    elif cmd == 'match':
        log, ixp, outp = sys.argv[2:5]
        first = int(sys.argv[sys.argv.index('--first') + 1]) if '--first' in sys.argv else 0
        last = int(sys.argv[sys.argv.index('--last') + 1]) if '--last' in sys.argv else 1 << 30
        expect = sys.argv[sys.argv.index('--expect') + 1] if '--expect' in sys.argv else None
        ix = pickle.load(open(ixp, 'rb'))
        planted = False
        if expect:
            ix, planted = plant_fake(ix, expect)
        out, stats = match(log, ix, first, last)
        # the null: the expected file drawn (a draw the release certainly makes),
        # the planted fake not
        null = {'expect': expect, 'expected_drawn': bool(expect and expect in out),
                'fake_planted': planted, 'fake_matched': 'null/fake' in out}
        out.pop('null/fake', None)
        stats['null'] = null
        json.dump({'log': log, 'stats': stats, 'drawn': out}, open(outp, 'w'), indent=1)
        print('%s: %d records, %d files drawn, %d ambiguous records; null %s' % (
            log, stats['records'], len(out), stats['ambiguous'], null))
        if expect and (not null['expected_drawn'] or null['fake_matched'] or not planted):
            print('NULL FAILED: %s' % null)
            sys.exit(2)
    elif cmd == 'merge':
        # xdraws.py merge OUT/release-drawn.json NAME=drawn.json ...
        out = {}
        for arg in sys.argv[3:]:
            name, path = arg.split('=', 1)
            d = json.load(open(path))['drawn']
            for src, e in d.items():
                m = out.setdefault(src, {'frames': 0, 'captures': [], 'draws': {}, 'tied': []})
                m['frames'] += e['frames'][2]
                m['captures'].append(name)
                m['tied'] = sorted(set(m['tied']) | set(e.get('tied', [])))
                for pc, n in e['draws'].items():
                    m['draws'][pc] = m['draws'].get(pc, 0) + n
        json.dump(out, open(sys.argv[2], 'w'), indent=1, sort_keys=True)
        print('%d files the release was seen drawing' % len(out))
    elif cmd == 'frames':
        recs, _ = load(sys.argv[2])
        fr, n = np.unique(recs['frame'], return_counts=True)
        for a, b in zip(fr[::max(1, len(fr) // 40)], n[::max(1, len(fr) // 40)]):
            print(a, b)


if __name__ == '__main__':
    main()
