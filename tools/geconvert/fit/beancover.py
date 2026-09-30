import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
import numpy as np
from scipy.spatial import cKDTree
import gefiles
from stagefit import bean_points
rng=np.random.default_rng(0)
import json,os
OUT=paths.data('beanscales.json')
out=json.load(open(OUT)) if os.path.exists(OUT) else {}
for pair in sys.argv[1:]:
    bean,ge=pair.split(':')
    G=np.array([v[:3] for v in gefiles.Bg(gefiles.rom_file(gefiles.LEVELS[ge][0])).world_vertices()],float)
    B=bean_points('original',bean); B=B[np.all(np.abs(B)<1e6,axis=1)]
    tb=cKDTree(G)
    sub=B[rng.choice(len(B),min(600,len(B)),replace=False)]
    best=(0,0)
    for s in np.geomspace(0.0005,20,30000):
        sc=np.mean(tb.query(sub*s,distance_upper_bound=2)[0]<1.5)
        if sc>best[0]: best=(sc,s)
    s=best[1]
    # refine
    for rad in (4,2,1.5):
        d,i=tb.query(B*s); k=d<rad
        if k.sum()<10: break
        s=float(np.sum(B[k]*G[i[k]])/np.sum(B[k]*B[k]))
    N=bean_points('new',bean); tn=cKDTree(N*s)
    dG,_=tn.query(G)
    out[ge]=dict(bean=bean,scale=s,cover=float(np.mean(dG<1.5)))
    print('%-12s %-5s scale %.9f  bean-orig pts on GE %.2f   GE vertices covered by bean HD %.2f' % (bean,ge,s,np.mean(tb.query(B*s)[0]<1.5),np.mean(dG<1.5)),flush=True)

json.dump(out,open(OUT,'w'),indent=1)
