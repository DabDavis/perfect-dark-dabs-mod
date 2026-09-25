"""Guards shooting at the player: one guard put D units in front of the player,
made to attack (standing or kneeling) for FRAMES frames while the player
stands or crouches, counting every shot that reached chrCalculateHit(), the
shotbondsum it added and every hit (a reset of the sum). Screenshots of the
aim pose.

env: TAG, D (distance), MODE stand|kneel, CROUCH 0|1, DIFF (g_Difficulty),
     FRAMES, START, SHOTS (screenshots), X Y Z TH (player spot, optional),
     TURN (degrees off the view to put the guard), MOVEPLAYER=1 (the player
     goes D from the nearest guard instead), WEAPON (a comma list the guard
     must hold), GIVE (swap his gun's number), INV=0 (take damage, health
     refilled each tick), LOSDBG=1 (what blocks the line at the end)

Run from a directory with the binary, data/, added-content/ and mods/:
  SDL_VIDEODRIVER=offscreen gdb -batch -x aimprobe.py --args ./pd.x86_64 \
      --savedir save_x --skip-intro --skip-mission-intro --no-sound \
      --boot-stage 0x63 --fixed-step --rng-seed 1
"""
import gdb, os, math
gdb.execute('set pagination off'); gdb.execute('set confirm off'); gdb.execute('set unwind-on-signal on')
def ev(e): return gdb.parse_and_eval(e)
def f(e): return float(ev(e))
TAG = os.environ.get('TAG', 'x')
D = float(os.environ.get('D', '300'))
MODE = os.environ.get('MODE', 'stand')
CROUCH = int(os.environ.get('CROUCH', '0'))
DIFF = int(os.environ.get('DIFF', '0'))
FRAMES = int(os.environ.get('FRAMES', '1200'))
NSHOTS = int(os.environ.get('SHOTS', '3'))
WANT = [int(x, 0) for x in os.environ['WEAPON'].split(',')] if os.environ.get('WEAPON') else None
START = int(os.environ.get('START', '300'))
started = {'y': False}
def at_frame(fr):
    gdb.execute('break videoEndFrame if g_Vars.lvframenum >= %d' % fr)
    gdb.execute('run' if not started['y'] else 'continue'); started['y'] = True; gdb.execute('delete')

SHOTS = os.path.join(os.path.dirname(os.path.abspath('pd.x86_64')), 'screenshots')
OUT = os.environ.get('OUT', 'aimshots')
os.makedirs(OUT, exist_ok=True)
def shot(what):
    before = set(os.listdir(SHOTS)) if os.path.isdir(SHOTS) else set()
    gdb.execute('call (void)screenshotRequest()')
    fr = int(ev('g_Vars.lvframenum'))
    at_frame(fr + 2)
    new = sorted((set(os.listdir(SHOTS)) if os.path.isdir(SHOTS) else set()) - before)
    if new:
        os.rename(os.path.join(SHOTS, new[-1]), '%s/%s_%s.png' % (OUT, TAG, what))

stats = {'calls': 0, 'ok': 0, 'hits': 0, 'sum': 0.0, 'resets': 0, 'dmg': 0.0, 'dmgevents': 0}
class CalcFinish(gdb.FinishBreakpoint):
    def __init__(self, chr, before):
        super().__init__(gdb.newest_frame(), internal=True)
        self.chr = chr; self.before = before
    def stop(self):
        after = float(ev('((struct chrdata *)%d)->shotbondsum' % self.chr))
        if after < self.before or after == 0.0 and self.before == 0.0 and self.hitflag():
            stats['ok'] += 1; stats['resets'] += 1
            stats['sum'] += 1.0 - self.before
        elif after > self.before:
            stats['ok'] += 1
            stats['sum'] += after - self.before
        return False
    def hitflag(self):
        return False
    def out_of_scope(self):
        pass
class Calc(gdb.Breakpoint):
    def stop(self):
        chr = int(ev('chr'))
        if chr == GUARD:
            stats['calls'] += 1
            CalcFinish(chr, float(ev('chr->shotbondsum')))
        return False
at_frame(START)
while int(ev('g_Vars.in_cutscene')) or int(ev('g_Vars.tickmode')) != 1:
    at_frame(int(ev('g_Vars.lvframenum')) + 60)
START = int(ev('g_Vars.lvframenum'))
P = 'g_Vars.currentplayer->'
gdb.execute('set variable g_Difficulty = %d' % DIFF)
if os.environ.get('X'):
    X, Y, Z = (float(os.environ[a]) for a in 'XYZ')
    for a, v in zip('xyz', (X, Y, Z)): gdb.execute('set variable %sprop->pos.%s = %f' % (P, a, v))
    gdb.execute('set variable %svv_theta = %f' % (P, float(os.environ.get('TH', '0'))))
    at_frame(START + 20)
th = math.radians(f('%svv_theta' % P))
px, py, pz = (f('%sprop->pos.%s' % (P, a)) for a in 'xyz')
room = int(ev('%sprop->rooms[0]' % P))
PLAYERCHR = int(ev('(long)%sprop->chr' % P))

