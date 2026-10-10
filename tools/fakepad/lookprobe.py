import gdb, os
GE = os.environ.get('PROBE_GE') == '1'
state = {'done': set(), 'mark': {}, 'seen': set()}
def ev(x): return gdb.parse_and_eval(x)
def sv(x): gdb.execute("set var " + x)
def once(tag):
    if tag in state['done']: return False
    state['done'].add(tag); return True
P = "g_Vars.players[0]"
LOOKS = {150: "watch UPRIGHT, P1 row untouched (own PD bit REVERSE)",
         190: "as P2 (mpindex 1): row REVERSE, watch UPRIGHT",
         230: "watch REVERSE, P1 row untouched",
         245: "as P2 (mpindex 1): row UPRIGHT, watch REVERSE"}
class Pitch(gdb.Breakpoint):
    def stop(self):
        f = int(ev("g_Vars.lvframenum"))
        if f in LOOKS and f not in state['seen']:
            state['seen'].add(f)
            gdb.write("LOOK frame %d gexplusmode %d mpindex %d invertpitch %d  <- %s\n" % (
                f, int(ev("g_GexPlusMode")), int(ev("g_Vars.currentplayerstats->mpindex")),
                int(ev("data->invertpitch")), LOOKS[f]))
        return False
class B(gdb.Breakpoint):
    def stop(self):
        f = int(ev("g_Vars.lvframenum"))
        if f >= 100 and once('a'):
            if GE: sv("g_GexPlusMode = 1")
            sv("g_PlayerConfigsArray[12].options = g_PlayerConfigsArray[12].options | 1")
            sv("g_PlayerConfigsArray[0].options = g_PlayerConfigsArray[0].options & ~1")
            sv("g_PlayerConfigsArray[1].options = g_PlayerConfigsArray[1].options | 1")
            sv("g_PlayerExtCfg[0].gemplook = 0")
            sv("g_PlayerExtCfg[1].gemplook = 2")
        if f >= 160 and once('p2a'): sv("g_Vars.playerstats[0].mpindex = 1")
        if f >= 200 and once('b'):
            sv("g_Vars.playerstats[0].mpindex = 0")
            sv("g_PlayerConfigsArray[12].options = g_PlayerConfigsArray[12].options & ~1")
        if f >= 235 and once('p2b'):
            sv("g_Vars.playerstats[0].mpindex = 1"); sv("g_PlayerExtCfg[1].gemplook = 1")
        if f >= 250 and once('p2c'): sv("g_Vars.playerstats[0].mpindex = 0")
        if f >= 300 and once('cm'):
            sv("g_PlayerConfigsArray[0].controlmode = 1"); sv("g_PlayerExtCfg[0].extcontrols = 0")
            gdb.write("pad attached %d\n" % int(ev("g_TestAttached")))
        if f >= 320 and once('rx'):
            sv("g_TestAxis[2] = 30000"); state['mark']['t0'] = float(ev(P+"->vv_theta"))
        if f >= 330 and once('rxs'):
            gdb.write("during RX+: c1 stick_x %d\n" % int(ev("g_Vars.players[0] ? 0 : 0")))
        if f >= 340 and once('rxend'):
            sv("g_TestAxis[2] = 0"); gdb.write("STICK RX+ 20 frames: theta %.2f -> %.2f (delta %+.2f)\n" % (state['mark']['t0'], float(ev(P+"->vv_theta")), float(ev(P+"->vv_theta")) - state['mark']['t0']))
        if f >= 360 and once('ry'):
            sv("g_TestAxis[3] = 30000"); state['mark']['v0'] = float(ev(P+"->vv_verta"))
        if f >= 375 and once('ryend'):
            sv("g_TestAxis[3] = 0"); gdb.write("STICK RY+ 15 frames: verta %.2f -> %.2f (delta %+.2f)\n" % (state['mark']['v0'], float(ev(P+"->vv_verta")), float(ev(P+"->vv_verta")) - state['mark']['v0']))
        if f >= 400:
            gdb.execute("kill")
        return False
B("lvTick")
Pitch("bondmove.c:715")
