"""Generates guns/ares_gun_layout.json: what gunscen_ares.py needs to measure a
gun on the real cartridge - the hand, inventory, weapon-stats and ammo layouts
from the native port's DWARF (an -m32 build whose structs are the ROM's), the
N64 ELF's symbols (every function's range, so a return address names its
caller) and the enums the give replicates.

Run on the oracle host, from ~/claude-007/007:

    gdb -batch -x ~/gefidelity/guns/gen_gun_layout.py build/port/ge007 > ~/gefidelity/guns/ares_gun_layout.json
"""
import gdb, json, subprocess, os

ELF = os.environ.get('GE_ELF', 'build/u/ge007.u.elf')
TYPES = ['struct hand', 'struct InvItem', 'struct WeaponStats', 'struct AmmoStats', 'struct GunModelFileRecord',
         'struct player', 'ChrRecord', 'PropRecord']
DATA = ['g_CurrentPlayer', 'g_GlobalTimer', 'currentFrameCounter', 'g_ChrSlots', 'g_NumChrSlots', 'gitem_structs',
        'ammo_related', 'default_weaponstats', 'g_CameraMode']
ENUMS = {'enum ITEM_IDS': ['ITEM_BOMBCASE', 'ITEM_UNARMED'],
         'enum INV_ITEM_TYPE': ['INV_ITEM_WEAPON'],
         'enum GUN_ANIMATION_STATE_IDS': ['GUN_ANIM_STATE_IDLE', 'GUN_ANIM_STATE_SWITCH_LOWER',
                                          'GUN_ANIM_STATE_SWITCH_SWAP']}


def flatten(t, prefix='', base=0, out=None, depth=0):
    out = {} if out is None else out
    t = t.strip_typedefs()
    if t.code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION) or depth > 4:
        return out
    for f in t.fields():
        if f.bitpos is None:
            continue
        off = base + f.bitpos // 8
        if not f.name:
            flatten(f.type, prefix, off, out, depth + 1)
            continue
        ft = f.type.strip_typedefs()
        key = prefix + f.name
        ent = {'off': off, 'size': ft.sizeof, 'type': str(f.type)}
        if f.bitsize:
            ent['bitpos'] = f.bitpos % 8
            ent['bits'] = f.bitsize
        if ft.code == gdb.TYPE_CODE_ARRAY:
            ent['elem'] = ft.target().strip_typedefs().sizeof
            ent['count'] = ft.range()[1] + 1
        out.setdefault(key, ent)
        if ft.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
            flatten(ft, key + '.', off, out, depth + 1)
    return out


layout = {'types': {}, 'symbols': {}, 'functions': [], 'enums': {}, 'problems': []}
for name in TYPES:
    t = gdb.lookup_type(name)
    layout['types'][name] = {'size': t.strip_typedefs().sizeof, 'fields': flatten(t)}
nm = subprocess.run(['nm', '-n', ELF], capture_output=True, text=True).stdout.splitlines()
for line in nm:
    p = line.split()
    if len(p) != 3:
        continue
    a = int(p[0], 16)
    if p[2] in DATA:
        layout['symbols'][p[2]] = a
    if p[1] in 'Tt' and (0x7000_0000 <= a < 0x7100_0000 or 0x7f00_0000 <= a < 0x8000_0000):
        layout['functions'].append([a, p[2]])
for s in DATA:
    if s not in layout['symbols']:
        layout['problems'].append('symbol %s not in %s' % (s, ELF))
for en, names in ENUMS.items():
    try:
        vals = {f.name: f.enumval for f in gdb.lookup_type(en).fields()}
    except gdb.error as e:
        layout['problems'].append('%s: %s' % (en, e))
        continue
    for n in names:
        if n in vals:
            layout['enums'][n] = vals[n]
        else:
            layout['problems'].append('%s not in %s' % (n, en))
# the WeaponStats flag the dual rule reads (bondinv.c bondinvItemAvailableForHand, BUGFIX_R0 on the US cartridge)
layout['enums']['WEAPONSTATBITFLAG_CAN_DUAL_WIELD'] = 0x100000
layout['enums']['WEAPONSTATBITFLAG_AMMO_CLIP_LIMIT'] = 0x200000
print(json.dumps(layout, indent=1, sort_keys=True))
