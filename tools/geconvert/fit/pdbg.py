import sys, struct, zlib
def inflate(b):
    h=int.from_bytes(b[0:2],'big')
    if h==0x1173: return zlib.decompressobj(-15).decompress(b[5:])
    if h==0x1172: return zlib.decompressobj(-15).decompress(b[2:])
    raise ValueError(hex(h))
def load(path):
    d=open(path,'rb').read()
    infl,s1c,primc=struct.unpack_from('>III',d,0)
    prim=inflate(d[0xc:0xc+primc])
    u=lambda o:struct.unpack_from('>I',prim,o)[0]
    rooms_at=u(4)-0x0f000000
    rooms=[]
    j=1
    while True:
        o=rooms_at+20*j
        p=u(o)
        if p==0: break
        pos=struct.unpack_from('>3f',prim,o+4)
        rooms.append((j,p,pos))
        j+=1
    return d,prim,rooms,s1c
if __name__=='__main__':
    d,prim,rooms,s1c=load(sys.argv[1])
    print(len(rooms),'rooms')
    xs=[r[2] for r in rooms]
    print('pos min',[min(p[k] for p in xs) for k in range(3)],'max',[max(p[k] for p in xs) for k in range(3)])
    for r in rooms[:12]: print(r[0],hex(r[1]),[round(v) for v in r[2]])

def room_vertices(path):
    d,prim,rooms,s1c=load(path)
    infl,_,primc=struct.unpack_from('>III',d,0)
    delta=infl-primc-0xc
    out={}
    for i,(j,p,pos) in enumerate(rooms):
        nxt=rooms[i+1][1] if i+1<len(rooms) else None
        if nxt is None:
            # last room: read to the end marker in the bgroom table
            o=struct.unpack_from('>I',prim,4)[0]-0x0f000000
            nxt=struct.unpack_from('>I',prim,o+20*(j+1))[0]
        foff=p-0x0f000000-delta
        blob=d[foff:foff+(nxt-p)]
        try:
            r=inflate(blob)
        except Exception as e:
            continue
        vptr,cptr=struct.unpack_from('>II',r,0)
        nv=(cptr-vptr)//12 if cptr>vptr else 0
        vo=vptr-p
        vs=[]
        for k in range(nv):
            x,y,z=struct.unpack_from('>hhh',r,vo+12*k)
            vs.append((pos[0]+x,pos[1]+y,pos[2]+z))
        out[j]=vs
    return out,rooms
