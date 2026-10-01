#!/usr/bin/env python3
"""Regenerate fields.json: GoldenEye's record layouts, field by field, out of
the DWARF of the decomp's native port (an i386 build, so its pointers are four
bytes and every offset is the ROM's own).

    gen_fields.py [--host sdg@10.8.0.3] [--bin ~/claude-007/007/build/port/ge007]

Builds a probe object on the host the way the port's own
port/tools/gen_setup_layout.py does (one variable of each type, compiled with
the port's flags, so a type no code uses still has DWARF), then runs gdb on it
with a Python script that flattens each type into leaves (byte offset, size,
field path, C type); census.py names an offset with them. Read only: nothing on
the host is written but files under /tmp, removed after.
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = 'sdg@10.8.0.3'
BIN = '~/claude-007/007/build/port/ge007'

TYPES = [
    # setup propdefs (port/shim/setup_bswap.c portSetupPropdef) and sections
    'DoorRecord', 'GlobalDoorScaleRecord', 'ObjectRecord', 'KeyRecord', 'CCTVRecord',
    'AmmoCrateRecord', 'WeaponObjRecord', 'GuardRecord', 'MonitorObjRecord',
    'MultiMonitorObjRecord', 'AutogunRecord', 'LinkRecord', 'HatRecord', 'GuardAttributeRecord',
    'MultiAmmoCrateRecord', 'BodyArmourRecord', 'TagObjectRecord', 'MissionObjectiveRecord',
    'RenameObjectRecord', 'LockDoorRecord', 'VehichleRecord', 'AircraftRecord', 'GlassRecord',
    'SafeRecord', 'SafeObjectRecord', 'TankRecord', 'CutsceneRecord', 'TintedGlassRecord',
    'PropDefHeaderRecord', 'struct watchMenuObjectiveText',
    'SetupIntroSpawn', 'SetupIntroCamera', 'struct SetupIntroItem', 'struct SetupIntroAmmo',
    'struct SetupIntroSwirl', 'struct SetupIntroAnim', 'struct SetupIntroCuff',
    'struct SetupIntroWatch', 'struct SetupIntroCredits', 'SetupIntroEmpty',
    'PadRecord', 'BoundPadRecord', 'waypoint', 'waygroup', 'PathRecord', 'AIListRecord',
    'stagesetupfile',
    # bg and stan
    'bg_room_data', 'bg_portal_data_entry', 'bg_portal_entry', 'StandFileHeader',
    'StandFilePoint', 'StandFileFooter', 'Vtx_t',
    # models
    'ModelFileHeader', 'ModelNode', 'ModelSkeleton', 'ModelFileTextures',
    'ModelRoData_HeaderRecord', 'ModelRoData_GroupRecord', 'ModelRoData_DisplayListRecord',
    'ModelRoData_Op05Record', 'ModelRoData_Op06Record', 'ModelRoData_Op07Record',
    'ModelRoData_LODRecord', 'ModelRoData_BSPRecord', 'ModelRoData_BoundingBoxRecord',
    'ModelRoData_Op11Record', 'ModelRoData_GunfireRecord', 'ModelRoData_ShadowRecord',
    'ModelRoData_Op14Record', 'ModelRoData_InterlinkageRecord', 'ModelNode_Op16Record',
    'ModelRoData_Op17Record', 'ModelRoData_SwitchRecord', 'ModelRoData_GroupSimpleRecord',
    'ModelRoData_DisplayListPrimaryRecord', 'ModelRoData_HeadPlaceholderRecord',
    'ModelRoData_DisplayList_CollisionRecord',
    'ItemModelFileRecord', 'ChrModelFileRecord',
    # data segment rows and animations
    'EnvironmentRecord', 'ModelAnimation', 'ModelAnimRootMotionChannel',
]

GDB_SCRIPT = r'''
import gdb, json
out = {}
def leaves(t, base, path, acc, depth=0):
    t = t.strip_typedefs()
    code = t.code
    if code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION) and depth < 8:
        alts = []
        for f in t.fields():
            if not hasattr(f, 'bitpos'):
                continue
            name = f.name or '<anon>'
            if f.bitsize:
                acc.append([base + f.bitpos // 8, max(1, (f.bitpos % 8 + f.bitsize + 7) // 8),
                            path + name + ':%d' % f.bitsize, str(f.type)])
                continue
            leaves(f.type, base + f.bitpos // 8, path + name + '.', acc, depth + 1)
        return
    if code == gdb.TYPE_CODE_ARRAY and depth < 8:
        r = t.range()
        n = r[1] - r[0] + 1
        el = t.target()
        if n <= 0:
            return
        es = el.strip_typedefs().sizeof
        if n <= 64 or el.strip_typedefs().code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
            for i in range(min(n, 512)):
                leaves(el, base + i * es, path[:-1] + '[%d].' % i, acc, depth + 1)
        else:
            acc.append([base, n * es, path[:-1] + '[%d]' % n, str(t)])
        return
    acc.append([base, t.sizeof, path[:-1], str(t)])
for name in NAMES:
    try:
        t = gdb.lookup_type(name.replace('struct ', '')) if not name.startswith('struct ') else gdb.lookup_type(name[7:])
    except gdb.error:
        try:
            t = gdb.parse_and_eval('(%s *)0' % name).type.target()
        except gdb.error as e:
            out[name] = dict(error=str(e))
            continue
    acc = []
    leaves(t, 0, '', acc)
    out[name] = dict(size=t.strip_typedefs().sizeof, leaves=acc)
open(OUTPATH, 'w').write(json.dumps(out))
'''


def main():
    host, binary = HOST, BIN
    a = sys.argv[1:]
    if '--host' in a:
        host = a[a.index('--host') + 1]
    if '--bin' in a:
        binary = a[a.index('--bin') + 1]
    repo = os.path.dirname(os.path.dirname(os.path.dirname(binary)))
    script = 'NAMES = %r\nOUTPATH = "/tmp/gefidelity_fields.json"\n' % TYPES + GDB_SCRIPT
    probe = '#include <ultra64.h>\n#include <bondtypes.h>\n#include "game/bg.h"\n#include "game/bgfog.h"\n' + ''.join(
        '%s v_%d;\n' % (t, i) for i, t in enumerate(TYPES))
    subprocess.run(['ssh', '-o', 'BatchMode=yes', host, 'cat > /tmp/gefidelity_fields.py'],
                   input=script.encode(), check=True)
    subprocess.run(['ssh', '-o', 'BatchMode=yes', host, 'cat > /tmp/gefidelity_probe.c'],
                   input=probe.encode(), check=True)
    flags = ("python3 -c \"import re;c=dict(re.findall(r'^(PORT_\\w+)\\s*:?=\\s*(.*)$',"
             "open('port/config.mk').read(),re.M));print(' '.join([c['PORT_ARCH'],c['PORT_SHIM_INCLUDE'],"
             "c['PORT_DEFINES'],c['PORT_PERMIT']]))\"")
    subprocess.run(['ssh', '-o', 'BatchMode=yes', host,
                    'cd %s && gcc -g -gdwarf-4 $(%s) -c -o /tmp/gefidelity_probe.o /tmp/gefidelity_probe.c '
                    '&& gdb -batch -x /tmp/gefidelity_fields.py /tmp/gefidelity_probe.o > /tmp/gefidelity_fields.log 2>&1; '
                    'tail -3 /tmp/gefidelity_fields.log' % (repo, flags)], check=True)
    data = subprocess.run(['ssh', '-o', 'BatchMode=yes', host,
                           'cat /tmp/gefidelity_fields.json; rm -f /tmp/gefidelity_fields.json '
                           '/tmp/gefidelity_fields.py /tmp/gefidelity_fields.log /tmp/gefidelity_probe.c '
                           '/tmp/gefidelity_probe.o'],
                          check=True, capture_output=True).stdout
    fields = json.loads(data)
    bad = [k for k, v in fields.items() if 'error' in v]
    fields['_source'] = '%s:%s tree, a probe built with the port flags (i386 DWARF)' % (host, repo)
    json.dump(fields, open(os.path.join(HERE, 'fields.json'), 'w'), indent=0, sort_keys=True)
    print('fields.json: %d types, missing %s' % (len(fields) - 1 - len(bad), bad))


if __name__ == '__main__':
    main()
