#!/usr/bin/env python3

"""
Reads the XBLA release's high resolution meshes, and writes them out as OBJ.

These are the "improved models" people mean. They are not Perfect Dark model
files; port/src/xblamesh.c draws the unskinned ones in the game and
CLAUDE-notes/xbla.md says where the rest of that stands. This exists so the
geometry can be looked at, which is the only way to check any of the parsing,
and so the game's version has something to be compared against.

Lives beside xblaconvert.py because both read the same containers through
x360.py, not because a mesh has anything to do with a texture pack.

The format, all of which is checked against every one of the 595 files that
parse (see "How this was confirmed" below):

    u32   vertexCount
    u32   vertexOffset
    u32   indexOffset
    u32   drawCount
    u32   drawOffset
    u32   matrixCount
    f32   unknown           100.0 on every unskinned mesh, 1000.0 on nearly
                            every skinned one
    u32   groupOffset       == 32 + 48 * matrixCount

    f32   matrices[matrixCount][3][4]        row major, translation in column 3
    {u32 firstDraw, drawCount, matrixIndex}  groups, up to drawOffset
    {u32 firstTri, triCount, material}       draws, drawCount of them
    vertex vertices[vertexCount]             stride 36 or 48
    u16   indices[]                          to the end of the file

A vertex is position (3 floats), UV (2), normal (3, unit length), colour
(one u32). A skinned one adds two blend weights and a packed
{bone0, bone1, bone2, influenceCount} byte quad. There are only the two
weights: they sum to 1.0 on every skinned vertex in the release, so there is
no 1 - w0 - w1 third one, and the count runs 1 to 6 regardless. Stride 36 means unskinned and matrixCount is then always 0;
stride 48 means skinned and matrixCount is then always non-zero.

The material word is a texture record index in Textures.raw in bits 0-12
(they run 3741 to 5746, which is inside the 2244 records past the game's own
3503 - the meshes use the console's own art, never the numbered replacements),
bit 15 set when that texture has alpha, and two bytes above that whose meaning
is not known: bits 16-23 hold a value from 0 to 100 and bits 24-31 a small
ordinal 0-4 that is zero on all but 15 of the 761 distinct materials.

The vertices are in the mesh's own space already. The matrix palette is not a
transform to apply to them - doing that folds a character in half. It is what
a skinned draw needs alongside a pose, and a still render wants none of it.

How this was confirmed. Every claim above holds over all 595 files at once,
which is what separates it from a plausible read of one file:

  * vertexOffset == drawOffset + 12 * drawCount, and
    groupOffset == 32 + 48 * matrixCount, in all 595.
  * The draws' triCount sums to exactly the file's index count in all 595, and
    their firstTri chains from 0 with no gap or overlap.
  * The groups tile the draw list exactly - first group at draw 0, each one
    starting where the last ended, the last ending at drawCount - in all 595.
  * Every group's matrixIndex is less than matrixCount (one file has
    0xFFFFFFFF with no matrices at all).
  * Rendering a mesh with each draw sampling its own texture at the decoded
    UVs gives recognisable, correctly placed art: shoes on the feet, hair on
    the head, a screen on the laptop. Getting the submesh split, the material
    field or the UV pair wrong gives a smeared mess instead.
  * The slot a mesh id names is the id minus one. Of the models with one
    unskinned mesh between them, 91 have a mesh whose bounding box matches the
    nodes it replaces to within 3%, and every one of them matches at minus one
    - none at all match at the id itself. Rendering agrees: all eleven
    `P*chair*Z` models come out as chairs at minus one, and `Cdark_combatZ` as
    Joanna in combat gear rather than as the evening dress one slot up.

Usage:

    tools/texpack/xblamesh.py --package "/path/to/Perfect Dark XBLA.7z" --list
    tools/texpack/xblamesh.py --package ... --info 2370
    tools/texpack/xblamesh.py --package ... --mesh-id 0x943 --textures --out jo.obj

A slot is a PackedSegFile slot. A model node's mesh id names one as
(part << 12) | (slot + 1), sitting in the two padding bytes after struct
modelnode's type - the low 12 bits are a *file id*, and slot i is file id
i + 1 the way the rest of PackedSegFile works, so the mesh is one slot below
the number in the node. --mesh-id takes the number as the node carries it and
does the subtraction; --slot takes the slot itself.

Every part of a model carries the id of the one mesh that replaces the whole
model, and the part number is that node's entry in the mesh's matrix palette.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import x360
import xblaconvert

PACKAGE_PATH = 'DataFiles/PackedSegFile'
TEXTURE_PATH = 'DataFiles/Textures.raw'

# The fixed part of a mesh header, before the matrix palette.
MESH_HEADER = 32

# Every table past the header is three words wide.
MESH_ENTRY = 12

# A matrix is three rows of four floats.
MESH_MATRIX = 48

# The material word: a Textures.raw record index, and a flag saying that
# record has an alpha channel.
MATERIAL_TEXTURE = 0x1FFF
MATERIAL_ALPHA = 0x8000


class PackedSeg:
    """PackedSegFile: {count, count x 16 byte records, data}."""

    def __init__(self, raw):
        self.raw = raw
        self.count = struct.unpack('>I', raw[:4])[0]
        self.records = [struct.unpack('>4I', raw[4 + i * 16:20 + i * 16])
                        for i in range(self.count)]

    def read(self, i):
        offset, usize, csize, _flags = self.records[i]
        # An unused slot holds leftover bytes rather than zeros, and gives
        # itself away by having no uncompressed size.
        if usize == 0:
            return None
        if csize == 0:
            return self.raw[offset:offset + usize]
        return x360.lzx_decompress(self.raw, offset, csize, usize)


class Mesh:
    def __init__(self, slot, data):
        self.slot = slot
        self.data = data

        if len(data) < MESH_HEADER:
            raise ValueError('slot %d: %d bytes is too short for a header'
                             % (slot, len(data)))

        (self.count, self.vertex_offset, self.index_offset, self.num_draws,
         self.draw_offset, self.num_matrices, self.unknown,
         self.group_offset) = struct.unpack('>6IfI', data[:MESH_HEADER])

        if self.count == 0 or self.vertex_offset < MESH_HEADER:
            raise ValueError('slot %d: not a mesh (count %d, vertex offset %d)'
                             % (slot, self.count, self.vertex_offset))

        if self.index_offset <= self.vertex_offset or self.index_offset > len(data):
            raise ValueError('slot %d: vertices %d..%d do not fit in %d bytes'
                             % (slot, self.vertex_offset, self.index_offset, len(data)))

        span = self.index_offset - self.vertex_offset

        if span % self.count:
            raise ValueError('slot %d: %d vertex bytes do not divide by %d vertices'
                             % (slot, span, self.count))

        self.stride = span // self.count

        if self.stride not in (36, 48):
            raise ValueError('slot %d: vertex stride %d is not one of the two known'
                             % (slot, self.stride))

        if self.group_offset != MESH_HEADER + MESH_MATRIX * self.num_matrices:
            raise ValueError('slot %d: %d matrices do not reach the group table at %d'
                             % (slot, self.num_matrices, self.group_offset))

        if self.draw_offset + MESH_ENTRY * self.num_draws != self.vertex_offset:
            raise ValueError('slot %d: %d draws at %d do not reach the vertices at %d'
                             % (slot, self.num_draws, self.draw_offset,
                                self.vertex_offset))

        self.num_indices = (len(data) - self.index_offset) // 2
        self.num_groups = (self.draw_offset - self.group_offset) // MESH_ENTRY

    def matrices(self):
        """The palette, each as three rows of four floats."""
        for i in range(self.num_matrices):
            o = MESH_HEADER + i * MESH_MATRIX
            row = struct.unpack('>12f', self.data[o:o + MESH_MATRIX])
            yield (row[0:4], row[4:8], row[8:12])

    def groups(self):
        """(firstDraw, drawCount, matrixIndex) for each group."""
        for i in range(self.num_groups):
            o = self.group_offset + i * MESH_ENTRY
            yield struct.unpack('>3I', self.data[o:o + MESH_ENTRY])

    def draws(self):
        """(firstTriangle, triangleCount, material) for each draw."""
        for i in range(self.num_draws):
            o = self.draw_offset + i * MESH_ENTRY
            yield struct.unpack('>3I', self.data[o:o + MESH_ENTRY])

    def textures(self):
        """Every Textures.raw record this mesh draws with, in first use order."""
        seen = []
        for _first, _count, material in self.draws():
            n = material & MATERIAL_TEXTURE
            if n not in seen:
                seen.append(n)
        return seen

    def vertex(self, i):
        """(position, uv, normal, colour) - the parts that are known."""
        o = self.vertex_offset + i * self.stride
        f = struct.unpack('>8f', self.data[o:o + 32])
        colour = struct.unpack('>I', self.data[o + 32:o + 36])[0]
        return f[0:3], f[3:5], f[5:8], colour

    def skin(self, i):
        """(weights, bones) for a skinned vertex, or None for an unskinned one."""
        if self.stride != 48:
            return None
        o = self.vertex_offset + i * self.stride
        w0, w1 = struct.unpack('>2f', self.data[o + 36:o + 44])
        b0, b1, b2, n = struct.unpack('>4B', self.data[o + 44:o + 48])
        return (w0, w1, 1.0 - w0 - w1)[:n], (b0, b1, b2)[:n]

    def triangles(self):
        for t in range(self.num_indices // 3):
            o = self.index_offset + t * 6
            yield struct.unpack('>3H', self.data[o:o + 6])

    def write_obj(self, path, textures=None):
        """
        OBJ plus, when textures are given, an MTL and the PNGs it names.

        OBJ measures its V from the bottom of the image and the mesh measures
        it from the top, so the coordinate is flipped on the way out and the
        PNGs are written the right way up.
        """
        tris = list(self.triangles())
        base = os.path.splitext(path)[0]
        mtl = None

        if textures:
            mtl = base + '.mtl'
            self._write_mtl(mtl, os.path.dirname(path) or '.', textures)

        with open(path, 'w') as f:
            f.write('# Perfect Dark XBLA mesh, PackedSegFile slot %d\n' % self.slot)
            f.write('# %d vertices, %d triangles, %d byte stride, %d draws, %d groups\n'
                    % (self.count, len(tris), self.stride, self.num_draws,
                       self.num_groups))
            if mtl:
                f.write('mtllib %s\n' % os.path.basename(mtl))
            f.write('o xbla_%d\n' % self.slot)

            for i in range(self.count):
                pos, uv, nrm, _colour = self.vertex(i)
                f.write('v %.4f %.4f %.4f\n' % pos)
                f.write('vt %.6f %.6f\n' % (uv[0], 1.0 - uv[1]))
                f.write('vn %.4f %.4f %.4f\n' % nrm)

            for n, (first, count, material) in enumerate(self.draws()):
                f.write('g draw%d\n' % n)
                if mtl:
                    f.write('usemtl tex%d\n' % (material & MATERIAL_TEXTURE))
                for a, b, c in tris[first:first + count]:
                    if a < self.count and b < self.count and c < self.count:
                        f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n'
                                % (a + 1, a + 1, a + 1, b + 1, b + 1, b + 1,
                                   c + 1, c + 1, c + 1))

        return len(tris)

    def _write_mtl(self, path, outdir, textures):
        from PIL import Image

        with open(path, 'w') as f:
            for n in self.textures():
                png = 'xbla_tex_%04d.png' % n
                image = textures.decode(n)
                if image is not None:
                    Image.fromarray(image, 'RGBA').save(os.path.join(outdir, png))
                f.write('newmtl tex%d\n' % n)
                f.write('Kd 1 1 1\n')
                if image is not None:
                    f.write('map_Kd %s\n' % png)
                    f.write('map_d %s\n' % png)
                f.write('\n')


class Textures:
    """Textures.raw, decoded a record at a time and kept."""

    def __init__(self, raw):
        self.raw = raw
        self.meta, self.fetch, self.data, self.count = xblaconvert.read_records(raw)
        self.cache = {}

    def decode(self, n):
        """One record the right way up as RGBA, or None if it is empty."""
        if n in self.cache:
            return self.cache[n]

        image = None

        if n < self.count:
            offset, width, height, _sw, _sh, usize, csize = self.meta[n][:7]
            if usize and csize:
                surface = x360.lzx_decompress(self.raw, self.data + offset,
                                              csize, usize)
                # The console art is in N64 row order, upside down on screen.
                image = x360.decode_texture(surface, width, height,
                                            self.fetch[n])[::-1].copy()

        self.cache[n] = image
        return image


def _find_package(path):
    """Accepts the archive, the STFS package, or a file already pulled out."""
    with open(path, 'rb') as f:
        head = f.read(4)

    if head in x360.STFS_MAGICS:
        return x360.STFS(path)

    if head[:2] == b'7z' or path.lower().endswith(('.7z', '.zip')):
        import tempfile
        tmp = tempfile.mkdtemp()
        _extract(path, tmp)
        for root, _dirs, files in os.walk(tmp):
            for name in files:
                full = os.path.join(root, name)
                with open(full, 'rb') as f:
                    if f.read(4) in x360.STFS_MAGICS:
                        return x360.STFS(full)
        sys.exit('no STFS package inside %s' % path)

    return None


def load_packedseg(path):
    stfs = _find_package(path)
    if stfs:
        return PackedSeg(stfs.extract(PACKAGE_PATH))
    with open(path, 'rb') as f:
        return PackedSeg(f.read())


def load_textures(path):
    stfs = _find_package(path)
    if not stfs:
        sys.exit('--textures needs the archive or the STFS package, not %s'
                 % os.path.basename(path))
    return Textures(stfs.extract(TEXTURE_PATH))


def _extract(path, dest):
    try:
        import py7zr
        with py7zr.SevenZipFile(path, 'r') as z:
            z.extractall(dest)
        return
    except ImportError:
        pass

    import shutil
    import subprocess
    exe = shutil.which('7z') or shutil.which('7za') or shutil.which('7zr')
    if not exe:
        sys.exit('Reading the archive needs py7zr (pip install py7zr) or a 7z command.')
    r = subprocess.run([exe, 'x', '-y', '-o' + dest, path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.exit('7z failed: %s' % r.stderr.decode('utf-8', 'replace').strip())


def meshes(pk):
    """Every slot that parses as a mesh, in slot order."""
    for i in range(pk.count):
        try:
            data = pk.read(i)
        except Exception:
            continue
        if not data or len(data) < MESH_HEADER or data[0] != 0:
            continue
        try:
            yield Mesh(i, data)
        except ValueError:
            continue


def show(m):
    print('slot %d: %d vertices, %d triangles, stride %d%s'
          % (m.slot, m.count, m.num_indices // 3, m.stride,
             ', skinned' if m.stride == 48 else ''))
    print('  unknown float %g, %d matrices, %d groups, %d draws'
          % (m.unknown, m.num_matrices, m.num_groups, m.num_draws))

    for i, rows in enumerate(m.matrices()):
        print('    matrix %-3d %s' % (i, '  '.join('[%8.3f %8.3f %8.3f %8.3f]' % r
                                                   for r in rows)))

    draws = list(m.draws())

    for first, count, matrix in m.groups():
        print('    group draws %d..%d, matrix %d' % (first, first + count - 1,
                                                     matrix))
        for tri, tris, material in draws[first:first + count]:
            print('      draw tris %6d..%-6d texture %4d%s%s'
                  % (tri, tri + tris - 1, material & MATERIAL_TEXTURE,
                     ' alpha' if material & MATERIAL_ALPHA else '',
                     ' unknown %d/%d' % ((material >> 16) & 0xFF, material >> 24)
                     if material >> 16 else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--package', required=True,
                    help='the XBLA 7z, its STFS package, or a PackedSegFile')
    ap.add_argument('--list', action='store_true', help='every mesh and its size')
    ap.add_argument('--info', type=int, action='append', default=[],
                    help='print one mesh\'s tables; repeatable')
    ap.add_argument('--slot', type=int, action='append', default=[],
                    help='a PackedSegFile slot to write out; repeatable')
    ap.add_argument('--mesh-id', type=lambda x: int(x, 0), action='append',
                    default=[], dest='meshid',
                    help='a mesh id as a model node carries it, which is one '
                         'more than the slot; repeatable')
    ap.add_argument('--all', action='store_true', help='write every mesh')
    ap.add_argument('--textures', action='store_true',
                    help='write an MTL and the PNGs it names beside the OBJ')
    ap.add_argument('--out', help='output .obj, or a directory with --all')
    args = ap.parse_args()

    pk = load_packedseg(args.package)
    textures = load_textures(args.package) if args.textures else None

    # The id's low 12 bits are a file id, so the slot is one below it.
    args.slot += [(i & 0xFFF) - 1 for i in args.meshid]

    if args.list:
        total_v = total_t = 0
        for m in meshes(pk):
            tris = m.num_indices // 3
            total_v += m.count
            total_t += tris
            print('slot %4d  %7d verts  %7d tris  stride %2d  %3d draws  '
                  'textures %s'
                  % (m.slot, m.count, tris, m.stride, m.num_draws,
                     ' '.join(str(n) for n in m.textures()[:6])))
        print('%d vertices and %d triangles in total' % (total_v, total_t))
        return

    for slot in args.info:
        show(Mesh(slot, pk.read(slot)))

    if args.all:
        outdir = args.out or 'xbla-meshes'
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for m in meshes(pk):
            m.write_obj(os.path.join(outdir, 'xbla_%04d.obj' % m.slot), textures)
            n += 1
        print('wrote %d meshes to %s' % (n, outdir))
        return

    if not args.slot:
        if not args.info:
            sys.exit('give --list, --info, --all, or at least one --slot')
        return

    for slot in args.slot:
        data = pk.read(slot)
        if not data:
            sys.exit('slot %d is empty' % slot)
        m = Mesh(slot, data)
        path = args.out if args.out and len(args.slot) == 1 else 'xbla_%04d.obj' % slot
        tris = m.write_obj(path, textures)
        print('slot %d: %d vertices, %d triangles, stride %d, %d draws -> %s'
              % (slot, m.count, tris, m.stride, m.num_draws, path))


if __name__ == '__main__':
    main()
