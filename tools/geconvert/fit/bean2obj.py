#!/usr/bin/env python3
"""Project Bean rendergraph -> OBJ + MTL + PNG textures (research tool).

Walks the model's command stream (the first alternative at every switch),
follows the vertex buffer, texture and bone palette records, and writes one
OBJ group per draw. Positions stay in the file's own units.

Stream records are tagged u32 (size << 16 | type << 8):
  0x2e  bind vertex buffer + shader: {next, vbdesc, 0x10000, 0, shader, 0}
  0x2d  material: {state, n<<16|?, texture index, ...}
  0x13  bone palette: {next, u16 count, u16 0, count bytes of bone numbers}
  0x01  draw: {D3D primitive (4 tri list, 5 strip, 13 quad list), index count, index buffer}
  0x16  switch: {n<<24, n child addresses}   0x19 jump: {address}   0x1d end
Vertex buffer descriptor (.data): {stride, object, .gpu offset, byte size}.
Index buffer object (.data): the descriptor is the 12 bytes before it,
{.gpu offset, byte size, 1}.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cafftool import Caff


def unpack_normal(w):
    def s10(v):
        return v - 1024 if v & 512 else v
    return (s10(w & 1023) / 511.0, s10((w >> 10) & 1023) / 511.0, s10((w >> 20) & 1023) / 511.0)


class Model:
    def __init__(self, path):
        c = self.caff = Caff(path)
        get = lambda a, s: [c.blob(f) for f in c.files
                            if f['asset'] == a and c.sections[f['sect'] - 1]['name'] == s]
        self.data = get(1, '.data')[0]
        self.gpu = get(1, '.gpu')[0]
        self.stream = get(1, '.stream')[0]
        pool = get(2, '.data')
        self.pool = [s.decode(errors='replace') for s in pool[0].split(b'\0') if s] if pool else []
        self.bones = [s for s in self.pool if s.startswith('SKEL_')]
        self.textures = [t for t in c.textures()]
        self.draws = []
        self.remap = None
        self.pose = self.read_pose()
        self.walk()
        self.uvscale = self.measure_uvscale()

    def measure_uvscale(self):
        """What one texture repeat is in this file's s16 UVs: 16384 or 32768.

        Rare exported with both and nothing in the stream, the shader inputs
        or the shaders' literals says which. Over every character and head
        the largest UV is either at most 16404 (the uniformed guards, pilot,
        Xenia, Boris, six heads, the crate prop) or at least 32031, so a file
        whose UVs never pass 17000 is in sixteen-thousandths. Divided by
        32768 those sampled only the top-left quarter of their atlas - the
        black guards and Boris's patchwork.
        """
        g = self.gpu
        biggest = 0
        for vb in set(d['vb'] for d in self.draws if d['vb'] is not None):
            stride, _, off, size = struct.unpack_from('>4I', self.data, vb)
            uvo = {36: 28, 32: 24, 28: 24, 24: 16}.get(stride)
            if uvo is None:
                continue
            if stride == 28 and all(g[off + i * 28 + 24] == 0xff for i in range(size // 28)):
                continue
            for i in range(size // stride):
                u, v = struct.unpack_from('>hh', g, off + i * stride + uvo)
                biggest = max(biggest, abs(u), abs(v))
        return 16384.0 if 0 < biggest <= 17000 else 32768.0

    def read_pose(self):
        """The skeleton: {local translation, absolute bind position, parent}.

        A 'pose' record (version 19.12.06.0036) holds a count at +0x18 and an
        entry array at the address in +0x34; an entry is 52 bytes: local
        xyz, absolute xyz, a spare xyz, 1.0, {parent, first child},
        {next sibling, self}, {0xffff, ?}. Bones are in SKEL_* name order.
        """
        d = self.data
        at = d.find(b'pose\0\0\0\0')
        if at < 0:
            return []
        count = self.u32(d, at + 0x18)
        entries = self.u32(d, at + 0x34)
        bones = []
        for i in range(count):
            o = entries + 52 * i
            local = struct.unpack_from('>3f', d, o)
            bind = struct.unpack_from('>3f', d, o + 12)
            parent, child, sibling, me = struct.unpack_from('>4H', d, o + 40)
            bones.append(dict(local=local, bind=bind, parent=None if parent == 0xffff else parent,
                              index=me, name=self.bones[i] if i < len(self.bones) else '?'))
        return bones

    def u32(self, blob, o):
        return struct.unpack_from('>I', blob, o)[0]

    def walk(self):
        st = self.stream
        end = self.u32(st, 4)
        pc, vb, tex, pal = 0x24, None, 0, [0]
        seen = 0
        while pc < end and seen < 100000:
            seen += 1
            tag = self.u32(st, pc)
            size, typ = tag >> 16, (tag >> 8) & 0xff
            if size < 4:
                break
            if typ == 0x16:
                n = tag & 0xff if tag & 0xff else st[pc + 4]
                pc = self.u32(st, pc + 8)
                continue
            if typ == 0x19:
                pc = self.u32(st, pc + 4)
                continue
            if typ == 0x1d:
                break
            if typ == 0x12:
                # Palette numbers -> pose bones: {u16 count, 0, count x {u16 bone, 0xffff, 4 bytes}}.
                count = struct.unpack_from('>H', st, pc + 8)[0]
                self.remap = [struct.unpack_from('>H', st, pc + 12 + 8 * k)[0] for k in range(count)]
            elif typ == 0x2e:
                vb = self.u32(st, pc + 8)
            elif typ == 0x2d:
                tex = self.u32(st, pc + 12) if size == 20 else self.u32(st, pc + 20)
            elif typ == 0x13:
                count = struct.unpack_from('>H', st, pc + 8)[0]
                pal = list(st[pc + 12:pc + 12 + count])
            elif typ == 0x01:
                prim, count, ib = struct.unpack_from('>III', st, pc + 4)
                self.draws.append(dict(vb=vb, tex=tex, pal=pal, prim=prim, count=count, ib=ib))
            pc += size

    def vertices(self, vbdesc):
        stride, _, off, size = struct.unpack_from('>4I', self.data, vbdesc)
        g = self.gpu
        out = []
        # Stride 28 is a skinned vertex with one of UV or colour: a colour's
        # alpha byte is 0xff on every vertex, a UV's high byte is not.
        has_col28 = stride == 28 and all(g[off + i * stride + 24] == 0xff for i in range(size // stride))
        for i in range(size // stride):
            o = off + i * stride
            pos = struct.unpack_from('>3f', g, o)
            if stride == 28:
                slots = struct.unpack_from('>4H', g, o + 12)
                bones = [s // 3 if s != 0xf000 else None for s in slots]
                nrm = unpack_normal(self.u32(g, o + 20))
                if has_col28:
                    u, v, col = 0, 0, self.u32(g, o + 24)
                else:
                    (u, v), col = struct.unpack_from('>hh', g, o + 24), 0xffffffff
                out.append(dict(pos=pos, nrm=nrm, uv=(u / self.uvscale, v / self.uvscale), col=col,
                                bones=bones, weights=[255, 0, 0, 0]))
                continue
            if stride in (32, 36):
                slots = struct.unpack_from('>4H', g, o + 12)
                if stride == 36:
                    wb = g[o + 20:o + 24]
                    weights = [wb[3], wb[2], wb[1], wb[0]]
                    k = o + 24
                else:
                    weights = [255, 0, 0, 0]
                    k = o + 20
                bones = [s // 3 if s != 0xf000 else None for s in slots]
            elif stride in (20, 24):
                bones, weights, k = [None] * 4, [255, 0, 0, 0], o + 12
            else:
                raise ValueError('stride %d not handled' % stride)
            nrm = unpack_normal(self.u32(g, k))
            if stride == 20:
                # Position, normal, colour: the untextured spans.
                u, v = 0, 0
                col = self.u32(g, k + 4)
            else:
                u, v = struct.unpack_from('>hh', g, k + 4)
                col = self.u32(g, k + 8)
            out.append(dict(pos=pos, nrm=nrm, uv=(u / self.uvscale, v / self.uvscale), col=col,
                            bones=bones, weights=weights))
        return out

    def indices(self, ibobj, count):
        # The object is runtime storage; its descriptor follows the table
        # word that points at it: {object, .gpu offset, byte size, 1}.
        if not hasattr(self, 'ibtable'):
            self.ibtable = {}
            d = self.data
            for o in range(0, len(d) - 15, 4):
                obj, off, size, one = struct.unpack_from('>4I', d, o)
                if one == 1 and size and size % 2 == 0 and off + size <= len(self.gpu) and obj < len(d):
                    self.ibtable.setdefault(obj, (off, size))
        off, size = self.ibtable[ibobj]
        assert count * 2 <= size, (hex(ibobj), count, size)
        return struct.unpack_from('>%dH' % count, self.gpu, off)

    def triangles(self, d):
        idx = self.indices(d['ib'], d['count'])
        if d['prim'] == 4:
            return [idx[i:i + 3] for i in range(0, len(idx) - 2, 3)]
        if d['prim'] == 13:
            tris = []
            for i in range(0, len(idx) - 3, 4):
                a, b, c, e = idx[i:i + 4]
                tris += [(a, b, c), (a, c, e)]
            return tris
        if d['prim'] == 5:
            return [(idx[i], idx[i + 1], idx[i + 2]) if i % 2 == 0 else (idx[i + 1], idx[i], idx[i + 2])
                    for i in range(len(idx) - 2)]
        raise ValueError('primitive %d' % d['prim'])


def skin_bone(m, d, slot):
    """A vertex slot's palette entry -> pose bone index."""
    p = d['pal'][slot]
    return m.remap[p] if m.remap and p < len(m.remap) else p


