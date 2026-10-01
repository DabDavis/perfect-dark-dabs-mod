#!/usr/bin/env python3
"""GoldenEye level files out of the ROM: bg, stan, setups, inflated.

Read from the ROM alone (gerom.py). bg files are stored whole (their room
blobs are 1172-compressed inside); stan and setup files are 1172 over the
whole file.
"""
import os, struct, zlib
import gerom

# the GoldenEye decomp: only the offline HD fit tools read it (gemodelconv.prop_names())
GE = '/home/sdg/perfect-dark/claude-007/007'
ROM = os.path.join(GE, 'baserom.u.z64')

# level key: (bg name, stan name, solo setup, mp setup or None, levelscale)
LEVELS = {
    'dam':  ('bg_dam',  'Tbg_dam',  'UsetupdamZ',    None,               0.23363999),
    'run':  ('bg_run',  'Tbg_run',  'UsetuprunZ',    None,               0.089571431),
    'stat': ('bg_stat', 'Tbg_stat', 'UsetupstatueZ', 'Ump_setupstatueZ', 0.107202865),
    'tra':  ('bg_tra',  'Tbg_tra',  'UsetuptraZ',    None,               0.15019713),
    'pete': ('bg_pete', 'Tbg_pete', 'UsetuppeteZ',   None,               0.34187999),
    'jun':  ('bg_jun',  'Tbg_jun',  'UsetupjunZ',    None,               0.094662853),
    'oat':  ('bg_oat',  'Tbg_oat',  None,            'Ump_setupoatZ',    0.14142857),
    # GoldenEye's other multiplayer levels (setup_text_pointers in chraidata.c:
    # Library, Basement and Stack are three setups on one level)
    'dish': ('bg_dish', 'Tbg_dish', None,            'Ump_setupdishZ',   0.47142857),
    'ref':  ('bg_ref',  'Tbg_ref',  None,            'Ump_setuprefZ',    0.94285715),
    'lib':  ('bg_ame',  'Tbg_ame',  None,            'Ump_setupameZ',    0.65999997),
    'base': ('bg_ame',  'Tbg_ame',  None,            'Ump_setupimpZ',    0.65999997),
    'stack': ('bg_ame', 'Tbg_ame',  None,            'Ump_setupashZ',    0.65999997),
    'ark':  ('bg_ark',  'Tbg_ark',  None,            'Ump_setuparkZ',    1.20648),
    'sevb': ('bg_sevb', 'Tbg_sevb', None,            'Ump_setupsevbZ',   0.53931433),
    'arch': ('bg_arch', 'Tbg_arch', None,            'Ump_setuparchZ',   0.50678575),
    'cave': ('bg_cave', 'Tbg_cave', None,            'Ump_setupcaveZ',   0.26824287),
    'cryp': ('bg_cryp', 'Tbg_cryp', None,            'Ump_setupcrypZ',   0.25608),
    'crad': ('bg_crad', 'Tbg_crad', None,            'Ump_setupcradZ',   0.23571429),
    # and its solo levels with no multiplayer setup
    'sevx': ('bg_sevx', 'Tbg_sevx', 'UsetupsevxZ',   None,               0.45445713),
    'sevxb': ('bg_sevx', 'Tbg_sevx', 'UsetupsevxbZ', None,               0.45445713),
    'silo': ('bg_silo', 'Tbg_silo', 'UsetupsiloZ',   None,               0.47256002),
    'dest': ('bg_dest', 'Tbg_dest', 'UsetupdestZ',   None,               0.44757429),
    'depo': ('bg_depo', 'Tbg_depo', 'UsetupdepoZ',   None,               0.21847887),
    'arec': ('bg_arec', 'Tbg_arec', 'UsetupcontrolZ', None,              0.49886572),
    'sev':  ('bg_sev',  'Tbg_sev',  'UsetupsevbunkerZ', None,            0.53931433),
    'azt':  ('bg_azt',  'Tbg_azt',  'UsetupaztZ',    None,               0.35300568),
}


