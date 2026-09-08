"""
Reading Xbox 360 containers: STFS packages, LZX streams, GPU texture surfaces.

Nothing here knows anything about Perfect Dark. It is the three layers that sit
between a downloaded XBLA package and a picture, and each one is a format the
console defined rather than the game:

  * STFS is the package a title ships in - "LIVE", "CON " or "PIRS". It is a
    little filesystem with its data blocks interleaved with hash tables.
  * LZX is what XMemCompress produces. The stream is cut into chunks whose
    headers are outside the LZX bitstream, so the framing is read here and the
    bitstream is decoded by an ordinary LZXD decoder.
  * A texture is a tiled, byte-swapped, sometimes block-compressed surface, and
    its format lives in the GPU fetch constant rather than in the data.

Two things are worth knowing before changing any of it. The volume descriptor's
fields are easy to place one byte out - if the file table comes back as 257
blocks of mostly garbage instead of 1 block of names, that is the mistake. And
the LZX window is 17 bits: no other size decodes these streams, so a "bad
huffman code" from the decoder means the framing was wrong, not the window.
"""

import struct

import numpy as np

# ---------------------------------------------------------------------------
# STFS
# ---------------------------------------------------------------------------

STFS_MAGICS = (b'LIVE', b'CON ', b'PIRS')

# Data starts here; the level 0 hash table for the first group sits in the
# 0x1000 bytes immediately before it.
STFS_DATA_START = 0xC000
STFS_BLOCK = 0x1000
STFS_BLOCKS_PER_HASH = 0xAA


