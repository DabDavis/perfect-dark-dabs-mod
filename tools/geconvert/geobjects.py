"""GoldenEye's own objects as Perfect Dark setup objects, for the GoldenEye
remake: from the level's own GoldenEye setup, on its own pads and bound pads,
with the remake's own models (MODEL_REMAKE_FIRST + GoldenEye's model number,
files from gemodelconv.py). Nothing here reads GE-X.

The conversion is the one GE-X made of the GoldenEye levels it kept, learnt by
comparing the two (CLAUDE-notes/ge-bean.md, "Runway and Caves"): the header
copied, 1000 damage, a floor colour of 0x0fff, a door's fields moved from
GoldenEye's 0x80 to Perfect Dark's 0x5c with accel and decel times 1000, and a
door's flags 0x80 -> 0x40, 0x40 dropped, 0x08 -> 0x80.
"""
import struct
import numpy as np

MODEL_REMAKE_FIRST = 0x200
N64_DEFAULTOBJ = 0x5c

# GoldenEye's setup commands, in words (loadobjectmodel.c sizepropdef(), the
# branch that is built: a type it has no case for is one word)
GE_SIZES = {1: 64, 2: 2, 3: 32, 4: 33, 5: 32, 6: 0x3b, 7: 0x21, 8: 0x22, 9: 7, 10: 0x40, 11: 0x95,
            12: 32, 13: 0x36, 14: 3, 17: 32, 18: 3, 19: 4, 20: 0x2d, 21: 0x22, 22: 4, 23: 4, 24: 1,
            25: 2, 26: 2, 27: 2, 28: 2, 29: 2, 30: 4, 31: 1, 32: 4, 33: 5, 34: 1, 35: 4, 36: 32,
            37: 10, 38: 4, 39: 0x2c, 40: 0x2d, 42: 32, 43: 32, 44: 5, 45: 0x38, 46: 7, 47: 37}

# GoldenEye type -> (Perfect Dark type, its size in words). Vehicles, the
# aircraft and the tank stand as plain props; guards, guns,
# autoguns, keys, hats, the mission's commands and ammo crates (the arena
# places its own) are not arena objects.
CARRY = {1: (0x01, 55), 3: (0x03, 23), 5: (0x05, 23), 10: (0x0a, 53), 11: (0x0b, 140), 12: (0x0c, 23),
         39: (0x03, 23), 40: (0x03, 23), 42: (0x2a, 24), 43: (0x2b, 23), 45: (0x03, 23),
         47: (0x2f, 26)}


def records(d):
    """[(type, bytes)] of a GoldenEye setup's objects."""
    h = struct.unpack_from('>10I', d, 0)
    o = h[3]
    out = []
    while o + 4 <= len(d):
        t = d[o + 3]
        if t == 48:
            break
        out.append((t, d[o:o + 4 * GE_SIZES[t]]))
        o += 4 * GE_SIZES[t]
    return out


def bound_pads(d, ls, offset):
    """GoldenEye's bound pads as Perfect Dark pad records in the arena's frame:
    a float position, up and look, and the box divided by the level scale and
    rounded, as GE-X writes them. They follow the pads, so bound pad k is pad
    numpads + k."""
    h = struct.unpack_from('>10I', d, 0)
    out = []
    o = h[7]
    while h[7] and o + 0x44 <= len(d):
        if struct.unpack_from('>I', d, o + 36)[0] == 0:
            break
        pos = np.array(struct.unpack_from('>3f', d, o)) / ls - offset
        box = [float(round(v / ls)) for v in struct.unpack_from('>6f', d, o + 0x2c)]
        rec = (struct.pack('>I', 0x200 << 14) + struct.pack('>3f', *pos) + d[o + 12:o + 36]
               + struct.pack('>6f', *box))
        out.append((0x200, pos, rec))
        o += 0x44
    return out


def obj_health(raw):
    """What an object can take before it breaks: GoldenEye's record carries it
    as a 16.16 word at 0x74 (its loader divides by 65536), and the arithmetic
    from there is Perfect Dark's own. Most are 1000; Dam's padlocks are 200."""
    h = int(struct.unpack_from('>i', raw, 0x74)[0] / 65536)
    if h < 1:
        h = 1000
    return min(h, 32767)


def _base(b, pdtype, words, padnum, flags):
    out = bytearray(4 * words)
    out[0:3] = b[0:3]
    out[3] = pdtype
    model = struct.unpack_from('>h', b, 4)[0]
    struct.pack_into('>hHII', out, 4, MODEL_REMAKE_FIRST + model, padnum, flags, struct.unpack_from('>I', b, 12)[0])
    struct.pack_into('>hh', out, 0x4c, 0, obj_health(b))
    struct.pack_into('>I', out, 0x58, 0x0fff0000)
    return out


def _door_flags(f):
    top = f >> 24
    new = top & ~0xc8
    if top & 0x80:
        new |= 0x40
    if top & 0x08:
        new |= 0x80
    return (new << 24) | (f & 0xffffff)


# GoldenEye's motorbike (PROP_MOTORBIKE), which no GoldenEye level places
MOTORBIKE = 287


def hoverbike(padnum):
    """GoldenEye's motorbike as a Perfect Dark hoverbike on pad `padnum`, which
    rides as Perfect Dark's own: the record GE-X writes for the bikes it adds to
    its Runway (its flags, a hov of type 1), on the remake's own model."""
    rec = bytearray(4 * 56)
    struct.pack_into('>HBBhHIII', rec, 0, 256, 0, 0x33, MODEL_REMAKE_FIRST + MOTORBIKE, padnum,
                     0x05120101, 0x00304300, 0x02000000)
    struct.pack_into('>hh', rec, 0x4c, 0, 1000)
    struct.pack_into('>I', rec, 0x58, 0x0fff0000)
    rec[0x5c] = 1
    rec[0x5d] = 1
    return bytes(rec)


