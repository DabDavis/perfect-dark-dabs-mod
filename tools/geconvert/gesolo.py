"""GoldenEye's twenty solo missions as Perfect Dark setups.

GoldenEye's setup file is Perfect Dark's ancestor and the two have not moved
far apart: the same ten-slot header (waypoints, waygroups, intro, props, paths,
ailists, pads, bound pads, and two name tables Perfect Dark dropped), and the
same propdef type numbers - door 1, guard 9, the objectives 0x17-0x23, glass
0x2a, tinted glass 0x2f - all the way up. What differs is the size of a record
(GoldenEye's ObjectRecord is 0x80 bytes against Perfect Dark's 0x5c), the
second opcode byte on an AI command, and the handful of fields Perfect Dark
moved. So a mission converts record for record.

Everything here reads the ROM alone. GE-X is the oracle the field maps were
learnt from (CLAUDE-notes/ge-bean.md) and is never read.

A record keeps its place in the list: every GoldenEye record becomes exactly
one Perfect Dark record, so the indices the file refers to itself by - a door's
sibling, a tag's object, an objective's criteria - are the same on both sides.
A type with no Perfect Dark equivalent becomes OBJTYPE_22, which is one word
and does nothing.
"""
import struct

import geobjects
import geaitable

MODEL_REMAKE_FIRST = geobjects.MODEL_REMAKE_FIRST

# Perfect Dark's own record sizes in words, as port/src/preprocess/filesetup.c
# sizes them (objSizeN64(), the n64_* structs)
PD_SIZES = {
    0x01: 55, 0x02: 2, 0x03: 23, 0x04: 24, 0x05: 23, 0x06: 49, 0x07: 24, 0x08: 26,
    0x09: 11, 0x0a: 53, 0x0b: 140, 0x0c: 23, 0x0d: 43, 0x0e: 2, 0x0f: 23, 0x11: 23,
    0x12: 2, 0x13: 5, 0x14: 42, 0x15: 26, 0x16: 4, 0x17: 4, 0x18: 1, 0x19: 2,
    0x1a: 2, 0x1b: 2, 0x1c: 2, 0x1d: 2, 0x1e: 4, 0x1f: 1, 0x20: 4, 0x21: 5,
    0x22: 1, 0x23: 4, 0x24: 23, 0x25: 10, 0x26: 4, 0x27: 34, 0x28: 35, 0x2a: 24,
    0x2b: 23, 0x2c: 5, 0x2d: 32, 0x2e: 7, 0x2f: 26, 0x30: 37, 0x31: 5, 0x32: 4,
    0x33: 56, 0x34: 1,
}

# The GoldenEye types that are an ObjectRecord with a tail: the tail is copied
# from GoldenEye's 0x80 to Perfect Dark's 0x5c, field by field, as
# (GoldenEye offset, Perfect Dark offset, width, multiplier). The rest of a
# Perfect Dark record is left zero, which is what its loader expects of the
# runtime fields.
OBJ_TAILS = {
    0x04: ((0x80, 0x5c, 4, 1),),                       # key: the key flags
    0x07: ((0x80, 0x5c, 4, 1),),                       # ammo crate: the ammo type
    0x15: ((0x80, 0x5c, 4, 1), (0x84, 0x60, 4, 1)),    # armour: initial and current
}
# Glass (0x2a) has no tail: GoldenEye's record is the ObjectRecord and nothing
# more, and Perfect Dark's portalnum is found at the load. Reading one anyway
# takes the head of the next record.

# GoldenEye types with no Perfect Dark record of the same shape. They keep their
# place in the list as a one-word OBJTYPE_22, which the engine walks over.
#
# **Hats (0x11)** are left out with them. GoldenEye's hat is its own model, and
# a converted one is a rigid prop - one matrix, a position node at its root -
# which Perfect Dark cannot pose on a head: the frame a guard wearing one was
# ticked, modelasm00018680() took the parent's matrix of a node that has none
# and the mission died. GoldenEye's own heads carry their hats anyway, and the
# remake's guards wear Bean's, GE-X's or Perfect Dark's heads.
AS_NOTHING = {0x0e, 0x11, 0x12, 0x13, 0x14}

