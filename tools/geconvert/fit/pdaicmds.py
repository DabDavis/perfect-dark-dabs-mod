import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths
C = paths.repo('src/game/chrai.c')

def table():
    txt = open(C).read()
    ptr = txt.split('g_CommandPointers[])(void) = {', 1)[1].split('\n};', 1)[0]
    names = re.findall(r'/\*(0x[0-9a-f]{4})\*/\s*(\w+),', ptr)
    ln = txt.split('u16 g_CommandLengths[] = {', 1)[1].split('\n};', 1)[0]
    lens = re.findall(r'/\*(0x[0-9a-f]{4})\*/\s*(\d+),', ln)
    lens = {int(a, 16): int(b) for a, b in lens}
    return [(int(a, 16), n, lens.get(int(a, 16))) for a, n in names]

if __name__ == '__main__':
    t = table()
    print(len(t))
    for op, n, l in t[:20]:
        print('%04x %-40s %s' % (op, n, l))
