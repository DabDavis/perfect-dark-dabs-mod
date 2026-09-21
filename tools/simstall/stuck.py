import re,sys,math
rows=[l.split(" | ") for l in open(sys.argv[1])]
hist={}
for r in rows:
    f=int(r[0].split()[1])
    for c in r[1:]:
        m=re.match(r"(b\d+) act=(\d+) my=(\d+) pos=(-?\d+),(-?\d+),(-?\d+)",c)
        b,act,my,x,y,z=m.groups()
        hist.setdefault(b,[]).append((f,int(act),int(my),int(x),int(y),int(z)))
for b,h in hist.items():
    i=0
    while i<len(h):
        j=i
        while j+1<len(h) and h[j+1][1]==15 and h[i][1]==15 and math.dist(h[j+1][3:],h[i][3:])<40: j+=1
        if j-i>=2: print(b,"frames",h[i][0],"-",h[j][0],"at",h[i][3:],"my",h[i][2])
        i=j+1
