#!/usr/bin/env python3
"""What a GoldenEye XBLA (Bean) model file holds, read independently of the game.

The file's `.stream` is a list of records {u16 size, u8 type, ...}. The ones
that matter here:

    0x16  a switch: {count at +4, then count targets}; every target runs to a
          0x19 that jumps to the join. Bean's models switch vertex shader,
          vertex buffer and material this way; the game always takes the
          first target (beanWalkStream()).
    0x19  a jump {target}
    0x17  a section {kind, end}: pieces a model can turn on and off (a gun's
          trigger finger, its shells; a head's sunglasses in the originals)
    0x2d  a material {texture, slot} pairs from +12
    0x01  a draw {prim, count, index buffer}
    0x30  a draw with a fourth word: the piece it belongs to
    0x1d  the end

Every draw is classed by how it is reached:
    'main'  on the path the game walks (first target of every switch)
    'alt'   only through a switch's later target
    'dead'  in the stream but on no path at all

    beanref.py new/gun/ppk [...]          print one file's draws
    beanref.py --json new/gun/ppk         as JSON
"""
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'geconvert', 'fit'))
from cafftool import Caff  # noqa: E402

BEAN = os.environ.get('GEFIT_BEAN', os.path.expanduser('~/perfect-dark/.xbla-work/ge-bean/Bean'))


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