# a live guard holding a gun
n = int(ev('g_NumChrSlots')); c = None
for i in range(n):
    ch = ev('&g_ChrSlots[%d]' % i)
    if int(ch['model']) and int(ch['prop']) and int(ch['prop']['type']) == 3 and int(ch['chrnum']) >= 0 \
            and int(ch['actiontype']) not in (4, 5) and int(ev('(long)chrGetHeldProp(&g_ChrSlots[%d], 0)' % i)):
        w = int(ev('chrGetHeldProp(&g_ChrSlots[%d], 0)->weapon->weaponnum' % i))
        if WANT is None or w in WANT:
            if os.environ.get('MOVEPLAYER'):
                d2 = (f('g_ChrSlots[%d].prop->pos.x' % i) - px) ** 2 + (f('g_ChrSlots[%d].prop->pos.z' % i) - pz) ** 2
                if c is None or d2 < best: c, best = i, d2
            else:
                c = i; break
C = 'g_ChrSlots[%d]' % c
GUARD = int(ev('(long)&%s' % C)); GUARDPROP = int(ev('(long)%s.prop' % C))
gy = f('%svv_manground' % P)
gdb.execute('set variable %s.ailist = 0' % C)
gdb.execute('set variable $pos = (struct coord *)malloc(12)')
gdb.execute('set variable $rooms = (RoomNum *)malloc(32)')
if os.environ.get('MOVEPLAYER'):
    # the guard stays where the level put him; the player goes D from him
    cx, cz = f('%s.prop->pos.x' % C), f('%s.prop->pos.z' % C)
    dx, dz = px - cx, pz - cz
    l = math.hypot(dx, dz) or 1.0
    px, pz = cx + dx / l * D, cz + dz / l * D
    for a, v in (('x', px), ('z', pz)): gdb.execute('set variable %sprop->pos.%s = %f' % (P, a, v))
    th = math.atan2(-(cx - px), cz - pz)
    gdb.execute('set variable %svv_theta = %f' % (P, math.degrees(th) % 360))
    at_frame(int(ev('g_Vars.lvframenum')) + 20)
    px, pz = f('%sprop->pos.x' % P), f('%sprop->pos.z' % P)
    gx, gz = cx, cz
    ang = math.atan2(px - gx, pz - gz) % (2 * math.pi)
    gdb.execute('call (void)chrStand(&%s)' % C)
else:
  for turn in (int(os.environ.get('TURN', '0')),):
    th2 = th + math.radians(turn)
    gx, gz = px - math.sin(th2) * D, pz + math.cos(th2) * D
    for a, v in zip('xyz', (gx, gy, gz)): gdb.execute('set variable $pos->%s = %f' % (a, v))
    gdb.execute('set variable $rooms[0] = %d' % room); gdb.execute('set variable $rooms[1] = -1')
    ang = math.atan2(px - gx, pz - gz)
    if ang < 0: ang += 2 * math.pi
    moved = int(ev('(int)chrMoveToPos(&%s, $pos, $rooms, %f, 1)' % (C, ang)))
    th = th2
print('AIM %s guard slot %d chrnum %d weapon %#x acc %d body %d at %.0f %.0f %.0f room %d player %.0f %.0f %.0f' % (
    TAG, c, int(ev('%s.chrnum' % C)), int(ev('chrGetHeldProp(&%s, 0)->weapon->weaponnum' % C)),
    int(ev('%s.accuracyrating' % C)), int(ev('%s.bodynum' % C)), gx, gy, gz, room, px, py, pz))
# everyone else stands still, out of the way
for i in range(n):
    if i != c and int(ev('g_ChrSlots[%d].model' % i)) and int(ev('g_ChrSlots[%d].prop' % i)) and int(ev('g_ChrSlots[%d].prop->type' % i)) == 3 \
            and int(ev('g_ChrSlots[%d].actiontype' % i)) not in (4, 5):
        gdb.execute('set variable g_ChrSlots[%d].ailist = 0' % i)
        gdb.execute('call (void)chrStand(&g_ChrSlots[%d])' % i)
        gdb.execute('set variable g_ChrSlots[%d].chrflags = g_ChrSlots[%d].chrflags | 0x00000400' % (i, i))
if os.environ.get('INV', '1') == '1':
    gdb.execute('set variable %sinvincible = 1' % P)
gdb.execute('call (void)chrStand(&%s)' % C)
gdb.execute('set variable %s.target = -1' % C)
GIVE = os.environ.get('GIVE')
if GIVE:
    gdb.execute('set variable chrGetHeldProp(&%s, 0)->weapon->weaponnum = %d' % (C, int(GIVE, 0)))
    gdb.execute('set variable chrGetHeldProp(&%s, 0)->weapon->gset.weaponnum = %d' % (C, int(GIVE, 0)))
    ws = []
    for h in (0, 1):
        if int(ev('(long)chrGetHeldProp(&%s, %d)' % (C, h))):
            ws.append('%#x' % int(ev('chrGetHeldProp(&%s, %d)->weapon->weaponnum' % (C, h))))
    print('AIM %s gave %s: holds %s' % (TAG, GIVE, ws))
