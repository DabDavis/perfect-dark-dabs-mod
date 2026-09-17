"""GoldenEye image ids the remake's files use, moved off the numbers Perfect
Dark's own global textures are loaded by.

A Stage Loader map draws its stage's textures from its mod by number, so a
number the mod ships replaces Perfect Dark's texture of that number on that
stage. Rooms and models are fine with that - their numbers are the mod's -
but the texture config tables in src/textureconfig.c (sky and water, glares,
explosions, sparks, shadows...) are loaded by number too, and GoldenEye's own
images collide with some of them: the prop models' textures drew the cloud
layer as a pattern of GoldenEye art. Those images get free numbers past
GoldenEye's last (2697), skipping the reserved ones.
"""
import os, re

PD = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FIRST_FREE = 2698
NUM_TEXTURES = 3503


def reserved():
    src = open(PD + '/src/textureconfig.c').read()
    consts = {m.group(1): int(m.group(2), 0) for m in
              re.finditer(r'#define\s+(TEXTURE_\w+)\s+(0x[0-9a-fA-F]+|\d+)', open(PD + '/src/include/constants.h').read())}
    out = set()
    for m in re.finditer(r'\{\s*(0x[0-9a-fA-F]+|\d+|TEXTURE_\w+)\s*,\s*(0x[0-9a-fA-F]+|\d+|TEXTURE_\w+)?', src):
        for v in m.groups():
            if v:
                out.add(consts[v] if v.startswith('TEXTURE_') else int(v, 0))
    return out


RESERVED = reserved()
_map = {}
_next = FIRST_FREE


def remap(image):
    global _next
    if image not in RESERVED:
        return image
    if image not in _map:
        # a stable order: the colliding ids in order take the free numbers in order
        for old in sorted(RESERVED):
            if old in _map:
                continue
            while _next in RESERVED:
                _next += 1
            if _next >= NUM_TEXTURES:
                raise OverflowError('no free texture numbers left')
            _map[old] = _next
            _next += 1
    return _map[image]