OBJTYPE_NOTHING = 0x22
OBJTYPE_END = 0x34
OBJTYPE_CHR = 0x09

# GoldenEye's own weapon numbers are the port's GoldenEye weapons, which
# geguns.c adds after Perfect Dark's own (CLAUDE-notes/ge-bean.md, "GoldenEye's
# guns as Perfect Dark weapons"): item 0 is WEAPON_GE_FIRST.
WEAPON_GE_FIRST = 0x5e
GE_NUM_WEAPONS = 0x76 - 0x5e + 1


def STOCK_BODIES(bodyid, headid):
    """A guard's body and head as the file keeps them: GoldenEye's own numbers.

    The port maps them when the mission loads, because what a GoldenEye guard
    can be made of depends on what the player has installed - GoldenEye's own
    characters out of the XBLA release (gebean.c), GE-X's borrowed ones, or
    Perfect Dark's own - and the conversion runs before any of that is known.
    """
    return bodyid & 0xff, (headid & 0xff) if headid >= 0 else 0xff


def ge_records(d):
    """[(index, type, bytes)] of a GoldenEye setup's propdefs."""
    h = struct.unpack_from('>10I', d, 0)
    o, out = h[3], []
    while o + 4 <= len(d):
        t = d[o + 3]
        if t == 48:
            return out
        n = geobjects.GE_SIZES.get(t)
        if n is None:
            raise ValueError('setup: propdef type %d at %#x' % (t, o))
        out.append((len(out), t, d[o:o + 4 * n]))
        o += 4 * n
    return out


NO_PAD = 0xffff


def pad_num(p, numpads, bound=False):
    """A GoldenEye pad id in the converted level.

    GoldenEye's own pads keep their index and its bound pads are written after
    them, so a bound pad - one at 10000 and up, and a door's pad field, which is
    always one - is numpads plus its own index. 0xffff is not a pad at all: a
    collectable a guard carries has no place to stand, and 36 of Dam's did,
    which came out as pad 55902 and made the loader read past the pad table.
    """
    if p == NO_PAD:
        return NO_PAD
    if bound:
        return p + numpads
    return p + numpads - 10000 if p >= 10000 else p


def pad_of(t, raw, numpads):
    return pad_num(struct.unpack_from('>H', raw, 6)[0], numpads, bound=(t == 1))


def base_record(raw, pdtype, words, padnum):
    """GoldenEye's ObjectRecord as Perfect Dark's defaultobj."""
    out = bytearray(4 * words)
    out[0:3] = raw[0:3]
    out[3] = pdtype
    model = struct.unpack_from('>h', raw, 4)[0]
    struct.pack_into('>hHII', out, 4, MODEL_REMAKE_FIRST + model, padnum,
                     struct.unpack_from('>I', raw, 8)[0], struct.unpack_from('>I', raw, 12)[0])
    struct.pack_into('>hh', out, 0x4c, 0, 1000)      # damage, maxdamage
    struct.pack_into('>I', out, 0x58, 0x0fff0000)    # the floor colour GE-X gives them
    return out