def posed_positions(m, verts, d, rot):
    """Skin verts with rot = {bone index: 3x3 local rotation}; bind is translation-only."""
    import numpy as np
    world = {}
    def mat(b):
        if b in world:
            return world[b]
        bone = m.pose[b]
        local = np.eye(4)
        local[:3, 3] = bone['local']
        if b in rot:
            local[:3, :3] = rot[b]
        w = local if bone['parent'] is None else mat(bone['parent']) @ local
        world[b] = w
        return w
    out = []
    for v in verts:
        acc = np.zeros(3)
        total = 0
        # A buffer is shared by draws with different palettes; a vertex of
        # another draw can name slots past this one's, and is not drawn here.
        if any(b is not None and b >= len(d['pal']) for b in v['bones']):
            out.append(v['pos'])
            continue
        for s in range(4):
            if v['bones'][s] is None or v['weights'][s] == 0:
                continue
            b = skin_bone(m, d, v['bones'][s])
            skin = mat(b).copy()
            skin[:3, 3] -= skin[:3, :3] @ np.array(m.pose[b]['bind'])
            acc += v['weights'][s] * (skin @ np.array(list(v['pos']) + [1.0]))[:3]
            total += v['weights'][s]
        out.append(tuple(acc / total) if total else v['pos'])
    return out


