"""GoldenEye's own guard hit rate: a Facility guard stood D units in front of
Bond and made to attack him (chrAttackStand/Kneel's GoldenEye originals) for
TICKS sixtieths, every other character stilled. Counts chrlvUpdateShotbondsum
calls, the shotbondsum each added and the hits. DIFF sets g_SelectedDifficulty
(0-3; the pad script enters on 00 Agent, and the modifiers are rewritten
every frame from it). Run under PORT_LOCKSTEP=1 PORT_VI_LOCKSTEP=1 from
~/claude-007/007 on 10.8.0.3:
  PORT_PAD_SCRIPT=$HOME/dam-oracle/dam.padscript PORT_BOOT_FRAMES=1000000 \
      gdb -batch -x gehitrate.py --args ./build/port/ge007 --boot"""
import gdb, os, math
gdb.execute("set pagination off"); gdb.execute("set confirm off")
D = float(os.environ.get("D", "300")); MODE = os.environ.get("MODE", "stand")
TICKS = int(os.environ.get("TICKS", "3600"))
def ev(e): return gdb.parse_and_eval(e)
def until(counter):
    gdb.execute("break lvlRender if currentFrameCounter >= %d" % counter)
    gdb.execute("continue"); gdb.execute("delete")
def now(): return int(ev("currentFrameCounter"))
gdb.execute("break bossSetLoadedStage if stage == LEVELID_DAM")
gdb.execute("run")
gdb.execute("set variable stage = LEVELID_FACILITY"); gdb.execute("delete")
until(1900)
X, Y, Z, theta, verta = 4766.3 + 4509, 239.0 - 106, 796.0 - 1063, 158.0, -8.0
stan = int(ev("(unsigned long)stanFindFloorTileBelowY(%f, %f, %f, 0.0f)" % (X, Y + 100, Z)))
def place():
    for a, v in (("x", X), ("z", Z)):
        gdb.execute("set variable g_CurrentPlayer->prop->pos.%s = %f" % (a, v))
        gdb.execute("set variable g_CurrentPlayer->field_488.collision_position.%s = %f" % (a, v))
    for f in ("prop->stan", "field_488.current_tile_ptr", "field_488.current_tile_ptr_for_portals"):
        gdb.execute("set variable g_CurrentPlayer->%s = (StandTile *)%d" % (f, stan))
    gdb.execute("set variable g_CurrentPlayer->vv_theta = %f" % theta)
    gdb.execute("set variable g_CurrentPlayer->vv_verta = %f" % verta)
n = int(ev("g_NumChrSlots"))
live = [i for i in range(n) if int(ev("(long)g_ChrSlots[%d].model" % i)) and int(ev("(long)g_ChrSlots[%d].prop" % i))]
c = live[0]; C = "g_ChrSlots[%d]" % c
for i in live:
    gdb.execute("set variable g_ChrSlots[%d].ailist = 0" % i)
def still():
    for i in live:
        if i != c:
            gdb.execute("set variable g_ChrSlots[%d].actiontype = ACT_NULL" % i)
            gdb.execute("call (void)chrStopFiring(&g_ChrSlots[%d])" % i)
for _ in range(20):
    place(); still(); until(now() + 3)
th = math.radians(theta)
GX, GZ = X - math.sin(th) * D, Z + math.cos(th) * D
gstan = int(ev("(unsigned long)stanFindFloorTileBelowY(%f, %f, %f, 0.0f)" % (GX, Y + 100, GZ)))
gy = float(ev("stanGetPositionYValue((StandTile *)%d, %f, %f)" % (gstan, GX, GZ)))
face0 = math.atan2(X - GX, Z - GZ) % (2 * math.pi)
gdb.execute("call (void)sub_GAME_7F03D058(%s.prop, 0)" % C)
gdb.execute("set variable %s.prop->pos.x = %f" % (C, GX)); gdb.execute("set variable %s.prop->pos.y = %f" % (C, gy)); gdb.execute("set variable %s.prop->pos.z = %f" % (C, GZ))
gdb.execute("set variable %s.prop->stan = (StandTile *)%d" % (C, gstan))
gdb.execute("set variable %s.chrflags = %s.chrflags | CHRFLAG_INIT" % (C, C))
gdb.execute("call (void)setsubroty(%s.model, %f)" % (C, face0))
gdb.execute("call (void)setsuboffset(%s.model, &%s.prop->pos)" % (C, C))
gdb.execute("call (void)chrDetectRooms(&%s)" % C)
gdb.execute("call (void)sub_GAME_7F03D058(%s.prop, 1)" % C)
DIFF = os.environ.get("DIFF")
print("GEHIT difficulty was %s" % ev("g_SelectedDifficulty"))
if DIFF:
    gdb.execute("set variable g_SelectedDifficulty = %s" % DIFF)
    until(now() + 2)