def guard_record(raw, numpads, bodies):
    """GoldenEye's GuardRecord as Perfect Dark's packedchr.

    GoldenEye keeps a guard in seven words - chr, pad, body, ai list, the two
    presets, health, reaction time, its flags and a head - and Perfect Dark's
    chr is eleven and names most of the same things:

        0x00 chrindex   0x02 unk02      0x03 type       0x04 spawnflags
        0x08 chrnum     0x0a padnum     0x0c bodynum    0x0d headnum
        0x0e ailistnum  0x10 padpreset  0x12 chrpreset  0x14 hearscale
        0x16 viewdist   0x18 flags      0x1c flags2     0x20 team
        0x21 squadron   0x22 chair      0x24 convtalk   0x28 tude ...

    GoldenEye's own setup flags are Perfect Dark's spawn flags for the three it
    uses: sunglasses (0x01), sunglasses half the time (0x02) and invincible
    (0x08). Its 0x04 is "this is a clone", which Perfect Dark has no spawn flag
    for.

    The two fields the GoldenEye decomp calls health and reaction time are its
    **hearing scale and vision range** - chraction.c reads them straight into
    the chr's hearingscale (over a thousand) and visionrange - which are Perfect
    Dark's own two fields, so they carry across as they are.

    The body and head are GoldenEye's own character numbers and are left as
    they are: what there is to wear is not known until the mission loads, so the
    port maps them then (gexplus.c's gexPlusMissionChr()).
    """
    (chrnum, padid, bodyid, ailist, preset, chrpreset,
     hearscale, viewdist, flags, headid) = struct.unpack_from('>10h', raw, 4)
    out = bytearray(4 * PD_SIZES[OBJTYPE_CHR])
    out[0:3] = raw[0:3]
    out[3] = OBJTYPE_CHR
    body, head = bodies(bodyid, headid)
    struct.pack_into('>I', out, 0x04, flags & 0x000b)
    struct.pack_into('>hH', out, 0x08, chrnum, pad_num(padid & 0xffff, numpads))
    out[0x0c] = body & 0xff
    out[0x0d] = head & 0xff
    struct.pack_into('>HHHHH', out, 0x0e, ailist & 0xffff,
                     pad_num(preset & 0xffff, numpads), chrpreset & 0xffff,
                     hearscale & 0xffff, viewdist & 0xffff)
    struct.pack_into('>h', out, 0x22, -1)      # no chair
    return out


def door_record(raw, numpads, recs, index):
    """GoldenEye's DoorRecord as Perfect Dark's, the fields it moved put back.

    Learnt from GE-X's own copies of the eleven GoldenEye setups it kept pad for
    pad (CLAUDE-notes/ge-bean.md, "Runway and Caves"): the two fields at
    GoldenEye's 0x8c and 0x90 are times in a thousandth of Perfect Dark's, the
    flags' top byte moves 0x80 -> 0x40 and 0x08 -> 0x80 and drops 0x40, and the
    sibling is a record offset either way.
    """
    padnum = pad_of(1, raw, numpads)
    flags = geobjects._door_flags(struct.unpack_from('>I', raw, 8)[0])
    out = base_record(raw, 0x01, PD_SIZES[0x01], padnum)
    struct.pack_into('>I', out, 8, flags)
    for ge, pd, mul in ((0x84, 0x5c, 1), (0x88, 0x60, 1), (0x8c, 0x64, 1000), (0x90, 0x68, 1000),
                        (0x94, 0x6c, 1), (0x98, 0x70, 1), (0x9c, 0x74, 1), (0xa0, 0x78, 1)):
        struct.pack_into('>i', out, pd, struct.unpack_from('>i', raw, ge)[0] * mul)
    rel = struct.unpack_from('>i', raw, 0x80)[0]
    sib = index + rel
    struct.pack_into('>i', out, 0xbc, rel if rel and 0 <= sib < len(recs) and recs[sib][1] == 1 else 0)
    out[0xc6] = raw[0xa7]     # the opening sound
    out[0xcc] = 0xff
    return out


def weapon_record(raw, numpads):
    """A GoldenEye collectable as a Perfect Dark weapon prop, on the port's own
    GoldenEye weapons (geguns.c, WEAPON_GE_FIRST + GoldenEye's item)."""
    out = base_record(raw, 0x08, PD_SIZES[0x08], pad_of(8, raw, numpads))
    item = raw[0x80]
    out[0x5c] = WEAPON_GE_FIRST + item if item < GE_NUM_WEAPONS else 0
    out[0x5d] = 0xff          # no second gun
    out[0x5e] = 0xff
    struct.pack_into('>h', out, 0x62, struct.unpack_from('>h', raw, 0x82)[0])
    return out


def objective_record(raw):
    """GoldenEye's objective heading as Perfect Dark's.

    GoldenEye gives an objective the lowest difficulty it appears at; Perfect
    Dark keeps a bit per difficulty, so the bits from that difficulty up are set.
    Its four difficulties are Perfect Dark's three and PD Mode, which is the
    hardest one again, so 007 takes 00 Agent's bit.
    """
    out = bytearray(4 * PD_SIZES[0x17])
    out[0:3] = raw[0:3]
    out[3] = 0x17
    index, text, mindiff = struct.unpack_from('>3i', raw, 4)
    struct.pack_into('>ii', out, 4, index, text)
    bits = 0
    for d in range(3):
        if d >= min(mindiff, 2):
            bits |= 1 << d
    out[0x0f] = bits
    return out