if CROUCH:
    gdb.execute('set variable %sautocrouchpos = 0' % P)
at_frame(START + 40)
gdb.execute('set variable %svv_verta = %f' % (P, float(os.environ.get('VA', '-6'))))

Calc('chrCalculateHit', internal=True)
begin = int(ev('g_Vars.lvframenum'))
shots_at = [begin + (k + 1) * FRAMES // (NSHOTS + 1) for k in range(NSHOTS)]
pitches = []
fr = begin
while fr < begin + FRAMES:
    if int(ev('%s.actiontype' % C)) != 8:
        gdb.execute('call (void)%s(&%s, 0x200, 0)' % ('chrAttackKneel' if MODE == 'kneel' else 'chrAttackStand', C))
    if CROUCH:
        gdb.execute('set variable %sautocrouchpos = 0' % P)
    gdb.execute('set variable %svv_theta = %f' % (P, math.degrees(th)))
    gdb.execute('set variable %sbondhealth = 1.0' % P)
    for a, v in (('x', px), ('z', pz)):
        gdb.execute('set variable %sprop->pos.%s = %f' % (P, a, v))
        gdb.execute('set variable %sbondshotspeed.%s = 0' % (P, a))
    fr += 1
    at_frame(fr)
    h = f('%sbondhealth' % P)
    if h < 1.0:
        stats['dmg'] += 1.0 - h; stats['dmgevents'] += 1
    if int(ev('%sisdead' % P)):
        print('AIM %s player died' % TAG); break
    if int(ev('%s.actiontype' % C)) == 8:
        pitches.append((f('%s.aimuprshoulder' % C), f('%s.aimupback' % C)))
    if shots_at and fr >= shots_at[0]:
        shots_at.pop(0)
        print('AIM %s pose frame %d anim %d frame %.1f aimuprshoulder %.3f aimupback %.3f eye %.1f' % (
            TAG, fr, int(ev('%s.model->anim->animnum' % C)), f('%s.model->anim->frame' % C),
            f('%s.aimuprshoulder' % C), f('%s.aimupback' % C), f('%svv_eyeheight' % P)))
        shot('%s%d' % (MODE, fr))
        fr = int(ev('g_Vars.lvframenum'))
elapsed = int(ev('g_Vars.lvframenum')) - begin
print('AIM %s end player pos %.0f %.0f %.0f rooms %d %d guard pos %.0f %.0f %.0f rooms %d %d los %d bondvisible %d' % (TAG, f('%sprop->pos.x' % P), f('%sprop->pos.y' % P), f('%sprop->pos.z' % P),
    int(ev('%sprop->rooms[0]' % P)), int(ev('%sprop->rooms[1]' % P)), f('%s.prop->pos.x' % C), f('%s.prop->pos.y' % C), f('%s.prop->pos.z' % C),
    int(ev('%s.prop->rooms[0]' % C)), int(ev('%s.prop->rooms[1]' % C)), int(ev('(int)chrHasLosToTarget(&%s)' % C)), int(ev('g_Vars.bondvisible'))))
avg = [sum(p[k] for p in pitches) / max(1, len(pitches)) for k in range(2)]
if os.environ.get('LOSDBG'):
    gdb.execute('set variable $a = (struct coord *)malloc(12)'); gdb.execute('set variable $b = (struct coord *)malloc(12)')
    gdb.execute('set variable $a->x = %s.prop->pos.x' % C); gdb.execute('set variable $a->z = %s.prop->pos.z' % C); gdb.execute('set variable $a->y = %s.prop->pos.y' % C)
    gdb.execute('set variable $b->x = %sprop->pos.x' % P); gdb.execute('set variable $b->z = %sprop->pos.z' % P); gdb.execute('set variable $b->y = %sprop->pos.y' % P)
    for t in (0x20, 0x01, 0x02, 0x04, 0x08, 0x3f):
        r = int(ev('(int)cdTestLos05($a, %s.prop->rooms, $b, %sprop->rooms, %d, 0x10)' % (C, P, t)))
        extra = ''
        if not r:
            gdb.execute('set variable $c = (struct coord *)malloc(12)')
            gdb.execute('call (void)cdGetPos($c, 0, "x")')
            extra = 'at %.0f %.0f %.0f obstacle %s' % (f('$c->x'), f('$c->y'), f('$c->z'), ev('cdGetObstacleProp()'))
        print('AIM LOS types %#x -> %d %s' % (t, r, extra))
print('AIM %s RESULT diff %d mode %s crouch %d dist %.0f frames %d calls %d added %d sum %.3f hits %d damage %.3f in %d  hits/min %.1f  mean aimuprshoulder %.3f aimupback %.3f' % (
    TAG, DIFF, MODE, CROUCH, D, elapsed, stats['calls'], stats['ok'], stats['sum'], stats['resets'], stats['dmg'], stats['dmgevents'],
    stats['resets'] * 3600.0 / max(1, elapsed), avg[0], avg[1]))
gdb.execute('kill'); gdb.execute('quit')
