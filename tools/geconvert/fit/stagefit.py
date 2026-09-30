import sys, os, struct, itertools
import numpy as np
from scipy.spatial import cKDTree
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import paths
import pdbg
from cafftool import Caff
BEAN=os.path.join(paths.BEAN,'files/%s/background/%s/default.bin')
def bean_points(look,name):
    c=Caff(BEAN%(look,name))
    get=lambda a,s:[c.blob(f) for f in c.files if f['asset']==a and c.sections[f['sect']-1]['name']==s]
    data=get(1,'.data')[0]; gpu=get(1,'.gpu')[0]; st=get(1,'.stream')[0]
    u=lambda b,o:struct.unpack_from('>I',b,o)[0]
    vbs=set(); pc=0x24
    while pc<len(st)-4:
        tag=u(st,pc); size=tag>>16; typ=(tag>>8)&0xff
        if size<4: break
        if typ==0x2e: vbs.add(u(st,pc+8))
        pc+=size
    pts=[]
    for vb in vbs:
        stride,obj,off,sz=struct.unpack_from('>4I',data,vb)
        n=sz//stride
        if n==0 or off+n*stride>len(gpu): continue
        arr=np.frombuffer(gpu[off:off+n*stride],dtype=np.uint8).reshape(n,stride)[:,:12].copy().view('>f4').reshape(n,3).astype(np.float64)
        ok=np.all(np.abs(arr)<1e6,axis=1)
        pts.append(arr[ok])
    return np.concatenate(pts)
def gex_points(bg):
    v,rooms=pdbg.room_vertices(os.path.join(paths.GEX_FILES,'bgdata/%s.seg'%bg))
    return np.array([p for x in v.values() for p in x],dtype=np.float64)
def register(B,G):
    tree=cKDTree(G)
    best=None
    rng=np.random.default_rng(1)
    sub=B[rng.choice(len(B),min(3000,len(B)),replace=False)]
    for perm in itertools.permutations(range(3)):
        for sg in itertools.product((1,-1),repeat=3):
            if np.linalg.det(np.diag(sg)[:,list(perm)])<0: continue  # rotations only
            P=sub[:,perm]*sg
            # scale from robust extents
            for scale in [None]:
                eb=np.percentile(P,98,axis=0)-np.percentile(P,2,axis=0)
                eg=np.percentile(G,98,axis=0)-np.percentile(G,2,axis=0)
                s=np.median(eg/np.maximum(eb,1e-6))
                cb=np.median(P,axis=0); cg=np.median(G,axis=0)
                Q=(P-cb)*s+cg
                # a few ICP steps (translation+scale)
                for it in range(15):
                    dd,ii=tree.query(Q)
                    keep=dd<np.percentile(dd,60)
                    A=P[keep]; T=G[ii[keep]]
                    ma=A.mean(0); mt=T.mean(0)
                    s=np.sum((A-ma)*(T-mt))/np.sum((A-ma)**2)
                    t=mt-s*ma
                    Q=P*s+t
                dd,ii=tree.query(Q)
                score=np.mean(dd<2.0)
                if best is None or score>best[0]: best=(score,perm,sg,s,t,np.median(dd))
    return best
if __name__=='__main__':
    for pair in sys.argv[1:]:
        bean,bg=pair.split(':')
        G=gex_points(bg)
        for look in ('original','new'):
            B=bean_points(look,bean)
            sc,perm,sg,s,t,md=register(B,G)
            print('%-10s %-6s %-8s bean %6d gex %6d  exact<2u %.2f median %.1f perm %s sign %s scale %.4f (1/%.3f) t %s'%(bean,bg,look,len(B),len(G),sc,md,perm,sg,s,1/s,np.round(t,1)))