def convert_props(d, numpads, bodies, models, stats):
    """The mission's props, one Perfect Dark record for each of GoldenEye's."""
    recs = ge_records(d)
    out = []
    for index, t, raw in recs:
        if t in AS_NOTHING or t not in PD_SIZES:
            stats['dropped'][t] = stats['dropped'].get(t, 0) + 1
            out.append(struct.pack('>I', OBJTYPE_NOTHING))
            continue
        if geobjects.GE_SIZES[t] >= 32:
            # every ObjectRecord names a model, doors and collectables included
            models.add(struct.unpack_from('>h', raw, 4)[0])
        if t == 1:
            out.append(bytes(door_record(raw, numpads, recs, index)))
        elif t == OBJTYPE_CHR:
            out.append(bytes(guard_record(raw, numpads, bodies)))
        elif t == 8:
            out.append(bytes(weapon_record(raw, numpads)))
        elif t == 0x17:
            out.append(bytes(objective_record(raw)))
        elif geobjects.GE_SIZES[t] >= 32:
            # an ObjectRecord and a tail
            rec = base_record(raw, t, PD_SIZES[t], pad_of(t, raw, numpads))
            for ge, pd, w, mul in OBJ_TAILS.get(t, ()):
                v = int.from_bytes(raw[ge:ge + w], 'big') * mul
                rec[pd:pd + w] = v.to_bytes(w, 'big')
            out.append(bytes(rec))
        else:
            # a short record: the same fields in the same order on both sides
            n = PD_SIZES[t]
            rec = bytearray(4 * n)
            keep = min(4 * n, len(raw))
            rec[0:keep] = raw[0:keep]
            rec[3] = t
            out.append(bytes(rec))
        stats['kept'][t] = stats['kept'].get(t, 0) + 1
    out.append(struct.pack('>I', OBJTYPE_END))
    return b''.join(out)


# GoldenEye's intro commands are Perfect Dark's own: the same types in the same
# order at the same widths, and only the end differs - GoldenEye stops at 9 and
# Perfect Dark at 12. **Type 6 is ten words**, which is what Perfect Dark's is
# (modrandom.c sizes INTROCMD_6 at 40 bytes) and what Dam's two of them lie at;
# geconvert.read_setup() had it at 13, which no multiplayer setup ever reached
# because none of them carries one.
GE_INTRO_WORDS = {0: 3, 1: 4, 2: 4, 3: 8, 4: 2, 5: 2, 6: 10, 7: 3, 8: 2}
INTROCMD_END = 12


def convert_intro(d):
    h = struct.unpack_from('>10I', d, 0)
    out, spawns = [], 0
    if not h[2]:
        return struct.pack('>i', INTROCMD_END), 0
    o = h[2]
    while o + 4 <= len(d):
        t = struct.unpack_from('>i', d, o)[0] & 0xff
        if t == 9:
            break
        n = GE_INTRO_WORDS.get(t)
        if n is None:
            break
        out.append(d[o:o + 4 * n])
        if t == 0:
            spawns += 1
        o += 4 * n
    out.append(struct.pack('>i', INTROCMD_END))
    return b''.join(out), spawns


def convert_paths(d, at):
    """GoldenEye's patrol paths, which are Perfect Dark's own record: a pointer
    to a -1 terminated list of pads, an id, a loop flag and a length."""
    h = struct.unpack_from('>10I', d, 0)
    if not h[4]:
        return struct.pack('>IBBH', 0, 0, 0, 0), b''
    paths, lists = [], []
    o = h[4]
    while True:
        ptr, pid, flags, ln = struct.unpack_from('>IBBH', d, o)
        if ptr == 0:
            break
        pads = []
        at2 = ptr
        while True:
            v = struct.unpack_from('>i', d, at2)[0]
            if v == -1:
                break
            pads.append(v)
            at2 += 4
        paths.append((pid, flags, ln, pads))
        o += 8
    head = 8 * (len(paths) + 1)
    pos = at + head
    body = b''
    out = b''
    for pid, flags, ln, pads in paths:
        out += struct.pack('>IBBH', pos, pid, flags, ln)
        blob = b''.join(struct.pack('>i', p) for p in pads) + struct.pack('>i', -1)
        body += blob
        pos += len(blob)
    out += struct.pack('>IBBH', 0, 0, 0, 0)
    return out, body