def inflate1172(b):
    assert b[:2] == b'\x11\x72', b[:4].hex()
    return zlib.decompressobj(-15).decompress(b[2:])


_rom = None


def rom():
    """The player's GoldenEye ROM (US): $GE_ROM, else the decomp's baserom."""
    global _rom
    if _rom is None:
        _rom = gerom.Rom(os.environ.get('GE_ROM', ROM))
    return _rom


# Faults in GoldenEye's own data, mended as the file is read (geconvert.c's
# g_RomPatches, which is this): (file, offset, the ROM's bytes, what they
# become). A file whose bytes are not the ROM's is left as it is. All are faults
# the community found in the XBLA release's copy of the same data and mended in
# its Community Edition; none is a change of design.
ROM_PATCHES = (
    # Bunker ii: two tiles meeting on the stairs (room 0x14, y 93) are
    # unlinked from both sides; each link names the other tile
    ('Tbg_sevb_all_p_stanZ', 0x65c2, '0000', '0ccd'),
    ('Tbg_sevb_all_p_stanZ', 0x6612, '0000', '0cc4'),
    # Silo: armour 59 gives half, as the others do, drawn as the full suit
    ('UsetupsiloZ', 0x4efc, '0073', '0074'),
    # Control: the blast door on pad 146 (object 184) without DOORFLAG_0004
    ('UsetupcontrolZ', 0xb8d4, '00000004', '00040004'),
    # Surface: a guard's pair of Klobbs (ai_31) without the paired flag
    ('UsetupsevxZ', 0x10794, '00', '80'),
    ('UsetupsevxZ', 0x107a3, '10', '90'),
    # Surface and Surface 2: the path pad by the outside railing, z -5001 -> -4968
    ('UsetupsevxZ', 0x31b0, 'c59c4800', 'c59b4000'),
    ('UsetupsevxbZ', 0x3024, 'c59c4800', 'c59b4000'),
    # Egyptian: the Golden Gun's glass case (door_win, objects 45-48) without
    # DOORFLAG_0004, as Control's blast door
    ('UsetupcrypZ', 0x4b5c, '0008', '000c'),
    ('UsetupcrypZ', 0x4c5c, '0008', '000c'),
    ('UsetupcrypZ', 0x4d5c, '0008', '000c'),
    ('UsetupcrypZ', 0x4e5c, '0008', '000c'),
)


def rom_file(stem):
    if stem.startswith('Tbg_'):
        stem += '_all_p_stanZ'
    elif stem.startswith('bg_'):
        stem += '_all_p'
    d = rom().file(stem)
    # since converter 97 the fixes make only the HD look's "_ce" copies
    # (geconvert.c's g_RomPatchesOn, romFileCe()); the files the N64 look
    # loads - and ai/aimap.py maps - are the cartridge's. GE_ROM_PATCHES=1 for
    # the copies.
    if os.environ.get('GE_ROM_PATCHES') == '1' and any(p[0] == stem for p in ROM_PATCHES):
        d = bytearray(d)
        for name, at, old, new in ROM_PATCHES:
            old, new = bytes.fromhex(old), bytes.fromhex(new)
            if name == stem and d[at:at + len(old)] == old:
                d[at:at + len(new)] = new
        d = bytes(d)
    return d


# GoldenEye's Japanese and PAL cartridges changed eight of the US one's setup
# files, and the PAL one five of its bg files' portals (geconvert.c's
# revisionSetup(), which is this: the long form of every change is there).
# GE Plus plays either (Mod.GePlusRevisionFixes), so a conversion writes both:
# each changed setup again as the later cartridges have it, `revsetup` on a
# mission line and `revmpsetup` on a map line, and the changed portals'
# thickness codes as `revportals`. Cuba's, the eighth, changed only its credits
# (gecredits.c). Checked against both ROMs; a setup any one of whose changes
# does not find the US bytes it replaces gets no second file.
REVISION_WORDS = (
    # Frigate: the third opening shot, moved
    ('UsetupdestZ', 0xbb3c, 0x0000801b, 0x0000a5dc),
    ('UsetupdestZ', 0xbb40, 0x00006ea3, 0x000066d3),
    ('UsetupdestZ', 0xbb44, 0xfffa7a99, 0xfffa5a45),
    ('UsetupdestZ', 0xbb48, 0x0005bfa9, 0x0005aaff),
    ('UsetupdestZ', 0xbb4c, 0x0005d453, 0x00060101),
    # Jungle: objective 3 complete on bit 0x40000, not the spawners' 0x8000
    ('UsetupjunZ', 0x6900, 0x00008000, 0x00040000),
    # Archives (the arena): the prop on bound pad 10249 PROPFLAG2_NOFALL
    ('Ump_setuparchZ', 0x7f98, 0x00000000, 0x00000100),
)