class STFS:
    """One STFS package, opened for reading."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x1000)

        if head[:4] not in STFS_MAGICS:
            raise ValueError('not an STFS package (magic %r)' % head[:4])

        header_size = struct.unpack('>I', head[0x340:0x344])[0]

        # Read-only packages put one hash table before each run of 0xAA blocks;
        # a read-write one keeps two, and every table offset doubles. The
        # rounded header size is what says which: 0xB000 means one.
        self.shift = 0 if ((header_size + 0xFFF) & 0xF000) >> 0xC == 0xB else 1

        # Volume descriptor at 0x379: length, version, block separation, then
        # the file table's block count and block number.
        self.ft_blocks = struct.unpack('<H', head[0x37C:0x37E])[0]
        self.ft_block = struct.unpack('<I', head[0x37E:0x381] + b'\0')[0]
        self.total_blocks = struct.unpack('>I', head[0x395:0x399])[0]

    def _backing(self, block):
        """Block number to backing block number, skipping the hash tables."""
        adjust = 0
        if block >= 0xAA:
            adjust += ((block // 0xAA) + 1) << self.shift
        if block >= 0x70E4:
            adjust += ((block // 0x70E4) + 1) << self.shift
        return block + adjust

    def offset(self, block):
        return STFS_DATA_START + self._backing(block) * STFS_BLOCK

    def read_block(self, block):
        self.f.seek(self.offset(block))
        return self.f.read(STFS_BLOCK)

    def next_block(self, block):
        """The block after this one, from its level 0 hash table entry."""
        table = self.offset(block - (block % STFS_BLOCKS_PER_HASH)) - STFS_BLOCK
        self.f.seek(table + (block % STFS_BLOCKS_PER_HASH) * 0x18)
        entry = self.f.read(0x18)
        return struct.unpack('>I', b'\0' + entry[0x15:0x18])[0]

    def chain(self, start, count, consecutive):
        blocks, block = [], start
        for _ in range(count):
            blocks.append(block)
            block = block + 1 if consecutive else self.next_block(block)
            if block == 0xFFFFFF or block >= self.total_blocks:
                break
        return blocks

    def listing(self):
        """Every entry in the file table, in table order."""
        raw = b''.join(self.read_block(self.ft_block + i)
                       for i in range(self.ft_blocks))
        entries = []

        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            flags = e[0x28]
            namelen = flags & 0x3F
            if namelen == 0:
                continue
            entries.append({
                'name': e[:namelen].decode('latin-1'),
                'isdir': bool(flags & 0x80),
                'consecutive': bool(flags & 0x40),
                'blocks': struct.unpack('<I', e[0x29:0x2C] + b'\0')[0],
                'start': struct.unpack('<I', e[0x2F:0x32] + b'\0')[0],
                'parent': struct.unpack('>H', e[0x32:0x34])[0],
                'size': struct.unpack('>I', e[0x34:0x38])[0],
            })

        return entries

    def path_of(self, entries, i):
        parts, parent, guard = [entries[i]['name']], entries[i]['parent'], 0
        while parent != 0xFFFF and parent < len(entries) and guard < 32:
            parts.append(entries[parent]['name'])
            parent = entries[parent]['parent']
            guard += 1
        return '/'.join(reversed(parts))

    def read_file(self, entry):
        out, left = bytearray(), entry['size']
        for block in self.chain(entry['start'], entry['blocks'], entry['consecutive']):
            if left <= 0:
                break
            out += self.read_block(block)[:min(STFS_BLOCK, left)]
            left -= STFS_BLOCK
        return bytes(out)

    def extract(self, wanted):
        """Reads one file out by path, e.g. 'DataFiles/Textures.raw'."""
        entries = self.listing()
        for i, e in enumerate(entries):
            if not e['isdir'] and self.path_of(entries, i) == wanted:
                return self.read_file(e)
        raise KeyError(wanted)


# ---------------------------------------------------------------------------
# LZX
# ---------------------------------------------------------------------------

LZX_WINDOW_BITS = 17

_EXTRA, _BASE = [], []
_j = 0
for _i in range(0, 51, 2):
    _EXTRA += [_j, _j]
    if _i != 0 and _j < 17:
        _j += 1
_EXTRA = _EXTRA[:51]
_j = 0
for _i in range(51):
    _BASE.append(_j)
    _j += 1 << _EXTRA[_i]


class _Bits:
    """LZX reads 16 bit little-endian words, and bits out of them MSB first."""

    def __init__(self, data):
        self.d, self.p, self.b, self.n = data, 0, 0, 0

    def _fill(self, need):
        while self.n < need:
            lo = self.d[self.p] if self.p < len(self.d) else 0
            hi = self.d[self.p + 1] if self.p + 1 < len(self.d) else 0
            self.p += 2
            self.b = (self.b << 16) | (lo | (hi << 8))
            self.n += 16

    def read(self, k):
        if k == 0:
            return 0
        self._fill(k)
        v = (self.b >> (self.n - k)) & ((1 << k) - 1)
        self.n -= k
        self.b &= (1 << self.n) - 1
        return v

    def bit(self):
        return self.read(1)

    def align(self):
        # Back the read position up over whatever whole words are still
        # buffered, so a raw read afterwards carries on from the right byte.
        # Only an uncompressed block reaches this, which these streams do not
        # contain - but x360.c does it properly and the two should agree.
        if self.n & 15:
            self.read(self.n & 15)
        self.p -= self.n >> 3
        self.n = 0
        self.b = 0


class _Huff:
    """Canonical Huffman over code lengths, decoded a bit at a time."""

    def __init__(self, lens):
        self.count = [0] * 17
        for l in lens:
            if l:
                self.count[l] += 1
        self.sym = [s for l in range(1, 17) for s, sl in enumerate(lens) if sl == l]

    def decode(self, br):
        code = first = index = 0
        for l in range(1, 17):
            code |= br.bit()
            c = self.count[l]
            if code - first < c:
                return self.sym[index + code - first]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError('bad huffman code')


def _position_slots(window_bits):
    if window_bits == 20:
        return 42
    if window_bits == 21:
        return 50
    return window_bits * 2


def _read_lengths(br, lens, first, last):
    """Code lengths are deltas against the previous block's, via a pretree."""
    pre = _Huff([br.read(4) for _ in range(20)])
    x = first
    while x < last:
        z = pre.decode(br)
        if z == 17:
            for _ in range(br.read(4) + 4):
                if x < last:
                    lens[x] = 0
                    x += 1
        elif z == 18:
            for _ in range(br.read(5) + 20):
                if x < last:
                    lens[x] = 0
                    x += 1
        elif z == 19:
            run = br.read(1) + 4
            v = lens[x] - pre.decode(br)
            if v < 0:
                v += 17
            for _ in range(run):
                if x < last:
                    lens[x] = v
                    x += 1
        else:
            v = lens[x] - z
            if v < 0:
                v += 17
            lens[x] = v
            x += 1