def export(path, outdir, name, rot=None):
    from PIL import Image
    m = Model(path)
    os.makedirs(outdir, exist_ok=True)
    texnames = []
    for i, (tn, w, h, fmt, rgba) in enumerate(m.textures):
        fn = '%s_tex%d.png' % (name, i)
        if rgba is not None:
            Image.fromarray(rgba, 'RGBA').save(os.path.join(outdir, fn))
        texnames.append(fn)
    with open(os.path.join(outdir, name + '.mtl'), 'w') as f:
        for i, fn in enumerate(texnames):
            f.write('newmtl tex%d\nKd 1 1 1\nmap_Kd %s\n\n' % (i, fn))
    vcache = {}
    lines = ['mtllib %s.mtl' % name]
    base = 1
    for n, d in enumerate(m.draws):
        key = (d['vb'], tuple(d['pal'])) if rot else d['vb']
        if key not in vcache:
            verts = m.vertices(d['vb'])
            vcache[key] = (base, verts)
            pos = posed_positions(m, verts, d, rot) if rot else [v['pos'] for v in verts]
            for v, p in zip(verts, pos):
                lines.append('v %.4f %.4f %.4f' % tuple(p))
                lines.append('vt %.6f %.6f' % (v['uv'][0], 1 - v['uv'][1]))
                lines.append('vn %.4f %.4f %.4f' % v['nrm'])
            base += len(verts)
        b0, verts = vcache[key]
        lines.append('g draw%02d_vb%x_prim%d_pal%s' % (n, d['vb'], d['prim'], '-'.join(map(str, d['pal']))))
        lines.append('usemtl tex%d' % d['tex'])
        for t in m.triangles(d):
            lines.append('f ' + ' '.join('%d/%d/%d' % (b0 + i, b0 + i, b0 + i) for i in t))
    with open(os.path.join(outdir, name + '.obj'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return m


if __name__ == '__main__':
    src, outdir = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(os.path.dirname(src))
    rot = None
    if len(sys.argv) > 4:
        # Pose test: BONE=DEGREES,... bends each bone about z.
        import math
        import numpy as np
        rot = {}
        probe = Model(src)
        for item in sys.argv[4].split(','):
            bname, deg = item.split('=')
            a = math.radians(float(deg))
            rot[probe.bones.index(bname)] = np.array([[math.cos(a), -math.sin(a), 0],
                                                      [math.sin(a), math.cos(a), 0], [0, 0, 1]])
    m = export(src, outdir, name, rot)
    print('%s: %d draws, %d textures, bones %s' % (src, len(m.draws), len(m.textures), m.bones))
    print('remap', m.remap)
    for b in m.pose:
        print('  pose %2d %-18s parent %-4s local %s bind %s' % (b['index'], b['name'], b['parent'],
              ' '.join('%.0f' % x for x in b['local']), ' '.join('%.0f' % x for x in b['bind'])))
    for d in m.draws:
        print('  vb %x tex %d prim %2d count %5d pal %s' % (d['vb'], d['tex'], d['prim'], d['count'], d['pal']))