# Silo's opening: four more shots after the US one (Wreck's report)
REVISION_SILO_SHOT = (0x00000006, 0x0007d0b2, 0xfffe687b, 0x000430b2, 0x000561d5, 0x00060101, 0x00000081,
                      0x00008844, 0x00008845, 0x00000000)
REVISION_SILO_SHOTS = (
    (0x00000006, 0x0006ee17, 0xfffe31d9, 0x00019306, 0x0005adca, 0x000097e9, 0x0000000c, 0x00008844, 0x00008845, 0x00000000),
    (0x00000006, 0x00040d2a, 0x0000115e, 0x0003aad5, 0x0002c9c5, 0x00052a8a, 0x000000dd, 0x00008844, 0x00008845, 0x00000000),
    (0x00000006, 0x000045aa, 0xfffdd9d6, 0x0003beac, 0x0004240b, 0x00010325, 0x000000d8, 0x00008844, 0x00008845, 0x00000000),
    (0x00000006, 0xfffcbca9, 0x00003e47, 0x000153ee, 0x000294b6, 0x0005c274, 0x0000005f, 0x00008844, 0x00008845, 0x00000000),
)

# the AI lists they changed, whole: (file, list id, the US list's length and
# CRC-32 or 0 for a list they added, the list)
REVISION_LISTS = (
    ('UsetupstatueZ', 0x1009, 0, 0, bytes.fromhex(
        'a0010010000003023303f70023013302239a0020000005fd000104')),
    ('UsetupjunZ', 0x0409, 158, 0xe3ac8041, bytes.fromhex(
        'ad6d646b00eb26020351006426035100c82c0351012c2c05fd040c022c40fc36000702363335803614002400fc3d0236'
        '15002400fc3d17000400fc2c022cae020d03b400003c2c010d022c40fc36000702360103023d97fc80ae020b0330fc4b'
        'b400003c2c010b022c16000400fc2c022c020c30fc4b31fc4b03b400012c2c2f2c010c022c3335802c0103022c020798'
        'fc8005fd0407024b05fd040a022605fd040b04')),
    ('UsetupjunZ', 0x040b, 159, 0x898d9724, bytes.fromhex(
        'ad6576616400911497fc803335800a0a000b0014003c02100208032f2c0108022c020a15002400fc3d023dae020b0330'
        'fc4bb400003c2c010b022c4f0019fc0916000400fc2c022c020c03b400012c2c2f2c010c022c30fc4b3335802c010802'
        '2c020798fc8005fd0407024b05fd040a0209032f2ce72c0109022c16000400fc2c05fd0407022c0319fdfc08ad636865'
        '61700098fc80020d032f2c010d022c05fd040704')),
    ('UsetupjunZ', 0x1002, 187, 0x926bb643, bytes.fromhex(
        '020303eb4c5501d42c0103022c9a00040000ecd700da020803dc2c0108022c31014b30014bf12c05fd000f022cddeadf'
        '1fdf2059042c59052c59062c59072c59082c59092c590a2c590b2c590c2c590d2c590e2c590f2c59102c59112c59122c'
        '59132c59162c59142c59152ce404022ced030303d51e00020000a10100000400a00104800000d901024f2c022c050104'
        '1ea0f804800000d9f802502c022c05f8041f0303dbf40100ff05fd0001024bd205fd0001024c05fd000104')),
    ('UsetupcradZ', 0x041d, 117, 0x730026ac, bytes.fromhex(
        'eb3decd700da020703dc290107022931003000290230bd08ff0092041e0000001029022903033100300029023005fd04'
        '1f0229ddeaed030303d50400020000a100000004000500041a5500740a5500940aa0f8000004000029020a05f8041b02'
        '290208039c20000000290108022905fd041f023d05fd000104')),
    ('UsetupcradZ', 0x041f, 40, 0x62b20580, bytes.fromhex(
        'eb3decd700e300ddeaed030303d50100010000a1f800000400a0f81800000005f8041905fd0001023d05fd000104')),
    ('UsetuptraZ', 0x1003, 178, 0x8dba61fc, bytes.fromhex(
        '0206039c000008000101060201ae0211033046023146023048353148350202b40003840101110235b400070801011102'
        '01aec390120212b400012c010301120201c39013b7003cb9b5021303960435bb001e140235960235bb00170202359601'
        '35bb0014010235bb00001501130201c4003a00c70000000e10940101130202c901c400d702c602004700009402011302'
        '14c400d603c6030047000003c400d801c60100470000940401130215b69a00080000020803010804')),
)

