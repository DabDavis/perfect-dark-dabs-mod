# Seeded simulant navigation probe, run by probe.sh. Every 30 level frames it
# records, per simulant: a stall (ST: in ACT_GOPOS and within 40 units of where
# it was 3 s ago), the waypoint it is running to (W), going over a floorless or
# bottomless spot (VOID), the nearest simulant to the player and how many have
# the player in sight (H), and every 1800 frames each chr's deaths, suicides and
# kills (K). A fall death - chrDie() in chr0f01f378() - prints FALL. With
# DETAIL=1 the first stall at each spot also prints the route state (DUMP).
set pagination off
set confirm off
handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint
break lvTick if g_Vars.lvframe60 % 30 == 0
python
import gdb, math, os
hist = {}
voided = {}
dumped = set()
DETAIL = int(os.environ.get("DETAIL", "0"))
def v(e): return gdb.parse_and_eval(e)
def xyz(c): return (float(c['x']), float(c['y']), float(c['z']))
def curwp(c):
    g = c['act_gopos']; w = g['waypoints'][int(g['curindex'])]
    return int(w['padnum']) if int(w) else -1
def dump(i, c, f):
    try:
        g = c['act_gopos']
        wps = []
        for k in range(4):
            w = g['waypoints'][k]
            if int(w) == 0: break
            wps.append(int(w['padnum']))
        print("DUMP f%d b%d pos=%s my=%d act=%d inv=%d lastok=%d lift=%d mode=%d cur=%d wps=%s end=%s aim=%s ground=%.0f" % (
            f, i, xyz(c['prop']['pos']), int(c['myaction']), int(c['actiontype']), int(c['invalidmove']), int(c['lastmoveok60']),
            int(c['liftaction']), int(g['waydata']['mode']), int(g['curindex']), wps, xyz(g['endpos']), xyz(g['waydata']['aimpos']), float(c['ground'])))
    except Exception as e: print("DUMPERR", e)
class Tick(gdb.Command):
    def __init__(s): super().__init__("tickprobe", gdb.COMMAND_USER)
    def invoke(s, arg, tty):
        f = int(v("g_Vars.lvframe60"))
        if not hist:
            # the player stands at the spawn and cannot die, so the match
            # runs its length with somebody to hunt (--spectate hides stalls)
            gdb.execute("set var g_Vars.players[0]->invincible=1")
            for k in range(8):
                if os.environ.get("DIFF"): gdb.execute("set var g_BotConfigsArray[%d].difficulty=%s" % (k, os.environ["DIFF"]))
                if os.environ.get("SPEED"): gdb.execute("set var g_BotConfigsArray[%d].stats[0]=%s" % (k, os.environ["SPEED"]))
        n = int(v("g_BotCount"))
        stalled = 0
        for i in range(n):
            c = v("g_MpBotChrPtrs[%d]" % i)
            if int(c) == 0: continue
            p = xyz(c['prop']['pos']); act = int(c['actiontype'])
            h = hist.setdefault(i, [])
            h.append((f, act, p))
            if len(h) > 12: h.pop(0)
            if act == 15:
                try:
                    ap = c['act_gopos']['waydata']['aimpos']
                    print("W f%d b%d wp=%d d=%.0f p=%.0f,%.0f,%.0f" % (f, i, curwp(c), math.dist((p[0], p[2]), (float(ap['x']), float(ap['z']))), p[0], p[1], p[2]))
                except Exception: pass
            gnd = float(c['ground'])
            if gnd < -5000 and not voided.get(i) and f > 0:
                voided[i] = True; print("VOID f%d b%d pos=(%.0f %.0f %.0f)" % (f, i, p[0], p[1], p[2]))
            elif gnd > -5000: voided[i] = False
            if len(h) >= 6 and all(x[1] == 15 for x in h[-6:]) and math.dist(h[-6][2], p) < 40:
                stalled += 1
                print("ST f%d b%d %.0f %.0f %.0f g=%.0f wp=%d inv=%d" % (f, i, p[0], p[1], p[2], gnd, curwp(c), int(c['invalidmove'])))
                key = (i, round(p[0]/100), round(p[2]/100))
                if DETAIL and key not in dumped:
                    dumped.add(key); dump(i, c, f)
        try:
            pp = v("g_Vars.players[0]->prop")
            ppos = xyz(pp['pos'])
            pidx = (int(pp) - int(v("g_Vars.props"))) // int(v("sizeof(struct prop)"))
            dmin = 1e9; tgt = 0; sight = 0
            for i in range(n):
                c = v("g_MpBotChrPtrs[%d]" % i)
                if int(c) == 0: continue
                dmin = min(dmin, math.dist(xyz(c['prop']['pos']), ppos))
                if int(c['target']) == pidx:
                    tgt += 1
                    sight += int(c['aibot']['targetinsight']) != 0
            print("H f%d dmin=%.0f tgt=%d sight=%d" % (f, dmin, tgt, sight))
        except Exception as e: print("H err", e)
        print("T f%d stalled=%d" % (f, stalled))
        if f % 1800 == 0:
            nc = int(v("g_MpNumChrs")); ks = []
            for k in range(nc):
                cf = v("g_MpAllChrConfigPtrs[%d]" % k)
                ks.append("%d:%d/%d/%d" % (k, int(cf['numdeaths']), int(cf['killcounts'][k]), sum(int(cf['killcounts'][j]) for j in range(nc) if j != k)))
            print("K f%d %s" % (f, " ".join(ks)))
Tick()
class Fall(gdb.Breakpoint):
    def stop(self):
        c = gdb.parse_and_eval("chr"); p = c['prop']['pos']
        print("FALL f%d at (%.0f %.0f %.0f)" % (int(gdb.parse_and_eval("g_Vars.lvframe60")), float(p['x']), float(p['y']), float(p['z'])))
        return False
if os.environ.get("FALLLINE"): Fall("chr.c:%s" % os.environ["FALLLINE"])
end
commands 1
silent
tickprobe
continue
end
run
