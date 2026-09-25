"""GoldenEye's own frames of one animation on a Facility guard stood in front of
Bond, held on each of FRAMES (comma list) of animation ANIM (its row in
animation_table_ptrs1, the initanitable order), written to TOUR_OUT as PPMs,
the guard's AI and aim zeroed. Oracle half of
the GE Plus guard aim check."""
import gdb, os, math
gdb.execute("set pagination off"); gdb.execute("set confirm off")
OUT = os.environ["TOUR_OUT"]; ANIM = int(os.environ.get("ANIM", "75"), 0)
FRAMES = [float(x) for x in os.environ.get("FRAMES", "30,55,67,77,87").split(",")]
FLIP = int(os.environ.get("FLIP", "0"))
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
    if stan:
        for f in ("prop->stan", "field_488.current_tile_ptr", "field_488.current_tile_ptr_for_portals"):
            gdb.execute("set variable g_CurrentPlayer->%s = (StandTile *)%d" % (f, stan))
    gdb.execute("set variable g_CurrentPlayer->vv_theta = %f" % theta)
    gdb.execute("set variable g_CurrentPlayer->vv_verta = %f" % verta)
for _ in range(20):
    place(); until(now() + 3)
th = math.radians(theta)
D = float(os.environ.get("D", "220"))
GX, GZ = X - math.sin(th) * D, Z + math.cos(th) * D
gstan = int(ev("(unsigned long)stanFindFloorTileBelowY(%f, %f, %f, 0.0f)" % (GX, Y + 100, GZ)))
c = None
for i in range(int(ev("g_NumChrSlots"))):
    if int(ev("(long)g_ChrSlots[%d].model" % i)) and int(ev("(long)g_ChrSlots[%d].prop" % i)):
        c = i; break
C = "g_ChrSlots[%d]" % c
gy = float(ev("g_CurrentPlayer->field_488.pos.y")) if False else None
print("GEAIM chr slot %d tile %#x" % (c, gstan))
face0 = math.atan2(X - GX, Z - GZ) % (2 * math.pi)
gy = float(ev("stanGetPositionYValue((StandTile *)%d, %f, %f)" % (gstan, GX, GZ)))
print("GEAIM ground %.1f" % gy)
gdb.execute("set variable %s.ailist = 0" % C)
gdb.execute("call (void)sub_GAME_7F03D058(%s.prop, 0)" % C)
gdb.execute("set variable %s.prop->pos.x = %f" % (C, GX)); gdb.execute("set variable %s.prop->pos.y = %f" % (C, gy)); gdb.execute("set variable %s.prop->pos.z = %f" % (C, GZ))
gdb.execute("set variable %s.prop->stan = (StandTile *)%d" % (C, gstan))

gdb.execute("set variable %s.chrflags = %s.chrflags | CHRFLAG_INIT" % (C, C))
gdb.execute("call (void)setsubroty(%s.model, %f)" % (C, face0))
gdb.execute("call (void)setsuboffset(%s.model, &%s.prop->pos)" % (C, C))
gdb.execute("call (void)chrDetectRooms(&%s)" % C)
gdb.execute("call (void)sub_GAME_7F03D058(%s.prop, 1)" % C)
anim = "(ModelAnimation *)(unsigned long)(unsigned int)animation_table_ptrs1[%d]" % ANIM
face = math.atan2(X - GX, Z - GZ) % (2 * math.pi)
for fr in FRAMES:
    for k in range(4):
        place()
        gdb.execute("set variable %s.actiontype = ACT_NULL" % C)
        gdb.execute("call (void)chrStopFiring(&%s)" % C)
        gdb.execute("set variable %s.aimuprshoulder = 0" % C); gdb.execute("set variable %s.aimuplshoulder = 0" % C)
        gdb.execute("set variable %s.aimupback = 0" % C); gdb.execute("set variable %s.aimsideback = 0" % C)
        gdb.execute("call (void)setsubroty(%s.model, %f)" % (C, face))
        gdb.execute("call (void)modelSetAnimation(%s.model, %s, %d, %f, 0.0f, 0.0f)" % (C, anim, FLIP, fr))
        until(now() + 1)
    print("GEAIM anim %s want %s frame1 %s act %s" % (ev("%s.model->anim" % C), ev(anim), ev("%s.model->animframe1" % C), ev("%s.actiontype" % C)))
    print("GEAIM frame %.1f chr pos %.1f %.1f %.1f" % (fr, float(ev("%s.prop->pos.x" % C)), float(ev("%s.prop->pos.y" % C)), float(ev("%s.prop->pos.z" % C))))
    gdb.execute("call (int)fast3dWritePPM(\"%s/pose_%d_%05.1f.ppm\", (const unsigned short *)osViGetCurrentFramebuffer(), g_colorImageWidth, g_scissorLry)" % (OUT, ANIM, fr))
gdb.execute("kill"); gdb.execute("quit")