class LZX:
    """One decode context. The window and the trees persist across chunks."""

    def __init__(self, window_bits=LZX_WINDOW_BITS):
        slots = _position_slots(window_bits)
        self.window_bits = window_bits
        self.nmain = 256 + (slots << 3)
        self.mainlens = [0] * self.nmain
        self.lenlens = [0] * 249
        self.main = self.length = self.aligned = None
        self.R = [1, 1, 1]
        self.btype = self.bleft = 0
        self.started = False
        self.out = bytearray()

    def chunk(self, data, outlen):
        br = _Bits(data)
        target = len(self.out) + outlen

        if not self.started:
            # One header for the whole stream: an Intel (E8) translation size
            # that these packages never set.
            if br.bit():
                br.read(16)
                br.read(16)
            self.started = True

        while len(self.out) < target:
            if self.bleft == 0:
                self.btype = br.read(3)
                self.bleft = (br.read(16) << 8) | br.read(8)

                if self.btype in (1, 2):
                    if self.btype == 2:
                        self.aligned = _Huff([br.read(3) for _ in range(8)])
                    _read_lengths(br, self.mainlens, 0, 256)
                    _read_lengths(br, self.mainlens, 256, self.nmain)
                    self.main = _Huff(self.mainlens)
                    _read_lengths(br, self.lenlens, 0, 249)
                    self.length = _Huff(self.lenlens)
                elif self.btype == 3:
                    br.align()
                    raw = br.d[br.p:br.p + 12]
                    self.R = [int.from_bytes(raw[i * 4:i * 4 + 4], 'little')
                              for i in range(3)]
                    br.p += 12
                    br.b = br.n = 0
                else:
                    raise ValueError('bad LZX block type %d' % self.btype)

            todo = min(self.bleft, target - len(self.out))

            if self.btype == 3:
                self.out += br.d[br.p:br.p + todo]
                br.p += todo
                self.bleft -= todo
                if self.bleft == 0 and br.p & 1:
                    br.p += 1
                continue

            done = 0
            while done < todo:
                m = self.main.decode(br)

                if m < 256:
                    self.out.append(m)
                    done += 1
                    continue

                m -= 256
                mlen = m & 7
                if mlen == 7:
                    mlen = self.length.decode(br) + 7
                mlen += 2

                slot = m >> 3
                if slot == 0:
                    off = self.R[0]
                elif slot == 1:
                    off = self.R[1]
                    self.R[1] = self.R[0]
                    self.R[0] = off
                elif slot == 2:
                    off = self.R[2]
                    self.R[2] = self.R[0]
                    self.R[0] = off
                else:
                    extra = _EXTRA[slot]
                    if self.btype == 2:
                        if extra >= 3:
                            verbatim = br.read(extra - 3) << 3
                            align = self.aligned.decode(br)
                        elif extra == 3:
                            verbatim, align = 0, self.aligned.decode(br)
                        else:
                            verbatim, align = br.read(extra), 0
                    else:
                        verbatim, align = br.read(extra), 0
                    off = _BASE[slot] - 2 + verbatim + align
                    self.R[2], self.R[1], self.R[0] = self.R[1], self.R[0], off

                src = len(self.out) - off
                if src < 0:
                    raise ValueError('match reaches before the window start')
                for _ in range(mlen):
                    self.out.append(self.out[src])
                    src += 1
                done += mlen

            self.bleft -= done

        return self.out


