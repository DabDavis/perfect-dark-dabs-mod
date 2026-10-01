"""Generates common/ares_layout_wide.json: the wide world dump's layouts (doors,
every object record type, guard records, the stan file) - gen_ares_layout.py's
method (the -m32 port's DWARF for layouts, the N64 ELF for symbols) with more
types and symbols, kept apart so the shared ares_layout.json never changes
under a run.

Run on the oracle host, from ~/claude-007/007:

    gdb -batch -x gen_ares_layout_wide.py build/port/ge007 > ares_layout_wide.json
"""
import gdb, json, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) if '__file__' in dir() else '.')
ELF = os.environ.get('GE_ELF', 'build/u/ge007.u.elf')
TYPES = ['PropDefHeaderRecord', 'ObjectRecord', 'DoorRecord', 'GuardRecord', 'GuardAttributeRecord',
         'WeaponObjRecord', 'AmmoCrateRecord', 'MultiAmmoCrateRecord', 'KeyRecord', 'HatRecord',
         'MonitorRecord', 'MultiMonitorObjRecord', 'MonitorObjRecord', 'AutogunRecord', 'GlassRecord',
         'TintedGlassRecord', 'BodyArmourRecord', 'CCTVRecord', 'GasReleasingRecord', 'SafeRecord',
         'VehichleRecord', 'AircraftRecord', 'TankRecord', 'LinkRecord', 'LockDoorRecord', 'RenameObjectRecord',
         'TagObjectRecord', 'StandTile', 'stagesetup', 'PropRecord', 'ChrRecord', 'Model']
SYMBOLS = ['standTileStart', 'room_data_float2', 'g_CurrentSetup', 'g_GlobalAILists', 'g_ChrSlots',
           'g_NumChrSlots', 'g_DoorScale']


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
            flatten(f.type, prefix, off, out, depth + 1)
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
    try:
        t = gdb.lookup_type(name)
    except gdb.error as e:
        layout['problems'].append('type %s: %s' % (name, e))
        continue
    layout['types'][name] = {'size': t.strip_typedefs().sizeof, 'fields': flatten(t)}
nm = subprocess.run(['nm', ELF], capture_output=True, text=True).stdout.splitlines()
for line in nm:
    p = line.split()
    if len(p) == 3 and p[2] in SYMBOLS:
        layout['symbols'][p[2]] = int(p[0], 16)
for s in SYMBOLS:
    if s not in layout['symbols']:
        layout['problems'].append('symbol %s not in %s' % (s, ELF))
print(json.dumps(layout, indent=1, sort_keys=True))
