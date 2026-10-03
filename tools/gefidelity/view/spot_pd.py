"""Our side of spot3_ares.py: SPOT="x,y,z,theta,verta[,room];..." in OUR world
(y the feet), AI frozen, a screenshot per spot (pd.log names the files)."""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
import gdbpd as lib
import gdb
lib.boot(None, int(os.environ.get('GF_DIFF', '0')))
try:
    gdb.execute('set variable g_Vars.currentplayer->bondmovemode = 0')
except gdb.error:
    pass
for k in range(int(lib.ev('g_NumChrSlots'))):
    gdb.execute('set variable g_ChrSlots[%d].ailist = 0' % k)
spots = [tuple(float(v) for v in s.split(',')) for s in os.environ['SPOT'].split(';')]
for i, s in enumerate(spots):
    x, y, z, th, va = s[:5]
    room = int(s[5]) if len(s) > 5 else None
    lib.hold(x, y, z, th, va, n=int(os.environ.get('GF_HOLD', '20')), room=room)
    lib.say('spot', i, 'cam', lib.ev('g_Vars.currentplayer->cam_pos'), 'theta', lib.ev('g_Vars.currentplayer->vv_theta'))
    if os.environ.get('GF_ROOMLIST'):
        n = int(lib.ev('g_Vars.roomcount'))
        on = [r for r in range(1, n) if int(lib.ev('g_Rooms[%d].flags' % r)) & 4]
        lib.say('spot', i, 'rooms', lib.ev('g_Vars.currentplayer->prop->rooms'), 'onscreen', on)
    lib.shot()
lib.finish()