GUARD = int(ev("(long)&%s" % C))
stats = {"calls": 0, "ok": 0, "sum": 0.0, "resets": 0, "dmg": 0}
class Fin(gdb.FinishBreakpoint):
    def __init__(self, before):
        super().__init__(gdb.newest_frame(), internal=True); self.before = before
    def stop(self):
        after = float(ev("((ChrRecord *)%d)->shotbondsum" % GUARD))
        if after < self.before:
            stats["ok"] += 1; stats["resets"] += 1; stats["sum"] += 1.0 - self.before
        elif after > self.before:
            stats["ok"] += 1; stats["sum"] += after - self.before
        return False
    def out_of_scope(self): pass
class Calc(gdb.Breakpoint):
    def stop(self):
        if int(ev("(long)self")) == GUARD:
            stats["calls"] += 1; Fin(float(ev("self->shotbondsum")))
        return False
class Dmg(gdb.Breakpoint):
    def stop(self):
        stats["dmg"] += 1
        return False
class Fire(gdb.Breakpoint):
    def stop(self):
        if int(ev("(long)self")) == GUARD:
            cyc["fired"] = True
        return False
Fire("chrlvFireWeaponRelated", internal=True)
Calc("chrlvUpdateShotbondsum", internal=True); Dmg("bondviewCallRecordDamageKills", internal=True)
t0 = int(ev("g_GlobalTimer")); frames = 0; deltas = 0.0
TRACE = int(os.environ.get("TRACE", "1"))
cyc = {"attacks": 0, "firing": 0, "inattack": False, "fired": False, "start": 0, "startfire": 0, "anim": 0}
print("GEHIT speedrating %d" % int(ev("%s.speedrating" % C)))
print("GEHIT weapon %s / %s" % (ev("((struct WeaponObjRecord *)chrGetEquippedWeaponProp(&%s, 0)->obj)->weaponnum" % C), ev("(long)chrGetEquippedWeaponProp(&%s, 1)" % C)))
print("GEHIT guard slot %d at %.0f %.0f %.0f acc %s rating %d" % (c, GX, gy, GZ, ev("g_AiAccuracyModifier"), int(ev("%s.accuracyrating" % C))))
while int(ev("g_GlobalTimer")) - t0 < TICKS:
    place(); still()
    gdb.execute("set variable g_CurrentPlayer->bondhealth = 1.0")
    if str(ev("%s.actiontype" % C)) != "ACT_ATTACK":
        gdb.execute("call (void)%s(&%s, 1, 0)" % ("sub_GAME_7F0256F0" if MODE == "kneel" else "sub_GAME_7F025560", C))
    t1 = int(ev("g_GlobalTimer"))
    until(now() + 1); frames += 1; deltas += float(ev("g_GlobalTimerDelta"))
    dt = int(ev("g_GlobalTimer")) - t1
    act = str(ev("%s.actiontype" % C)) == "ACT_ATTACK"
    if act and not cyc["inattack"]:
        cyc["attacks"] += 1; cyc["start"] = int(ev("g_GlobalTimer")) - dt; cyc["startfire"] = cyc["firing"]
    if cyc["fired"]:
        cyc["firing"] += dt
    cyc["fired"] = False
    if act:
        cyc["anim"] = int(ev("(long)%s.act_attack.animfloats->anim.anim" % C))
    if not act and cyc["inattack"]:
        print("GEHITA anim %#x ticks %d firing %d" % (cyc["anim"], int(ev("g_GlobalTimer")) - cyc["start"], cyc["firing"] - cyc["startfire"]))
    cyc["inattack"] = act
    if TRACE > 1:
        print("GEHITT t %d act %s frame %.1f speed %.2f hidden %#x sum %.3f dst %s" % (int(ev("g_GlobalTimer")) - t0, act, float(ev("%s.model->animframe1" % C)),
            float(ev("%s.model->playspeed" % C)) if False else 0.0, int(ev("%s.hidden" % C)), float(ev("%s.shotbondsum" % C)), ev("g_CurrentPlayer->damageshowtime")))
    if frames % 40 == 1:
        print("GEHIT f %d act %s item %s dst %s sum %.3f" % (frames, ev("%s.actiontype" % C), ev("%s.act_attack.attack_item" % C), ev("g_CurrentPlayer->damageshowtime"), float(ev("%s.shotbondsum" % C))))
el = int(ev("g_GlobalTimer")) - t0
print("GEHIT CYCLE ticks %d firing %d attacks %d -> %.1f ticks an attack, %.1f firing" % (el, cyc["firing"], cyc["attacks"], el / max(1, cyc["attacks"]), cyc["firing"] / max(1, cyc["attacks"])))
print("GEHIT acc %s dmg %s diff %s" % (ev("g_AiAccuracyModifier"), ev("g_AiDamageModifier"), ev("g_SelectedDifficulty")))
print("GEHIT RESULT mode %s dist %.0f ticks %d frames %d delta %.2f item %s calls %d ok %d sum %.3f hits %d dmgcalls %d hits/min %.1f" % (
    MODE, D, el, frames, deltas / max(1, frames), ev("%s.act_attack.attack_item" % C), stats["calls"], stats["ok"], stats["sum"], stats["resets"], stats["dmg"], stats["resets"] * 3600.0 / max(1, el)))
gdb.execute("kill"); gdb.execute("quit")
