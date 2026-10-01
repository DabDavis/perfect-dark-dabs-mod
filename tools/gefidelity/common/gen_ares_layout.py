"""Generates common/ares_layout.json: what aresge.py needs to read GoldenEye's
memory on the real cartridge - symbol addresses out of the decomp's N64 ELF and
struct layouts out of the native port's DWARF.

Run on the oracle host, from ~/claude-007/007:

    gdb -batch -x ~/gefidelity/common/gen_ares_layout.py build/port/ge007 > layout.json

The port is an -m32 build and its record structs are the ROM's own layouts (the
setup and model files are read in place), so its offsets are the cartridge's;
the generator checks the sizes it can against the decomp's comments. Symbols
come from build/u/ge007.u.elf, which builds the US ROM byte for byte.
"""
import gdb, json, subprocess, os

ELF = os.environ.get('GE_ELF', 'build/u/ge007.u.elf')
TYPES = ['PropDefHeaderRecord', 'ObjectRecord', 'DoorRecord', 'GuardRecord', 'PropRecord', 'ChrRecord',
         'PadRecord', 'BoundPadRecord', 'StandTile', 'AIListRecord', 'WeaponObjRecord', 'Model',
         'TintedGlassRecord', 'stagesetup', 'struct player', 'struct collision434', 'Mtxf', 'coord3d']
SYMBOLS = ['bossSetLoadedStage', 'proplvreset2', 'lvlRender', 'osViSwapBuffer', 'sizepropdef',
           'g_SelectedDifficulty', 'g_GlobalTimer', 'currentFrameCounter', 'g_CurrentSetup', 'g_ChrSlots',
           'g_NumChrSlots', 'g_CurrentPlayer', 'g_GlobalAILists', 'g_ClockTimer', 'g_BgCurrentRoom']
# exact sizes the decomp's own comments confirm; a mismatch means the port grew a struct
EXPECT = {'PadRecord': 0x2c, 'BoundPadRecord': 0x44, 'ObjectRecord': 0x80, 'PropRecord': 0x34}


def flatten(t, prefix='', base=0, out=None, depth=0):
    out = {} if out is None else out
    t = t.strip_typedefs()
    if t.code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION) or depth > 3:
        return out
    for f in t.fields():
        if f.bitpos is None:
            continue
        off = base + f.bitpos // 8
        name = f.name
        if name is None or name == '':
            flatten(f.type, prefix, off, out, depth + 1)    # anonymous union/struct: its members are ours
            continue
        ft = f.type.strip_typedefs()
        key = prefix + name
        ent = {'off': off, 'size': ft.sizeof, 'type': str(f.type)}
        if f.bitsize:
            ent['bitpos'] = f.bitpos % 8
            ent['bits'] = f.bitsize
        if ft.code == gdb.TYPE_CODE_ARRAY:
            ent['elem'] = ft.target().strip_typedefs().sizeof
            ent['count'] = ft.range()[1] + 1
        if key not in out:
            out[key] = ent
        if ft.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
            flatten(ft, key + '.', off, out, depth + 1)
    return out


layout = {'types': {}, 'symbols': {}, 'problems': []}
for name in TYPES:
    t = gdb.lookup_type(name)
    layout['types'][name] = {'size': t.strip_typedefs().sizeof, 'fields': flatten(t)}
    if name in EXPECT and t.sizeof != EXPECT[name]:
        layout['problems'].append('%s is %d bytes in the port, %d in the ROM' % (name, t.sizeof, EXPECT[name]))
nm = subprocess.run(['nm', ELF], capture_output=True, text=True).stdout.splitlines()
want = set(SYMBOLS)
for line in nm:
    p = line.split()
    if len(p) == 3 and p[2] in want:
        layout['symbols'][p[2]] = int(p[0], 16)
for s in SYMBOLS:
    if s not in layout['symbols']:
        layout['problems'].append('symbol %s not in %s' % (s, ELF))
# LEVELID_* by name, so the driver can swap a level in by the same name gdbge.py uses
layout['levelids'] = {}
lt = gdb.lookup_type('enum LEVELID') if True else None
for f in lt.fields():
    layout['levelids'][f.name] = f.enumval
print(json.dumps(layout, indent=1, sort_keys=True))