def objects(d, numpads, first_index, bodyarmour=None, bikepads=()):
    """GoldenEye's arena objects as Perfect Dark commands starting at command
    `first_index`, and the GoldenEye model numbers they use. Body armour is a
    shield with model `bodyarmour`, a stock model number. A motorbike stands on
    each of `bikepads`."""
    recs = records(d)
    kept, newindex = [], {}
    for i, (t, b) in enumerate(recs):
        if t in CARRY or (t == 21 and bodyarmour is not None):
            newindex[i] = first_index + len(kept)
            kept.append((i, t, b))
    out, models = [], set()
    for i, t, b in kept:
        model, padnum = struct.unpack_from('>hh', b, 4)
        padnum &= 0xffff
        padnum = padnum + numpads if t == 1 else (padnum + numpads - 10000 if padnum >= 10000 else padnum)
        flags = struct.unpack_from('>I', b, 8)[0]
        if t == 21:
            rec = _base(b, 0x15, 26, padnum, flags)
            struct.pack_into('>h', rec, 4, bodyarmour)
            rec[0x5c:0x60] = b[0x80:0x84]
            out.append(bytes(rec))
            continue
        pdtype, words = CARRY[t]
        models.add(model)
        if t == 1:
            rec = _base(b, pdtype, words, padnum, _door_flags(flags))
            for ge, pd, mul in ((0x84, 0x5c, 1), (0x88, 0x60, 1), (0x8c, 0x64, 1000), (0x90, 0x68, 1000),
                                (0x94, 0x6c, 1), (0x98, 0x70, 1), (0x9c, 0x74, 1), (0xa0, 0x78, 1)):
                struct.pack_into('>i', rec, pd, struct.unpack_from('>i', b, ge)[0] * mul)
            rel = struct.unpack_from('>i', b, 0x80)[0]
            sib = i + rel
            struct.pack_into('>i', rec, 0xbc, newindex[sib] - newindex[i] if rel and sib in newindex and recs[sib][0] == 1 else 0)
            rec[0xc6] = b[0xa7]
            rec[0xcc] = 0xff
        elif t == 47:
            rec = _base(b, pdtype, words, padnum, flags)
            tint, cull, opacity, portal = struct.unpack_from('>4i', b, 0x80)
            struct.pack_into('>hhhh', rec, 0x5c, tint, cull, opacity, portal)
        else:
            rec = _base(b, pdtype, words, padnum, flags)
        out.append(bytes(rec))
    for padnum in bikepads:
        out.append(hoverbike(padnum))
        models.add(MOTORBIKE)
    return out, models


class Rooms:
    """The room a position is in: the floor tile under it, else the room whose
    geometry's box holds it (a wall or ceiling pad)."""

    def __init__(self, stan, ls, offset, bg):
        self.tiles = []
        for t in stan:
            p = np.array([q[:3] for q in t['points']], float) / ls - offset
            self.tiles.append((t['room'], p, p[:, 0].min(), p[:, 0].max(), p[:, 2].min(), p[:, 2].max()))
        self.boxes = []
        for r, room in enumerate(bg.rooms, 1):
            v = room['vtx'] or b''
            if not v:
                continue
            pts = (np.array([struct.unpack_from('>3h', v, 16 * k) for k in range(len(v) // 16)], float) + room['pos']) / ls - offset
            self.boxes.append((r, pts.min(0), pts.max(0)))

    def floor(self, pos):
        """The floor a pad stands on: the highest tile whose x/z holds the pad
        and whose own height does not pass it. None where the pad is over a
        hole, or under the floor.
        """
        x, y, z = pos
        best = None
        for room, p, x0, x1, z0, z1 in self.tiles:
            if not (x0 <= x <= x1 and z0 <= z <= z1):
                continue
            inside = False
            n = len(p)
            for k in range(n):
                ax, az, bx, bz = p[k][0], p[k][2], p[(k + 1) % n][0], p[(k + 1) % n][2]
                if (az > z) != (bz > z) and x < (bx - ax) * (z - az) / (bz - az) + ax:
                    inside = not inside
            if not inside:
                continue
            fy = p[:, 1].mean()
            if fy <= y and (best is None or fy > best):
                best = fy
        return best

    def room(self, pos):
        x, y, z = pos
        best = None
        for room, p, x0, x1, z0, z1 in self.tiles:
            if not (x0 <= x <= x1 and z0 <= z <= z1):
                continue
            inside = False
            n = len(p)
            for k in range(n):
                ax, az, bx, bz = p[k][0], p[k][2], p[(k + 1) % n][0], p[(k + 1) % n][2]
                if (az > z) != (bz > z) and x < (bx - ax) * (z - az) / (bz - az) + ax:
                    inside = not inside
            if not inside:
                continue
            dy = y - p[:, 1].mean()
            score = dy if dy >= -60 else 10000 - dy
            if best is None or score < best[0]:
                best = (score, room)
        if best:
            return best[1]
        inbox = [(np.prod(mx - mn), r) for r, mn, mx in self.boxes if np.all(pos >= mn - 20) and np.all(pos <= mx + 20)]
        if inbox:
            return min(inbox)[1]
        return min((np.linalg.norm(np.clip(pos, mn, mx) - pos), r) for r, mn, mx in self.boxes)[1]
