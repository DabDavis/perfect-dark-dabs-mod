#!/usr/bin/env python3
"""Map GoldenEye N64 models (decomp Model.c) to GE-X files by their vertex arrays.

Every Vertex array of every GE chr model is signed by its first three xyz and
tagged with the joint of the group it hangs under; every GE-X C* file is
searched for every signature. Writes gemap.json:
  {gemodel: {gexfile: {"arrays": n, "joints": {joint: n}}}, "_arrays": {gemodel: total}}
"""
import collections, glob, json, os, re, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
GEDIR = os.path.join(paths.GE_DECOMP, 'assets/obseg/chr')
GEX = paths.GEX_FILES


def ge_arrays(path):
    src = re.sub(r'//[^\n]*', '', open(path).read())
    nodes = {}
    for m in re.finditer(r'ModelNode\s+(ModelNode_0x[0-9a-f]+)\s*=\s*\{(.*?)\};', src, re.S):
        parts = [p.strip() for p in m.group(2).split(',')]
        ref = lambda p: (re.search(r'&(\w+)', p).group(1) if '&' in p else None)
        nodes[m.group(1)] = dict(data=ref(parts[1]), parent=ref(parts[2]))
    groups = {}
    for m in re.finditer(r'ModelRoData_GroupRecord\s+(GroupRecord_0x[0-9a-f]+)\s*=\s*\{\s*\{([^}]*)\}\s*,\s*(0x[0-9A-Fa-f]+|\d+)', src):
        groups[m.group(1)] = int(m.group(3), 0)
    bydata = {n['data']: k for k, n in nodes.items()}
    verts = {}
    for m in re.finditer(r'Vertex\s+(Vertex_0x[0-9a-f]+)\[[^\]]*\]\s*=\s*\{(.*?)\};', src, re.S):
        nums = [int(x, 0) for x in re.findall(r'-?(?:0x[0-9A-Fa-f]+|\d+)', m.group(2))]
        verts[m.group(1)] = nums
    out = []
    for m in re.finditer(r'ModelRoData_DisplayList\w*\s+(\w+)\s*=\s*\{(.*?)\};', src, re.S):
        vref = re.findall(r'(Vertex_0x[0-9a-f]+)', m.group(2))
        if not vref or vref[0] not in verts or len(verts[vref[0]]) < 30:
            continue
        joint, node = None, bydata.get(m.group(1))
        while node:
            d = nodes[node]['data']
            if d in groups:
                joint = groups[d]
                break
            node = nodes[node]['parent']
        nums = verts[vref[0]]
        sig = b''.join(struct.pack('>hhh', *nums[i:i + 3]) for i in range(0, 30, 10))
        out.append((sig, joint))
    return out


def main():
    files = {}
    for p in glob.glob(GEX + '/C*'):
        d = open(p, 'rb').read()
        if d[:2] == b'\x11\x73':
            d = zlib.decompressobj(-15).decompress(d[5:])
        files[os.path.basename(p)] = d
    result, totals = {}, {}
    for name in sorted(os.listdir(GEDIR)):
        f = os.path.join(GEDIR, name, 'Model.c')
        if not os.path.exists(f):
            continue
        arrays = ge_arrays(f)
        totals[name] = len(arrays)
        per = collections.defaultdict(lambda: dict(arrays=0, joints=collections.Counter()))
        for sig, joint in arrays:
            for fn, d in files.items():
                i = d.find(sig[:6])
                while i >= 0:
                    if d[i + 12:i + 18] == sig[6:12] and d[i + 24:i + 30] == sig[12:18]:
                        per[fn]['arrays'] += 1
                        per[fn]['joints'][str(joint)] += 1
                        break
                    i = d.find(sig[:6], i + 1)
        result[name] = {fn: dict(arrays=v['arrays'], joints=dict(v['joints'])) for fn, v in per.items()}
    result['_arrays'] = totals
    json.dump(result, open(paths.data('gemap.json'), 'w'), indent=1)
    for name in sorted(totals):
        best = sorted(result[name].items(), key=lambda kv: -kv[1]['arrays'])[:4]
        print('%-18s %2d arrays: %s' % (name, totals[name], ', '.join('%s %d %s' % (fn, v['arrays'], v['joints']) for fn, v in best)))


if __name__ == '__main__':
    main()