# the PAL cartridge's portal thicknesses: (bg, the portal's place in the table,
# the US code, the PAL one)
REVISION_PORTALS = (
    ('bg_arch', 81, 0x0c, 0x28), ('bg_arch', 82, 0x0c, 0x28), ('bg_arch', 83, 0x38, 0x4a),
    ('bg_arec', 99, 0x00, 0x19), ('bg_arec', 100, 0x00, 0x19), ('bg_arec', 101, 0x09, 0x19),
    ('bg_arec', 102, 0x00, 0x19), ('bg_arec', 103, 0x00, 0x19), ('bg_arec', 104, 0x00, 0x19),
    ('bg_arec', 105, 0x09, 0x19),
    ('bg_jun', 22, 0x00, 0x2b),
    ('bg_pete', 50, 0x49, 0x5a),
    ('bg_silo', 63, 0x00, 0x08),
)


def revision_setup(stem, d):
    """The setup `stem` as the later cartridges have it, out of the US one `d`
    (as rom_file() reads it), or None where they have the US one's. What
    changed length goes after the end of the file and is pointed at - Silo's
    intro, each changed list, the list table where one is added."""
    f = bytearray(d)
    changes = missed = 0
    u32 = lambda o: struct.unpack_from('>I', f, o)[0]
    pad4 = lambda: f.extend(b'\0' * (-len(f) % 4))
    for name, at, old, new in REVISION_WORDS:
        if name != stem:
            continue
        if at + 4 <= len(f) and u32(at) == old:
            struct.pack_into('>I', f, at, new)
            changes += 1
        else:
            missed += 1
    if stem == 'UsetupsiloZ':
        words = (3, 4, 4, 8, 2, 2, 10, 3, 2)
        at = u32(8) if len(f) >= 12 else 0
        ok = at != 0 and at + 40 <= len(f) and struct.unpack_from('>10I', f, at) == REVISION_SILO_SHOT
        end = at
        while ok:
            t = u32(end) & 0xff if end + 4 <= len(f) else 0xff
            if t == 9:
                end += 4
                break
            if t >= 9 or end + 4 * words[t] > len(f):
                ok = False
                break
            end += 4 * words[t]
        if ok:
            newat = len(f)
            f += d[at:at + 40]
            for shot in REVISION_SILO_SHOTS:
                f += struct.pack('>10I', *shot)
            f += d[at + 40:end]
            struct.pack_into('>I', f, 8, newat)
            changes += 1
        else:
            missed += 1
    table = u32(20) if len(f) >= 24 else 0
    rows = []
    o = table
    while table and o + 8 <= len(f) and (u32(o) or u32(o + 4)):
        rows.append([u32(o), u32(o + 4)])
        o += 8
    lists = 0
    for name, lid, uslen, uscrc, data in REVISION_LISTS:
        if name != stem:
            continue
        row = next((r for r in rows if r[1] == lid), None)
        if uslen:
            if row is None or row[0] + uslen > len(f) or zlib.crc32(bytes(f[row[0]:row[0] + uslen])) != uscrc:
                missed += 1
                continue
        elif row is not None:
            missed += 1
            continue
        else:
            row = [0, lid]
            rows.append(row)
        pad4()
        row[0] = len(f)
        f += data
        lists += 1
    if lists:
        pad4()
        struct.pack_into('>I', f, 20, len(f))
        for at, lid in rows:
            f += struct.pack('>II', at, lid)
        f += b'\0' * 8
        changes += lists
    if missed:
        print('%s: %d of the later cartridges\' changes did not find the US bytes; only the US setup is written'
              % (stem, missed))
        return None
    if not changes:
        return None
    f.extend(b'\0' * (-len(f) % 16))
    return bytes(f)