def lzx_chunks(data, offset, csize):
    """
    Splits a chunked XMemCompress stream into (compressed bytes, output size).

    A chunk led by 0xFF states both sizes; anything else is a bare 16 bit
    compressed size and a full 32KB of output.
    """
    chunks, p, end = [], offset, offset + csize
    while p < end:
        if data[p] == 0xFF:
            usize = struct.unpack('>H', data[p + 1:p + 3])[0]
            csz = struct.unpack('>H', data[p + 3:p + 5])[0]
            head = 5
        else:
            csz = struct.unpack('>H', data[p:p + 2])[0]
            usize, head = 0x8000, 2
        if csz == 0:
            break
        chunks.append((data[p + head:p + head + csz], usize))
        p += head + csz
    return chunks


def lzx_decompress(data, offset, csize, usize, window_bits=LZX_WINDOW_BITS):
    cx = LZX(window_bits)
    for blob, out in lzx_chunks(data, offset, csize):
        cx.chunk(blob, out)
    return bytes(cx.out[:usize])


# ---------------------------------------------------------------------------
# Texture surfaces
# ---------------------------------------------------------------------------

# GPUTEXTURE_FETCH_CONSTANT data formats, only the ones these packages use.
FMT_8888, FMT_DXT1, FMT_DXT23, FMT_DXT45 = 6, 18, 19, 20
BYTES_PER_ELEMENT = {FMT_8888: 4, FMT_DXT1: 8, FMT_DXT23: 16, FMT_DXT45: 16}
FORMAT_NAMES = {FMT_8888: '8888', FMT_DXT1: 'DXT1', FMT_DXT23: 'DXT2/3',
                FMT_DXT45: 'DXT4/5'}


class FetchConstant:
    """The six dwords of a GPU fetch constant, as the fields that matter."""

    def __init__(self, dwords):
        d0, d1, d2 = dwords[0], dwords[1], dwords[2]
        self.tiled = bool(d0 >> 31)
        # Pitch counts 32-texel groups; block formats align to 32 blocks, so
        # their stored row is a multiple of 128 texels rather than 32.
        self.pitch = ((d0 >> 22) & 0x1FF) * 32
        self.format = d1 & 0x3F
        self.endian = (d1 >> 6) & 3
        self.width = (d2 & 0x1FFF) + 1
        self.height = ((d2 >> 13) & 0x1FFF) + 1


def endian_swap(data, mode):
    """1 is 8-in-16, 2 is 8-in-32; both put the surface into host order."""
    a = np.frombuffer(data, dtype=np.uint8).copy()
    if mode == 1:
        n = len(a) & ~1
        a[:n] = a[:n].reshape(-1, 2)[:, ::-1].ravel()
    elif mode == 2:
        n = len(a) & ~3
        a[:n] = a[:n].reshape(-1, 4)[:, ::-1].ravel()
    return a


def _tile_offsets(ew, eh, log2bpp):
    """XGAddress2DTiledOffset over a whole element grid at once."""
    y, x = np.meshgrid(np.arange(eh, dtype=np.int64),
                       np.arange(ew, dtype=np.int64), indexing='ij')
    macro = ((x >> 5) + (y >> 5) * ((ew + 31) >> 5)) << (log2bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2bpp
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) +
             (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)) >> log2bpp)


