"""Byte-read tracking for the Python converter, without touching its source.

A `Tracked` stands in for a bytes object the converter reads from (the ROM, the
data segment, every file it inflates). It is deliberately NOT a bytes subclass:
everything the converter can do with one goes through a method here or through
the buffer protocol (`__buffer__`, Python 3.12+), so no read can slip past.

- d[i]                      marks byte i
- d[a:b]                    marks nothing: returns a child Tracked over the same
                            map (a slice is only read when it is used)
- struct.unpack_from(f,d,o) marks o .. o+calcsize(f) (struct is patched);
  struct.unpack(f, d[a:b]) and iter_unpack mark the slice, as a parse
- anything that takes the whole thing - bytes(d), bytearray(d), zlib, ==,
  iteration, b''.join, int.from_bytes, a write to a file - marks all of it
- find/index mark the bytes they scanned

Each root buffer has a map (a bytearray of 0/1) under a key naming what it is
(`file:UsetupdamZ`, `rom`, `data`, `inflated:bg_dam_all_p@0x1234`).
"""
import linecache
import struct
import sys
import zlib

ROOTS = {}          # key -> Root
UNKNOWN_OPS = {}    # method name -> count, for anything delegated wholesale
_FROMBYTES = {}     # (file, line) -> whether that line is an int.from_bytes
_orig_unpack_from = struct.unpack_from
_orig_unpack = struct.unpack
_orig_iter_unpack = struct.iter_unpack
_OR = {1: bytes(x | 1 for x in range(256)), 2: bytes(x | 2 for x in range(256))}
_orig_calcsize = struct.calcsize


class Root:
    __slots__ = ('key', 'data', 'map', 'how', 'shadow', 'mask')

    def __init__(self, key, data):
        self.key = key
        self.data = bytes(data)
        self.map = bytearray(len(self.data))
        self.how = bytearray(len(self.data))   # 1 parsed (unpack, d[i], find), 2 copied whole
        self.shadow = None      # the null's map: the same marks less `mask`
        self.mask = None        # bytearray of 1 where the null hides reads

    def mark(self, a, b, how=1):
        if a < 0:
            a = 0
        if b > len(self.map):
            b = len(self.map)
        if b <= a:
            return
        self.map[a:b] = b'\x01' * (b - a)
        self.how[a:b] = self.how[a:b].translate(_OR[how])
        if self.shadow is not None:
            # the null's map takes the same read, less any byte the mask hides
            if any(self.mask[a:b]):
                for i in range(a, b):
                    if not self.mask[i]:
                        self.shadow[i] = 1
            else:
                self.shadow[a:b] = b'\x01' * (b - a)


def root(key, data):
    """A Tracked over `data`, its map under `key` (reused if the key exists and
    the bytes are the same)."""
    r = ROOTS.get(key)
    if r is None or r.data != bytes(data):
        if r is not None:
            key = '%s#%d' % (key, sum(1 for k in ROOTS if k.split('#')[0] == key))
        r = Root(key, data)
        ROOTS[key] = r
    return Tracked(r, 0, len(r.data))


class Tracked:
    __slots__ = ('_r', '_base', '_len')

    def __init__(self, r, base, n):
        self._r, self._base, self._len = r, base, n

    # --- what the converter does with one -------------------------------
    def _bytes(self):
        return self._r.data[self._base:self._base + self._len]

    def _all(self):
        self._r.mark(self._base, self._base + self._len, 2)
        return self._bytes()

    def __len__(self):
        return self._len

    def __getitem__(self, k):
        if isinstance(k, slice):
            a, b, s = k.indices(self._len)
            if s != 1:
                idx = range(a, b, s)
                for i in idx:
                    self._r.mark(self._base + i, self._base + i + 1)
                return self._bytes()[k]
            if b < a:
                b = a
            return Tracked(self._r, self._base + a, b - a)
        if k < 0:
            k += self._len
        if not 0 <= k < self._len:
            raise IndexError('index out of range')
        self._r.mark(self._base + k, self._base + k + 1)
        return self._r.data[self._base + k]

    def __buffer__(self, flags):
        return memoryview(self._all())

    def __bytes__(self):
        # int.from_bytes(d[a:b]) comes here too, and is a parse, not a copy:
        # tell the two apart by the calling line (C calls add no frame)
        f = sys._getframe(1)
        key = (f.f_code.co_filename, f.f_lineno)
        parse = _FROMBYTES.get(key)
        if parse is None:
            parse = _FROMBYTES[key] = 'from_bytes' in linecache.getline(*key)
        self._r.mark(self._base, self._base + self._len, 1 if parse else 2)
        return self._bytes()

    def __iter__(self):
        return iter(self._all())

    def __eq__(self, other):
        if isinstance(other, Tracked):
            other = other._all()
        return self._all() == other

    def __ne__(self, other):
        return not self.__eq__(other)

    __hash__ = None

    def __add__(self, other):
        if isinstance(other, Tracked):
            other = other._all()
        return self._all() + bytes(other)

    # no __radd__: `bytearray += d` and `b'..' + d` must fall through to the
    # sequence concat, which takes the buffer (and keeps a bytearray a bytearray)

    def find(self, sub, start=0, end=None):
        b = self._bytes()
        i = b.find(sub, start, end)
        stop = (i + len(sub)) if i >= 0 else (len(b) if end is None else end)
        self._r.mark(self._base + start, self._base + stop)
        return i

    def index(self, sub, start=0, end=None):
        i = self.find(sub, start, end)
        if i < 0:
            raise ValueError('subsection not found')
        return i

    def decode(self, *a):
        return self._all().decode(*a)

    def hex(self, *a):
        return self._all().hex(*a)

    def rfind(self, *a):
        UNKNOWN_OPS['rfind'] = UNKNOWN_OPS.get('rfind', 0) + 1
        return self._all().rfind(*a)

    def startswith(self, p, *a):
        n = len(p) if isinstance(p, (bytes, bytearray)) else max(len(x) for x in p)
        self._r.mark(self._base, self._base + min(n, self._len))
        return self._bytes().startswith(p, *a)

    def __getattr__(self, name):
        # any other bytes method: counted, and the whole buffer taken as read
        UNKNOWN_OPS[name] = UNKNOWN_OPS.get(name, 0) + 1
        return getattr(self._all(), name)

    def __repr__(self):
        return '<Tracked %s +%#x len %#x>' % (self._r.key, self._base, self._len)


def _unpack_from(fmt, buffer, offset=0):
    if isinstance(buffer, Tracked):
        n = _orig_calcsize(fmt)
        if offset < 0:
            offset += buffer._len
        buffer._r.mark(buffer._base + offset, buffer._base + offset + n)
        return _orig_unpack_from(fmt, buffer._bytes(), offset)
    return _orig_unpack_from(fmt, buffer, offset)


def _unpack(fmt, buffer):
    if isinstance(buffer, Tracked):
        buffer._r.mark(buffer._base, buffer._base + buffer._len)
        return _orig_unpack(fmt, buffer._bytes())
    return _orig_unpack(fmt, buffer)


def _iter_unpack(fmt, buffer):
    if isinstance(buffer, Tracked):
        buffer._r.mark(buffer._base, buffer._base + buffer._len)
        return _orig_iter_unpack(fmt, buffer._bytes())
    return _orig_iter_unpack(fmt, buffer)


def install():
    struct.unpack_from = _unpack_from
    struct.unpack = _unpack
    struct.iter_unpack = _iter_unpack


def uninstall():
    struct.unpack_from = _orig_unpack_from
    struct.unpack = _orig_unpack
    struct.iter_unpack = _orig_iter_unpack