class Bg:
    """A bg file: rooms {pos, vertices (16-byte Vtx, room-relative), primary and
    secondary display lists}, portals, vis commands. Pointers are 0x0f segment."""

    def __init__(self, data):
        self.d = d = data
        u = lambda o: struct.unpack_from('>I', d, o)[0]
        seg = lambda a: a & 0xffffff
        rooms_at, portals_at, vis_at = seg(u(4)), seg(u(8)), seg(u(12))
        self.rooms = []
        entries = []
        i = 0
        while True:
            o = rooms_at + 24 * i
            pt, pri, sec = u(o), u(o + 4), u(o + 8)
            pos = struct.unpack_from('>3f', d, o + 12)
            entries.append((pt, pri, sec, pos))
            if i > 0 and pri == 0:
                break
            i += 1
        # blob sizes run to the next non-zero offset in the file
        offs = sorted(set(seg(x) for e in entries for x in e[:3] if x))

        def blob(addr):
            if not addr:
                return None
            a = seg(addr)
            nxt = [x for x in offs if x > a]
            end = nxt[0] if nxt else len(d)
            return inflate1172(d[a:end])
        # entry 0 is empty, the last two are the end sentinel and the zero entry
        self.numrooms = len(entries) - 3
        for pt, pri, sec, pos in entries[1:1 + self.numrooms]:
            self.rooms.append(dict(pos=pos, vtx=blob(pt), pri=blob(pri), sec=blob(sec)))
        self.portals = []
        o = portals_at
        while u(o):
            p = seg(u(o))
            n = d[p]
            pts = [struct.unpack_from('>3f', d, p + 4 + 12 * k) for k in range(n)]
            self.portals.append(dict(points=pts, room1=d[o + 4], room2=d[o + 5], ctrl=struct.unpack_from('>H', d, o + 6)[0],
                                     vtxptr=u(o)))
            o += 8
        self.vis = []
        o = vis_at
        while vis_at and o + 8 <= len(d):
            t, ln = d[o], d[o + 1]
            arg = struct.unpack_from('>i', d, o + 4)[0]
            self.vis.append((t, ln, arg))
            o += 8
            if t == 0 and ln == 0 and arg == 0:
                break

    def world_vertices(self):
        out = []
        for r, room in enumerate(self.rooms, 1):
            v = room['vtx'] or b''
            for k in range(len(v) // 16):
                x, y, z = struct.unpack_from('>3h', v, 16 * k)
                out.append((room['pos'][0] + x, room['pos'][1] + y, room['pos'][2] + z, r))
        return out


if __name__ == '__main__':
    for key, (bg, stan, solo, mp, ls) in LEVELS.items():
        b = Bg(rom_file(bg))
        vs = b.world_vertices()
        st = rom_file(stan)
        print('%-5s rooms %3d portals %3d vis %3d vertices %6d stan %6d bytes solo %s mp %s' % (
            key, b.numrooms, len(b.portals), len(b.vis), len(vs), len(st),
            len(rom_file(solo)) if solo else '-', len(rom_file(mp)) if mp else '-'))
