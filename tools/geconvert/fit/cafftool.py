#!/usr/bin/env python3
"""Rare CAFF 07.08.06.0036 reader for Project Bean (GoldenEye XBLA) files.

Research tool. Layout from CAFFeinated's BundleV36 (OlieGamerTV) and
project-grabbed (x1nixmzeng), checked against Bean's files.
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
sys.path.insert(0, paths.TEXPACK)
import numpy as np
import x360

FMT = {0x52: ('DXT1', 1, 8), 0x53: ('DXT3', 3, 16), 0x54: ('DXT5', 5, 16),
       0x86: ('A8R8G8B8', 0, 4)}


class Caff:
    def __init__(self, path):
        self.path = path
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'CAFF', path
        self.version = d[4:0x14].split(b'\0')[0].decode()
        self.be = d[0x48] == 1
        e = '>' if self.be else '<'
        u = lambda o: struct.unpack_from(e + 'I', d, o)[0]
        self.hsize = u(0x14)
        self.nassets = u(0x1c)
        self.nfiles = u(0x20)
        self.nsect = d[0x49]
        self.compression = d[0x4a]
        namelen = u(0x4c)
        sect_size, file_size = u(0x50), u(0x64)
        pos = self.hsize
        ents = []
        for i in range(self.nsect):
            ents.append(dict(stroff=u(pos), align=d[pos + 4], pool=u(pos + 5),
                             size=u(pos + 9), csize=u(pos + 0x1d)))
            pos += 0x21
        base = pos
        for s in ents:
            s['name'] = d[base + s['stroff']:].split(b'\0')[0].decode()
        pos = base + namelen
        total = u(pos)
        offs = [u(pos + 4 + 4 * i) for i in range(self.nassets)]
        lab = pos + 4 + 4 * self.nassets
        self.names = [d[lab + o:].split(b'\0')[0].decode(errors='replace') for o in offs]
        pos = lab + total
        adb = u(pos)
        pos += 4 + adb
        self.files = []
        for i in range(self.nfiles):
            a, st, sz = struct.unpack_from(e + 'III', d, pos)
            self.files.append(dict(asset=a, start=st, size=sz, sect=d[pos + 12], align=d[pos + 13]))
            pos += 14
        self.tables_end = pos
        # Section data follows the two tables, in section order.
        off = self.hsize + sect_size + file_size
        for s in ents:
            s['offset'] = off
            off += s['size']
        self.sections = ents
        self.data_end = off

    def blob(self, f):
        s = self.sections[f['sect'] - 1]
        o = s['offset'] + f['start']
        return self.d[o:o + f['size']]

    def asset_name(self, f):
        n = self.names[f['asset'] - 1] if 0 < f['asset'] <= len(self.names) else '?%d' % f['asset']
        return n.split('\\')[-1]

    def summary(self):
        print('%s: %s be=%d assets=%d files=%d comp=%d tables_end=%x data_end=%x filesize=%x' % (
            self.path, self.version, self.be, self.nassets, self.nfiles, self.compression,
            self.tables_end, self.data_end, len(self.d)))
        for s in self.sections:
            print('  section %-8s size %8x offset %8x align %d pool %d' % (s['name'], s['size'], s['offset'], s['align'], s['pool']))
        for f in self.files:
            b = self.blob(f)
            print('  file asset %2d %-26s %-8s start %8x size %8x  %s' % (
                f['asset'], self.asset_name(f)[:26], self.sections[f['sect'] - 1]['name'],
                f['start'], f['size'], b[:16].hex()))

    def textures(self):
        """(name, width, height, fmtname, rgba) for every texture asset."""
        out = []
        for f in self.files:
            b = self.blob(f)
            if not b.startswith(b'texture\0'):
                continue
            gpu = [g for g in self.files if g['asset'] == f['asset'] and
                   self.sections[g['sect'] - 1]['name'].startswith('.gpu')]
            fmtword, = struct.unpack_from('>I', b, 0x18)
            swizzled = b[0x1a]
            fmt = b[0x1b]
            w, h = struct.unpack_from('>HH', b, 0x24)
            # Base and mip offsets: base is 0 in a texture's own .gpu entry,
            # and where it has none the pixels are in the file's shared
            # "texture pairs" .gpu asset at the base offset (Oddjob's body
            # and hat: 512x512 at 0, 128x128 at 0x20000).
            base, = struct.unpack_from('>I', b, 0x28)
            frames, tabpos = struct.unpack_from('>II', b, 0x38)
            name = self.asset_name(f)
            if not gpu:
                pairs = [g for g in self.files if self.asset_name(g) == 'texture pairs' and
                         self.sections[g['sect'] - 1]['name'].startswith('.gpu')]
                gpu = pairs
            else:
                base = 0
            if fmt not in FMT or not gpu:
                out.append((name, w, h, '%02x' % fmt, None))
                continue
            fname, kind, bpb = FMT[fmt]
            first = struct.unpack_from('>I', b, tabpos)[0] if frames else 0
            px = self.blob(gpu[0])[base + first:]
            src = x360.endian_swap(px, 1 if kind else 2) if swizzled else np.frombuffer(px, np.uint8)
            # The surfaces are tiled: whole 32 element tiles across and down.
            # 16 texels in, and only where there are mips to be packed behind
            # it; byte 0x30 is the level count. Same rule as x360DecodeTexture().
            packed = b[0x30] > 1
            ox, oy = x360.packed_mip_offset(w, h, 4 if kind else 1) if packed else (0, 0)
            if kind:
                bw, bh = -(-w // 4), -(-h // 4)
                ew, eh = -(-bw // 32) * 32, -(-bh // 32) * 32
                grid = x360._linear(src, ew, eh, bpb, True)
                rgba = x360._decode_dxt(np.ascontiguousarray(grid[oy:oy + bh, ox:ox + bw]), w, h, kind)
            else:
                ew, eh = -(-w // 32) * 32, -(-h // 32) * 32
                grid = x360._linear(src, ew, eh, 4, True)
                rgba = np.ascontiguousarray(grid[oy:oy + h, ox:ox + w][:, :, [2, 1, 0, 3]])
            out.append((name, w, h, fname, rgba))
        return out


if __name__ == '__main__':
    from PIL import Image
    outdir = os.environ.get('CAFF_OUT')
    for p in sys.argv[1:]:
        c = Caff(p)
        c.summary()
        for name, w, h, fmt, rgba in c.textures():
            print('  texture %-30s %4dx%-4d %s' % (name, w, h, fmt))
            if outdir and rgba is not None:
                tag = p.replace('/', '_').replace('default.', '')
                Image.fromarray(rgba, 'RGBA').save(os.path.join(outdir, '%s_%s.png' % (tag, name)))
