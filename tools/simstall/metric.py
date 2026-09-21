import re,sys,math
for fn in sys.argv[1:]:
    rows=[l.split(" | ") for l in open(fn)]
    hist={}
    for r in rows:
        f=int(r[0].split()[1])
        for c in r[1:]:
            m=re.match(r"(b\d+) act=(\d+) my=(\d+) pos=(-?\d+),(-?\d+),(-?\d+)",c)
            b,act,my,x,y,z=m.groups()
            hist.setdefault(b,[]).append((f,int(act),(int(x),int(y),int(z))))
    walk=stall=0
    for h in hist.values():
        for a,b in zip(h,h[1:]):
            if a[1]==15 and b[1]==15 and b[0]-a[0]>=45:
                walk+=1
                if math.dist(a[2],b[2])<15: stall+=1
    print("%-28s walking-intervals %4d stalled %4d = %.1f%%" % (fn.split('/')[-1],walk,stall,100.0*stall/max(walk,1)))
