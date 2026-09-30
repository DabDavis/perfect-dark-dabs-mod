import sys, os, glob, json
import numpy as np
from scipy.spatial import cKDTree
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import paths
from stagefit import bean_points, gex_points
BEANS=[b for b in sorted(os.listdir(os.path.join(paths.BEAN,'files/new/background'))) if not b.endswith('_hits')]
bgs=sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(paths.GEX_FILES,'bgdata/*.seg')))
rng=np.random.default_rng(1)
G={}
for bg in bgs:
    try:
        g=gex_points(bg)
        if len(g)>50: G[bg]=(g,cKDTree(g))
    except Exception as e: pass
print(len(G),'gex bgs',flush=True)
def fit(B,g,tree):
    sub=B[rng.choice(len(B),min(800,len(B)),replace=False)]
    eb=np.percentile(B,99,0)-np.percentile(B,1,0); eg=np.percentile(g,99,0)-np.percentile(g,1,0)
    best=(0,0,0)
    for s in [float(np.median(eg/np.maximum(eb,1e-3)))]+list(eg/np.maximum(eb,1e-3)):
        if not (0.01<s<10): continue
        for t in (np.zeros(3), np.median(g,0)-s*np.median(B,0)):
            for it in range(6):
                dd,ii=tree.query(sub*s+t)
                keep=dd<=np.percentile(dd,50)
                A=sub[keep]; T=g[ii[keep]]; ma=A.mean(0); mt=T.mean(0)
                den=np.sum((A-ma)**2)
                if den<=0: break
                s=max(0.01,np.sum((A-ma)*(T-mt))/den); t=mt-s*ma
            dd,_=tree.query(sub*s+t)
            sc=float(np.mean(dd<2.0))
            if sc>best[0]: best=(sc,s,t)
    return best
res=[]
for look in ('original','new'):
    for bean in BEANS:
        B=bean_points(look,bean)
        top=[]
        for bg,(g,tree) in G.items():
            sc,s,t=fit(B,g,tree)
            top.append((sc,bg,s,t))
        top.sort(key=lambda x:-x[0])
        for sc,bg,s,t in top[:3]:
            if sc>0.2:
                res.append(dict(look=look,bean=bean,bg=bg,score=sc,scale=s,t=[float(x) for x in t]))
        print(look,bean,' '.join('%s:%.2f@%.4f'%(bg,sc,s) for sc,bg,s,t in top[:3]),flush=True)
json.dump(res,open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'stagefit_all.json'),'w'),indent=1)