class BeanFile:
    def __init__(self, source, root=BEAN, path=None):
        self.source = source
        self.path = path or os.path.join(root, 'files', source, 'default.bin')
        c = self.caff = Caff(self.path)
        get = lambda a, s: [c.blob(f) for f in c.files if f['asset'] == a and c.sections[f['sect'] - 1]['name'] == s]
        self.stream = (get(1, '.stream') or [b''])[0]
        self.data = (get(1, '.data') or [b''])[0]
        self.gpu = (get(1, '.gpu') or [b''])[0]
        self.texnames = []
        for f in c.files:
            b = c.blob(f)
            if len(b) >= 8 and b[:8] == b'texture\0':
                self.texnames.append(c.asset_name(f))
        self.ibs = self._index_buffers()
        self.draws = []
        self._walk()

    def _index_buffers(self):
        """Index buffer objects in .data: {obj offset: (count of indices, width)}.

        The game finds them by their D3D header; here only their size is
        wanted, to count triangles, and a draw's own count is used instead
        where the buffer is not found."""
        return {}

    def _records(self):
        st = self.stream
        if len(st) < 0x28:
            return {}
        end = min(u32(st, 4), len(st))
        recs = {}
        pc = 0x24
        while pc + 4 <= end:
            tag = u32(st, pc)
            size = tag >> 16
            if size < 4 or pc + size > len(st):
                break
            recs[pc] = ((tag >> 8) & 0xff, size)
            pc += size
        self.end = end
        return recs

    def _walk(self):
        st = self.stream
        recs = self._records()
        if not recs:
            return
        reach = {}            # pc -> 'main' or 'alt'
        # (pc, how, section kinds open, material texture)
        self._vb = {}
        todo = [(0x24, 'main', (), None)]
        seen = set()
        while todo:
            pc, how, sections, tex = todo.pop()
            steps = 0
            while pc in recs and steps < 200000:
                steps += 1
                key = (pc, how, sections, tex)
                if key in seen:
                    break
                seen.add(key)
                typ, size = recs[pc]
                if reach.get(pc) != 'main':
                    reach[pc] = how
                sections = tuple(s for s in sections if pc < s[1])
                if typ == 0x1d:
                    break
                if typ == 0x19 and size >= 8:
                    pc = u32(st, pc + 4)
                    continue
                if typ == 0x16 and size >= 12:
                    n = st[pc + 4] if st[pc + 4] else 1
                    targets = [u32(st, pc + 8 + 4 * k) for k in range(n) if pc + 12 + 4 * k <= pc + size]
                    for k, t in enumerate(targets[1:], 1):
                        todo.append((t, 'alt', sections, tex))
                    pc = targets[0] if targets else pc + size
                    continue
                if typ == 0x17 and size >= 12:
                    sections = sections + ((u32(st, pc + 4), u32(st, pc + 8), pc),)
                elif typ == 0x2d and size >= 20:
                    # every picture the material binds ({texture, slot} pairs);
                    # which one the game draws with is beanMaterialTexture()'s call
                    tex = tuple(u32(st, pc + 12 + 8 * k) for k in range((size - 12) // 8))
                elif typ == 0x2e and size >= 12:
                    self._curvb = u32(st, pc + 8)
                elif typ in (0x01, 0x30) and size >= 16:
                    piece = u32(st, pc + 16) if typ == 0x30 and size >= 20 else -1
                    if piece >= 0x80000000:
                        piece -= 1 << 32
                    prim, count = u32(st, pc + 4), u32(st, pc + 8)
                    self._add_draw(pc, how, sections, tex, typ, prim, count, u32(st, pc + 12), piece)
                pc += size
        # draws no path reaches
        for pc, (typ, size) in sorted(recs.items()):
            if typ in (0x01, 0x30) and size >= 16 and pc not in reach:
                piece = u32(st, pc + 16) if typ == 0x30 and size >= 20 else -1
                if piece >= 0x80000000:
                    piece -= 1 << 32
                self._add_draw(pc, 'dead', (), None, typ, u32(st, pc + 4), u32(st, pc + 8), u32(st, pc + 12), piece)
        # a draw reached both ways is 'main'
        best = {}
        for d in self.draws:
            o = best.get(d['pc'])
            if o is None or (o['how'] != 'main' and d['how'] == 'main'):
                best[d['pc']] = d
        self.draws = sorted(best.values(), key=lambda d: d['pc'])

    def _add_draw(self, pc, how, sections, tex, typ, prim, count, ib, piece):
        # Xenos primitives: 4 list, 5 fan, 6 strip, 8 rectangle list, 13 quad list
        tris = {4: count // 3, 5: max(count - 2, 0), 6: max(count - 2, 0), 8: (count // 3) * 2,
                13: (count // 4) * 2}.get(prim, 0)
        self.draws.append({
            'pc': pc, 'how': how, 'type': typ, 'prim': prim, 'count': count, 'ib': ib, 'tris': tris,
            'piece': piece, 'sections': [s[0] for s in sections], 'vb': getattr(self, '_curvb', None),
            'tex': list(tex) if tex else None,
            'texname': '+'.join(self.texnames[t] if t < len(self.texnames) else '#%d' % t for t in tex) if tex else None})

    def where(self, d):
        """A draw's height in the model, as the share of the model's own
        height its vertices span (0 the bottom, 1 the top): 'head' for a body's
        own head reads as 0.8-1.0. None when its vertex buffer is not read."""
        try:
            if not hasattr(self, '_m'):
                import bean2obj
                self._m = bean2obj.Model(self.path)
                ys = []
                for e in self.draws:
                    if e['vb'] is not None:
                        vs = self._m.vertices(e['vb'])
                        ys += [vs[i]['pos'][1] for i in self._m.indices(e['ib'], e['count']) if i < len(vs)]
                self._ylo, self._yhi = min(ys), max(ys)
            vs = self._m.vertices(d['vb'])
            ys = [vs[i]['pos'][1] for i in self._m.indices(d['ib'], d['count']) if i < len(vs)]
            h = (self._yhi - self._ylo) or 1.0
            return (round((min(ys) - self._ylo) / h, 2), round((max(ys) - self._ylo) / h, 2))
        except Exception:  # noqa: BLE001
            return None

    def summary(self):
        out = {'source': self.source, 'draws': len(self.draws), 'textures': len(self.texnames)}
        for how in ('main', 'alt', 'dead'):
            ds = [d for d in self.draws if d['how'] == how]
            out[how] = {'draws': len(ds), 'tris': sum(d['tris'] for d in ds)}
        return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for src in args:
        f = BeanFile(src)
        if '--json' in sys.argv:
            print(json.dumps({'summary': f.summary(), 'draws': f.draws}, indent=1))
            continue
        print(f.summary())
        for d in f.draws:
            print('  %5x %-4s type %02x prim %d tris %5d piece %2d sections %s tex %s' % (
                d['pc'], d['how'], d['type'], d['prim'], d['tris'], d['piece'], d['sections'], d['texname']))


if __name__ == '__main__':
    main()
