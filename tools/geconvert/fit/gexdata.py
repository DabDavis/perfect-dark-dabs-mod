"""GE-X 6a's data segment tables, as the port's importer kept them
(build/mods/GE-X_6a_01-19-25/segs/data, base 0x80059fe0, modconfig.txt)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
MOD = paths.GEX_MOD
BASE = 0x80059fe0
data = open(MOD + '/segs/data', 'rb').read()
names = open(MOD + '/segs/data.names', 'rb').read().decode().split('\n')

def at(addr): return addr - BASE

def stages():
    out = {}
    for i in range(61):
        o = at(0x8007fcc0) + 0x38 * i
        sid, = struct.unpack_from('>h', data, o)
        bg, tiles, pads, setup, mpsetup = struct.unpack_from('>5H', data, o + 8)
        nm = lambda k: names[k] if k < len(names) else hex(k)
        out[sid] = dict(bg=nm(bg), tiles=nm(tiles), pads=nm(pads), setup=nm(setup), mpsetup=nm(mpsetup))
    return out

def solostages():
    return [struct.unpack_from('>I', data, at(0x80071e6c) + 12 * i)[0] for i in range(21)]

def modelstates():
    """[(file name, scale)]: struct modelstate is {modeldef pointer, u16 fileid, u16 scale}."""
    out = []
    for i in range(441):
        _, f, sc = struct.unpack_from('>IHH', data, at(0x8007b06c) + 8 * i)
        out.append((names[f] if f < len(names) else f, sc))
    return out

if __name__ == '__main__':
    st = stages()
    GE = ['Dam','Facility','Runway','Surface','Bunker','Silo','Frigate','Surface2','Bunker2','Statue','Archives','Streets','Depot','Train','Jungle','Temple','Caverns','Cradle','Aztec','Egypt','?']
    for n, sid in zip(GE, solostages()):
        print('%-9s 0x%02x' % (n, sid), st.get(sid))
