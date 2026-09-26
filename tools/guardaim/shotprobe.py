"""A player shot from gdb at a spot: X Y Z ROOMS TH VA place the player (vv_ground
follows Y - 159), CHR names a chr to report; prints every bgTestHitInRoom() /
bgTestHitInVtxBatch() hit, objTestHit() and chrTestHit() of shotCreate().
A gdb-called shot never lands on a chr; judge it by the background distance.
  SDL_VIDEODRIVER=offscreen gdb -batch -x shotprobe.py --args ./pd.x86_64 \
      --savedir save_hd --skip-intro --skip-mission-intro --no-sound --boot-stage 0x63 --fixed-step --rng-seed 1
"""
import gdb, os
gdb.execute('set pagination off'); gdb.execute('set confirm off'); gdb.execute('set unwind-on-signal on')
def ev(e): return gdb.parse_and_eval(e)
def f(e): return float(ev(e))
started = {'y': False}
def at_frame(fr):
    gdb.execute('break videoEndFrame if g_Vars.lvframenum >= %d' % fr)
    gdb.execute('run' if not started['y'] else 'continue'); started['y'] = True; gdb.execute('delete')
at_frame(300)
while int(ev('g_Vars.in_cutscene')) or int(ev('g_Vars.tickmode')) != 1:
    at_frame(int(ev('g_Vars.lvframenum')) + 60)
P = 'g_Vars.currentplayer->'
X, Y, Z = (float(os.environ[a]) for a in 'XYZ')
def place():
    for a, v in zip('xyz', (X, Y, Z)): gdb.execute('set variable %sprop->pos.%s = %f' % (P, a, v))
    rr = [int(r) for r in os.environ['ROOMS'].split(',')] + [-1]
    for k, r in enumerate(rr): gdb.execute('set variable %sprop->rooms[%d] = %d' % (P, k, r))
    gdb.execute('set variable %svv_theta = %f' % (P, float(os.environ['TH'])))
    gdb.execute('set variable %svv_verta = %f' % (P, float(os.environ['VA'])))
    gdb.execute('set variable %sinvincible = 1' % P)
    gdb.execute('set variable %svv_ground = %f' % (P, Y - 159))
    gdb.execute('set variable %svv_manground = %f' % (P, Y - 159))
fr = int(ev('g_Vars.lvframenum'))
for k in range(8):
    place(); fr += 5; at_frame(fr)
print('SP player %.0f %.0f %.0f theta %.1f verta %.1f' % (f(P+'prop->pos.x'), f(P+'prop->pos.y'), f(P+'prop->pos.z'), f(P+'vv_theta'), f(P+'vv_verta')))
gdb.execute('call (void)screenshotRequest()')
fr += 3; at_frame(fr)
class Fin(gdb.FinishBreakpoint):
    def __init__(self, room, ht):
        super().__init__(gdb.newest_frame(), internal=True); self.room = room; self.ht = ht
    def stop(self):
        r = int(self.return_value)
        if r:
            h = '((struct hitthing *)%d)' % self.ht
            print('SP bghit room %d ret %d at %.0f %.0f %.0f tex %d' % (self.room, r, f(h+'->pos.x'), f(h+'->pos.y'), f(h+'->pos.z'), int(ev(h+'->texturenum'))))
            rp = 'g_BgRooms[%d].pos' % self.room
            for v in ('unk18', 'unk1c', 'unk20'):
                print('SP   vtx %.0f %.0f %.0f' % (f(rp+'.x') + int(ev(h+'->'+v+'->x')), f(rp+'.y') + int(ev(h+'->'+v+'->y')), f(rp+'.z') + int(ev(h+'->'+v+'->z'))))
        return False
    def out_of_scope(self): pass
class BgHit(gdb.Breakpoint):
    def stop(self):
        Fin(int(ev('roomnum')), int(ev('(long)hitthing')))
        return False
class VB(gdb.Breakpoint):
    def stop(self):
        print('SP batch room %d type %d' % (int(ev('roomnum')), int(ev('batch->type'))))
        return False
class ObjHit(gdb.Breakpoint):
    def stop(self):
        print('SP objTestHit prop type %d model %d' % (int(ev('prop->type')), int(ev('prop->obj->modelnum'))))
        return False
class CFin(gdb.FinishBreakpoint):
    def __init__(self, sd):
        super().__init__(gdb.newest_frame(), internal=True); self.sd = sd
    def stop(self):
        print('SP   after chrTestHit hits[0].prop %s distance %.0f' % (ev('((struct shotdata *)%d)->hits[0].prop' % self.sd), f('((struct shotdata *)%d)->hits[0].distance' % self.sd)))
        return False
    def out_of_scope(self): pass
class ChrHit(gdb.Breakpoint):
    def stop(self):
        print('SP chrTestHit chrnum %d shotdist %.0f' % (int(ev('prop->chr->chrnum')), f('shotdata->distance')))
        CFin(int(ev('(long)shotdata')))
        return False
class VFin(gdb.FinishBreakpoint):
    def __init__(self, t, n, room):
        super().__init__(gdb.newest_frame(), internal=True); self.t = t; self.n = n; self.room = room
    def stop(self):
        if int(self.return_value):
            print('SP   batch hit room %d type %d batchidx %d' % (self.room, self.t, self.n))
        return False
    def out_of_scope(self): pass
class VBat(gdb.Breakpoint):
    def stop(self):
        room = int(ev('roomnum'))
        idx = int(ev('batch - g_Rooms[%d].vtxbatches' % room))
        VFin(int(ev('batch->type')), idx, room)
        return False
b0 = VBat('bgTestHitInVtxBatch', internal=True)
b1 = BgHit('bgTestHitInRoom', internal=True)
b2 = ObjHit('objTestHit', internal=True)
b3 = ChrHit('chrTestHit', internal=True)
for i in range(int(ev('g_NumChrSlots'))):
    if int(ev('g_ChrSlots[%d].prop' % i)) and int(ev('g_ChrSlots[%d].chrnum' % i)) == int(os.environ.get('CHR', '44')):
        print('SP chr %d at %.0f %.0f %.0f dmg %.2f' % (i, f('g_ChrSlots[%d].prop->pos.x' % i), f('g_ChrSlots[%d].prop->pos.y' % i), f('g_ChrSlots[%d].prop->pos.z' % i), f('g_ChrSlots[%d].damage' % i)))
        CI = i
print('SP aim ->', ev('propFindAimingAt(0, 0, 0)'))
print('SP shoot')
gdb.execute('call (void)shotCreate(0, 1, 0, 1, 0)')
print('SP rooms numbatches %d' % int(ev('g_Rooms[8].numvtxbatches')))
print('SP after dmg %.2f' % f('g_ChrSlots[%d].damage' % CI))
b0.delete(); b1.delete(); b2.delete(); b3.delete()
fr += 3; at_frame(fr)
gdb.execute('kill'); gdb.execute('quit')