def ai_length(d, at):
    """The length of the GoldenEye AI command at `at`.

    Every command has its own fixed length but one: PRINT is a debug comment
    whose text follows the opcode and runs to a NUL (chrai.c's chraiitemsize()).
    A walk that does not measure it lands in the middle of the next command,
    which is what truncated 225 of the missions' lists.
    """
    op = d[at]
    if op >= len(geaitable.TABLE):
        return None
    ln = geaitable.TABLE[op][1]
    if ln is not None:
        return ln
    end = at + 1
    while end < len(d) and d[end]:
        end += 1
    return end - at + 1


def convert_ailist(d, at, stats, numpads):
    """One GoldenEye AI list as Perfect Dark bytecode (geaitable.py).

    A command's pad argument is moved the way a record's is: GoldenEye's bound
    pads are written after its own, so one at 10000 and up becomes numpads plus
    its index.
    """
    out = bytearray()
    while at < len(d):
        op = d[at]
        ln = ai_length(d, at)
        if ln is None:
            stats['ai_unknown'] += 1
            break
        name, _, args, pd, spec, why = geaitable.TABLE[op]
        if pd is None:
            stats['ai_dropped'][name] = stats['ai_dropped'].get(name, 0) + 1
        else:
            vals, o = [], at + 1
            for a, w in args:
                v = int.from_bytes(d[o:o + w], 'big')
                if 'PAD' in a and w >= 2:
                    v = pad_num(v, numpads)
                vals.append(v)
                o += w
            out += struct.pack('>H', pd)
            for s in spec:
                if isinstance(s, tuple) and s and s[0] == '=':
                    out += int(s[1]).to_bytes(s[2], 'big')
                elif isinstance(s, tuple):
                    out += (vals[s[0]] & ((1 << (8 * s[1])) - 1)).to_bytes(s[1], 'big')
                else:
                    out += vals[s].to_bytes(args[s][1], 'big')
            stats['ai_kept'] += 1
        at += ln
        if name == 'EndList':
            return bytes(out)
    out += struct.pack('>H', 0x0004)   # aiEndList, whatever the walk ran into
    return bytes(out)


def convert_ailists(d, at, stats, numpads):
    h = struct.unpack_from('>10I', d, 0)
    if not h[5]:
        return struct.pack('>Ii', 0, 0), b''
    rows = []
    o = h[5]
    while True:
        ptr, lid = struct.unpack_from('>Ii', d, o)
        if not ptr and not lid:
            break
        rows.append((lid, ptr))
        o += 8
    head = 8 * (len(rows) + 1)
    pos = at + head
    table, code = b'', b''
    for lid, ptr in rows:
        blob = convert_ailist(d, ptr, stats, numpads)
        table += struct.pack('>Ii', pos, lid)
        code += blob
        pos += len(blob)
    table += struct.pack('>Ii', 0, 0)
    return table, code


def convert(d, numpads, bodies):
    """A GoldenEye solo setup as a Perfect Dark one.

    `bodies(bodyid, headid)` gives the Perfect Dark body and head a GoldenEye
    character becomes. Returns (bytes, models used, statistics).
    """
    models = set()
    stats = dict(kept={}, dropped={}, ai_kept=0, ai_dropped={}, ai_unknown=0)
    props = convert_props(d, numpads, bodies, models, stats)
    intro, stats['spawns'] = convert_intro(d)
    header = 0x20
    intro_at = header
    props_at = intro_at + len(intro)
    paths_at = props_at + len(props)
    paths, pathpads = convert_paths(d, paths_at)
    ai_at = paths_at + len(paths) + len(pathpads)
    ailists, aicode = convert_ailists(d, ai_at, stats, numpads)
    out = struct.pack('>8I', 0, 0, 0, intro_at, props_at, paths_at, ai_at, 0)
    out += intro + props + paths + pathpads + ailists + aicode
    return out, models, stats