def _linear(src, ew, eh, bpe, tiled):
    """An (eh, ew, bpe) grid, untiled if it needs to be and zero padded."""
    if tiled:
        idx = _tile_offsets(ew, eh, {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[bpe]).ravel()
        need = int(idx.max()) + 1
        if len(src) < need * bpe:
            src = np.concatenate([src, np.zeros(need * bpe - len(src), np.uint8)])
        return src[:need * bpe].reshape(need, bpe)[idx].reshape(eh, ew, bpe)

    need = ew * eh * bpe
    if len(src) < need:
        src = np.concatenate([src, np.zeros(need - len(src), np.uint8)])
    return src[:need].reshape(eh, ew, bpe)


def _expand565(c):
    # The same rounding as dxt.c, so this and the in-game import produce the
    # same pack rather than two that differ by a level here and there.
    r = ((c >> 11) & 31).astype(np.int32)
    g = ((c >> 5) & 63).astype(np.int32)
    b = (c & 31).astype(np.int32)
    return np.stack([r * 255 // 31, g * 255 // 63, b * 255 // 31], -1)


def _decode_dxt(blocks, w, h, kind):
    bh, bw, bs = blocks.shape
    colour = blocks[:, :, (8 if kind != 1 else 0):][:, :, :8].astype(np.uint32)
    c0 = colour[:, :, 0] | (colour[:, :, 1] << 8)
    c1 = colour[:, :, 2] | (colour[:, :, 3] << 8)
    idx = (colour[:, :, 4] | (colour[:, :, 5] << 8) |
           (colour[:, :, 6] << 16) | (colour[:, :, 7] << 24))

    p0, p1 = _expand565(c0), _expand565(c1)
    # Only DXT1 has the c0 <= c1 punch-through mode; the others always
    # interpolate at thirds and carry their alpha separately.
    opaque = (c0 > c1) if kind == 1 else np.ones(c0.shape, bool)
    pal = np.zeros((bh, bw, 4, 3), np.int32)
    pal[:, :, 0], pal[:, :, 1] = p0, p1
    pal[:, :, 2] = np.where(opaque[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, :, 3] = np.where(opaque[..., None], (p0 + 2 * p1) // 3, 0)

    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], -1)
    rgb = np.take_along_axis(pal, sel[..., None].astype(np.intp), axis=2)

    if kind == 1:
        alpha = np.where((sel == 3) & ~opaque[..., None], 0, 255).astype(np.uint8)
    elif kind == 3:
        packed = blocks[:, :, :8].astype(np.uint32)
        alpha = np.zeros((bh, bw, 16), np.uint8)
        for i in range(8):
            alpha[:, :, i * 2] = (packed[:, :, i] & 15) * 17
            alpha[:, :, i * 2 + 1] = (packed[:, :, i] >> 4) * 17
    else:
        a0 = blocks[:, :, 0].astype(np.int32)
        a1 = blocks[:, :, 1].astype(np.int32)
        bits = np.zeros((bh, bw), np.uint64)
        for i in range(6):
            bits |= blocks[:, :, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        hi = a0 > a1
        ramp = [a0, a1]
        for k in range(1, 7):
            six = (a0 * (5 - k) + a1 * k) // 5 if k < 5 else (0 if k == 5 else 255)
            ramp.append(np.where(hi, (a0 * (7 - k) + a1 * k) // 7, six))
        table = np.stack(ramp, -1).astype(np.int32)
        asel = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.intp)
                         for i in range(16)], -1)
        alpha = np.take_along_axis(table, asel, axis=2).astype(np.uint8)

    img = np.zeros((bh, 4, bw, 4, 4), np.uint8)
    img[:, :, :, :, :3] = rgb.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4)
    img[:, :, :, :, 3] = alpha.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3)
    return img.reshape(bh * 4, bw * 4, 4)[:h, :w]


def decode_texture(data, width, height, fetch):
    """
    One surface's base level as an (h, w, 4) RGBA array.

    Mip levels follow the base in the same buffer and are simply left off the
    end: the game picks its own, and a pack only replaces level 0.
    """
    fmt = fetch.format
    if fmt not in BYTES_PER_ELEMENT:
        raise ValueError('unsupported texture format %d' % fmt)

    src = endian_swap(data, fetch.endian)
    bpe = BYTES_PER_ELEMENT[fmt]

    if fmt == FMT_8888:
        ew = max(fetch.pitch, width)
        eh = (-(-height // 32) * 32) if fetch.tiled else height
        grid = _linear(src, ew, eh, bpe, fetch.tiled)
        # The surface is BGRA on the console.
        return np.ascontiguousarray(grid[:height, :width][:, :, [2, 1, 0, 3]])

    kind = {FMT_DXT1: 1, FMT_DXT23: 3, FMT_DXT45: 5}[fmt]
    ew = max(fetch.pitch, width) // 4
    bh = -(-height // 4)
    eh = (-(-bh // 32) * 32) if fetch.tiled else bh
    grid = _linear(src, ew, eh, bpe, fetch.tiled)
    return _decode_dxt(np.ascontiguousarray(grid[:bh, :-(-width // 4)]),
                       width, height, kind)
