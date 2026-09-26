"""GoldenEye's twenty solo missions as Perfect Dark setups.

GoldenEye's setup file is Perfect Dark's ancestor and the two have not moved
far apart: the same ten-slot header (waypoints, waygroups, intro, props, paths,
ailists, pads, bound pads, and two name tables Perfect Dark dropped), and the
same propdef type numbers - door 1, guard 9, the objectives 0x17-0x23, glass
0x2a, tinted glass 0x2f - all the way up. What differs is the size of a record
(GoldenEye's ObjectRecord is 0x80 bytes against Perfect Dark's 0x5c), the
second opcode byte on an AI command, and the handful of fields Perfect Dark
moved. So a mission converts record for record.

Everything here reads the ROM alone. GE-X is the oracle the field maps were
learnt from (CLAUDE-notes/ge-bean.md) and is never read.

A record keeps its place in the list: every GoldenEye record becomes exactly
one Perfect Dark record, so the indices the file refers to itself by - a door's
sibling, a tag's object, an objective's criteria - are the same on both sides.
A type with no Perfect Dark equivalent becomes OBJTYPE_22, which is one word
and does nothing.
"""
import struct

import gerom
import geobjects
import geaitable
import geanimtable

MODEL_REMAKE_FIRST = geobjects.MODEL_REMAKE_FIRST

# Perfect Dark's own record sizes in words, as port/src/preprocess/filesetup.c
# sizes them (objSizeN64(), the n64_* structs)
PD_SIZES = {
    0x01: 55, 0x02: 2, 0x03: 23, 0x04: 24, 0x05: 23, 0x06: 49, 0x07: 24, 0x08: 26,
    0x09: 11, 0x0a: 53, 0x0b: 140, 0x0c: 23, 0x0d: 43, 0x0e: 2, 0x0f: 23, 0x11: 23,
    0x12: 2, 0x13: 5, 0x14: 42, 0x15: 26, 0x16: 4, 0x17: 4, 0x18: 1, 0x19: 2,
    0x1a: 2, 0x1b: 2, 0x1c: 2, 0x1d: 2, 0x1e: 4, 0x1f: 1, 0x20: 4, 0x21: 5,
    0x22: 1, 0x23: 4, 0x24: 23, 0x25: 10, 0x26: 4, 0x27: 34, 0x28: 35, 0x2a: 24,
    0x2b: 23, 0x2c: 5, 0x2d: 32, 0x2e: 7, 0x2f: 26, 0x30: 37, 0x31: 5, 0x32: 4,
    0x33: 56, 0x34: 1,
}

# The GoldenEye types that are an ObjectRecord with a tail: the tail is copied
# from GoldenEye's 0x80 to Perfect Dark's 0x5c, field by field, as
# (GoldenEye offset, Perfect Dark offset, width, multiplier). The rest of a
# Perfect Dark record is left zero, which is what its loader expects of the
# runtime fields.
OBJ_TAILS = {
    0x04: ((0x80, 0x5c, 4, 1),),                       # key: the key flags
    0x07: ((0x80, 0x5c, 4, 1),),                       # ammo crate: the ammo type
    0x15: ((0x80, 0x5c, 4, 1), (0x84, 0x60, 4, 1)),    # armour: initial and current
    # A truck and an aircraft each own an AI list, and its **id** is what both
    # games keep in the record: GoldenEye's own load leaves the field alone and
    # Perfect Dark's `setupCreateProps()` reads it as one
    # (`ailistFindById((uintptr_t)truck->ailist)`). Everything else in the tail
    # - the speed, the path, the rotor - is set to zero at the creation by both
    # games, so the id is the whole of what has to move. Without it the field
    # was zero and `ailistFindById(0)` handed every vehicle in the game
    # **Perfect Dark's own global list 0**, which is the same fault the guards
    # had before their eighteen global lists were converted.
    0x27: ((0x80, 0x5c, 4, 1),),                       # truck: the AI list it runs
    0x28: ((0x80, 0x5c, 4, 1),),                       # aircraft: the AI list it runs
    # A tank's shells: the one word of its tail the setup sets (0xd8, thirty on
    # Runway and on Streets), which is what Bond is handed as he climbs in
    0x2d: ((0xd8, 0x5c, 4, 1),),                       # tank: the shells in it
    # An autogun's turn limits, turn speed and range: the same fields in the
    # same order on both sides, 0x24 further on here, and both loads convert
    # them out of the setup's 16.16 integers alike. Left at nought until
    # converter 67, and no autogun of any mission could see or turn. Its pad
    # is AUTOGUN below.
    0x0d: ((0x88, 0x64, 4, 1), (0x8c, 0x68, 4, 1),     # autogun: ymaxleft, ymaxright
           (0xa4, 0x80, 4, 1), (0xa8, 0x84, 4, 1)),    #          maxspeed, aimdist
}
# An autogun's pad - the one it rests facing - is GoldenEye's s32 at 0x80 and
# Perfect Dark's s16 at 0x5c, -1 for none.
AUTOGUN = 0x0d
# Glass (0x2a) has no tail: GoldenEye's record is the ObjectRecord and nothing
# more, and Perfect Dark's portalnum is found at the load. Reading one anyway
# takes the head of the next record.

# GoldenEye types with no Perfect Dark record of the same shape. They keep their
# place in the list as a one-word OBJTYPE_22, which the engine walks over.
#
# **Hats (0x11)** are left out with them. GoldenEye's hat is its own model, and
# a converted one is a rigid prop - one matrix, a position node at its root -
# which Perfect Dark cannot pose on a head: the frame a guard wearing one was
# ticked, modelasm00018680() took the parent's matrix of a node that has none
# and the mission died. GoldenEye's own heads carry their hats anyway, and the
# remake's guards wear Bean's, GE-X's or Perfect Dark's heads.
#
# **A switch (0x13) is kept.** GoldenEye's PROPDEF_SWITCH - "activating the
# first object activates the second, a door" - is what opens Dam's gates from
# their consoles, Facility's doors from their monitors and Aztec's from theirs
# (ten records over the twenty missions, every one a console and a door).
# Perfect Dark grew the same record a word into OBJTYPE_LINKLIFTDOOR and still
# carries GoldenEye's behaviour as the branch of doorCallLift() where the
# "lift" is a door, so it goes through as a short record: the two offsets are
# relative record indices, which the conversion keeps.
AS_NOTHING = {0x0e, 0x11, 0x12}
MONITOR = 0x0a
MULTI_MONITOR = 0x0b

OBJTYPE_NOTHING = 0x22
OBJTYPE_END = 0x34
OBJTYPE_CHR = 0x09

# GoldenEye's own item ids (bondconstants.h, ITEM_IDS) as the port's weapon
# numbers, which are not in the same order: GoldenEye's list starts with the
# unarmed hand and the two knives and names its guns after the real ones, and
# the port's twenty-five are GoldenEye's in-game names in Perfect Dark's own
# order. Everything past the last mine is a gadget, a key or a document, which
# Perfect Dark cannot hold as a weapon at all.
GE_ITEM_WEAPON = (
    0x01,            # 0  UNARMED       WEAPON_UNARMED
    0x01,            # 1  FIST          WEAPON_UNARMED
    0x71,            # 2  KNIFE         hunting knife
    0x72,            # 3  THROWKNIFE    throwing knife
    0x5e,            # 4  WPPK          PP7
    0x5f,            # 5  WPPKSIL       PP7 (silenced)
    0x60,            # 6  TT33          DD44 Dostovei
    0x61,            # 7  SKORPION      Klobb
    0x62,            # 8  AK47          KF7 Soviet
    0x63,            # 9  UZI           ZMG (9mm)
    0x64,            # 10 MP5K          D5K Deutsche
    0x65,            # 11 MP5KSIL       D5K (silenced)
    0x66,            # 12 SPECTRE       Phantom
    0x67,            # 13 M16           AR33 Assault Rifle
    0x68,            # 14 FNP90         RC-P90
    0x69,            # 15 SHOTGUN       Shotgun
    0x6a,            # 16 AUTOSHOT      Automatic Shotgun
    0x6b,            # 17 SNIPERRIFLE   Sniper Rifle
    0x6c,            # 18 RUGER         Cougar Magnum
    0x6d,            # 19 GOLDENGUN     Golden Gun
    0x5e,            # 20 SILVERWPPK    a PP7; the port has no silver one of its own
    0x5e,            # 21 GOLDWPPK      a PP7; nor a gold one
    0x6e,            # 22 LASER         Moonraker laser
    0x6e,            # 23 WATCHLASER    the watch laser is the same beam
    0x6f,            # 24 GRENADELAUNCH Grenade Launcher
    0x70,            # 25 ROCKETLAUNCH  Rocket Launcher
    0x73,            # 26 GRENADE       Grenade
    0x74,            # 27 TIMEDMINE     Timed Mine
    0x75,            # 28 PROXIMITYMINE Proximity Mine
    0x76,            # 29 REMOTEMINE    Remote Mine
)

# GoldenEye's ammunition types (bondconstants.h, AMMOTYPES) as the types the
# port's GoldenEye guns draw on, which are their hosts' (geguns.c): the PP7 and
# the DD44 stand on Perfect Dark's pistols and the Klobb, the ZMG, the D5K, the
# Phantom and the RC-P90 on its submachine guns, so GoldenEye's one pool of 9mm
# is two pools here and a grant of it fills both. Everything past the golden
# bullet is a gadget's count (the covert modem's 20, the bomb case's), which
# nothing in the port holds.
GE_AMMO_TYPES = {
    1: (0x01, 0x02),    # 9MM            pistol and SMG
    2: (0x01, 0x02),    # 9MM_2
    3: (0x04,),         # RIFLE
    4: (0x05,),         # SHOTGUN
    5: (0x07,),         # GRENADE
    6: (0x08,),         # ROCKETS
    7: (0x0c,),         # REMOTEMINE
    8: (0x0d,),         # PROXMINE
    9: (0x0e,),         # TIMEDMINE
    10: (0x09,),        # KNIFE
    11: (0x0b,),        # GRENADEROUND   the grenade launcher stands on the Devastator
    12: (0x0a,),        # MAGNUM
    13: (0x11,),        # GGUN           golden bullets, the port's pool of their own (AMMOTYPE_GOLDENGUN)
    20: (0x20,),        # BUG            the covert modem stands on the ECM mine
    22: (0x20,),        # GEKEY          and so do the GoldenEye key
    23: (0x20,),        # PLASTIQUE      and the plastique; no mission holds two of the three
}

# The commands whose ITEM_NUM is a weapon somebody holds (geaitable.py): the two
# that put an item in Bond's hands (e3, e4) and the one that asks what is in
# them (59), and the two that ask where a *thrown* one ended up (57 and 58:
# Dam's covert modem on its console, Bunker 2's remote mine). An item anywhere
# else is a key or a gadget's tag and is left alone.
GE_EQUIP_OPS = (0xe3, 0xe4, 0x59, 0x57, 0x58)

# And the two that hand a *guard* a gun or take one off him - TRYGiveMeItem (bf)
# and TRYDroppingItem (1b) - which name the gun twice: its prop number and its
# item id. Both were copied as they stood until converter 42, and both are
# numbers Perfect Dark has its own meaning for: GoldenEye's prop 0xbf is the PP7
# and Perfect Dark's model 0xbf is a dataDyne lab door, its item 4 the PP7 and
# Perfect Dark's weapon 4 the Mauler. A hundred and five commands over the
# twenty missions, so every guard a list armed - Statue Park's troops, Bond in
# Archives' ending - carried a door for a gun. The prop becomes the remake's own
# model, as a setup record's does, and the item goes through GE_ITEM_WEAPON.
GE_GIVE_OPS = (0xbf, 0x1b)

# The two commands that ask about Bond's own health (geaitable.py rows 7f and
# 80). GoldenEye's threshold is a byte where 255 is a full one (chrai.c divides
# it by 255 and compares the result against currentPlayerGetHealth(), which is a
# fraction); Perfect Dark's aiIfChrHealth*Than scales its own byte by a tenth
# and, for a player, compares it against bondhealth * 8, so a full one is 80
# there. Left alone GoldenEye's 76 - a guard asking whether Bond is under 30% -
# reads as 7.6 against a maximum of 8 and is true whatever his health is.
GE_BOND_HEALTH_OPS = (0x7f, 0x80)
GE_BOND_HEALTH_FULL = 80          # and GoldenEye's own is 255

# The six chr flag commands (9d-a2) carry GoldenEye's chrflags, which are
# Perfect Dark's bit for bit where the two kept a meaning (INIT is
# FORCETOGROUND, INVINCIBLE, HIDDEN, NO_SHADOW...) - but not 0x1000.
# GoldenEye's is CHRFLAG_LOCK_Y_POS: chr.c's ground callback leaves the chr's
# ground, fall and height alone while it is set, which is how the Cradle's
# Trevelyan is taken off the platform and falls from its height rather than
# being stood on the valley floor under the pad he is teleported to. Perfect
# Dark's 0x1000 is CHRCFLAG_UNEXPLODABLE. The bit goes to the port's own
# CHRCFLAG_GE_LOCKY, 0x40000000 (stock's CHRCFLAG_40000000, never used, and no
# GoldenEye mission uses it). Dam, Surface and the Cradle set it (converter 71).
GE_CHRFLAG_OPS = (0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2)
GE_CHRFLAG_LOCKY = 0x00001000
PD_CHRFLAG_GE_LOCKY = 0x40000000

# The four commands that ask whether something is in **a pad's room** (44, 54,
# 55 and e6). GoldenEye's argument is a pad and it compares the room of the
# pad's tile with the room of the other's (chraction.c, chrIfInPadRoom()).
# Perfect Dark's argument is a **room number** - chrGetPadRoom() returns it as
# it stands - unless it is 10000 or more, when it is a pad plus 10000 and the
# answer is that pad's room. Copied as it stood, "Bond is in the room of pad
# 330" asked for room 330, which Dam does not have: no trigger of the kind had
# ever fired on a converted mission, Dam's dive among them.
GE_PADROOM_OPS = (0x44, 0x54, 0x55, 0xe6)
PD_PADROOM_PAD = 10000

# IFBondYPosLessThan (d6) has no twin in Perfect Dark and becomes the port's own
# command, past the game's table beside aiGeExitOnButtonPress:
#     01e3 <y:4, signed> <label:1>
# GoldenEye's height is in its own runtime world, so it moves by the level's
# offset as everything else in the conversion does; four bytes because a moved
# height need not fit GoldenEye's two.
GE_IFBONDY_OP = 0xd6
GE_IFBONDY_CMD = 0x01e3

# GoldenEye's own **global** AI lists: chraidata.c's g_GlobalAILists, eighteen
# lists every level shares - the standard guard, the simple guard, the attack,
# the idle animations, the keyboard basher, the alarm raiser - as (pointer, id)
# pairs in the data segment, their bytecode beside them.
#
# A chr record or a command names one of them by an id of **1024 or less**
# (bondconstants.h's `isGlobalAIListID`), and a level's own lists start at 1025.
# Perfect Dark draws that line in the same place (`ailistFindById`: 0x401 and up
# is the stage's, below it the game's own g_GlobalAilists) - so a converted
# guard whose list was GoldenEye's global 2, its standard guard, ran *Perfect
# Dark's* global 2 instead, and ten of Dam's thirty-six did. Every mission in
# GE Plus was running Perfect Dark's unalerted and alerted guard AI over
# GoldenEye's levels, which is what "the AI is not behaving like the ROM" is.
#
# The eighteen are converted with each mission's own lists and given ids of
# their own, and every reference to one is moved with them. **The ids have to sit
# under 0x1000**: Perfect Dark makes a background chr of every stage list from
# there up (game_00b820.c) and ticks it from the first frame, so at 0x2000 the
# eighteen were each ticked as a chr of their own and m_RunToBond took the game
# down in chrGoToRoomPos() with no prop to move. 0x800 is above the highest id
# any of the twenty missions gives a list of its own (1066) and below the
# background lists' 0x1000.
GLOBAL_AI_AT = 0x8003744c
GE_GLOBAL_FIRST = 0x0800
GE_GLOBAL_LAST_ID = 1024


def global_ai_id(v):
    """A GoldenEye AI list id as the converted level's: its global lists move
    to GE_GLOBAL_FIRST and the level's own keep their numbers."""
    return GE_GLOBAL_FIRST + v if v <= GE_GLOBAL_LAST_ID else v


def global_lists(data):
    """[(id, offset)] of GoldenEye's own global AI lists in the data segment."""
    out = []
    at = GLOBAL_AI_AT - gerom.DATA_VRAM
    while True:
        ptr, lid = struct.unpack_from('>Ii', data, at + 8 * len(out))
        if not ptr:
            return out
        out.append((lid, ptr - gerom.DATA_VRAM))


# GoldenEye's gadgets, weapons of the port's own past the twenty-five guns
# (gegadgets.c). Three are *thrown and stick*, which GoldenEye's own code treats
# as mines from the hand to the wall (gun.c, gunfire.c), and stand on the ECM
# mine: Dam's covert modem and Frigate's tracker bug are ITEM_BUG, thrown as
# PROP_CHRBUG and counted in AMMO_BUG. The camera and the watch magnet do
# something of their own. And six have no model in the hand at all - equipped,
# and used by activating the thing they are for - which is the Data Uplink; a
# weapon number is an s8 and there are not six left, so they share two and the
# mission says which each is (no mission holds two of the same letter).
GE_GADGET_WEAPON = {
    47: 0x77,           # BUG              covert modem, tracker bug
    34: 0x78,           # PLASTIQUE        Silo
    61: 0x79,           # GOLDENEYEKEY     Bunker
    40: 0x7a,           # CAMERA           Bunker, Silo
    60: 0x7b,           # WATCHMAGNETATTRACT  Bunker 2, Archives
    38: 0x7c,           # DOORDECODER      Facility        gadget A
    39: 0x7c,           # BOMBDEFUSER      Frigate         gadget A
    46: 0x7c,           # KEYANALYSERCASE  Bunker          gadget A
    50: 0x7c,           # EXPLOSIVEFLOPPY  Aztec           gadget A (its guidance data)
    55: 0x7d,           # DATATHIEF        Bunker          gadget B
    73: 0x7d,           # DATTAPE          Aztec           gadget B
}
# the thrown ones' props, which no setup record need name: PROP_CHRBUG,
# PROP_CHRGOLDENEYEKEY and PROP_CHRPLASTIQUE
GE_GADGET_MODELS = (245, 248, 273)

# GoldenEye's PROPDEF_OBJECTIVE_COPY_ITEM (0x22, Bunker's "copy the GoldenEye
# key") asks one thing - has the key analyser copied the key - and Perfect Dark
# has no such record; its 0x22 is the one-word "nothing". It becomes a
# complete-on-flag objective on a stage flag of the port's own, which the key
# analyser sets (gegadgets.c's GEGADGET_COPY_FLAG). No mission's lists touch
# the top bit.
GE_COPYITEM = 0x22
GE_COPYITEM_FLAG = 0x80000000
OBJECTIVETYPE_COMPFLAGS = 0x1a

# The two objective records that name a **pad's room** (0x20 "enter room", 0x21
# "deposit item in room"): GoldenEye takes the pad's tile's room
# (objectivestatusCheckRoomEntered()) and Perfect Dark hands the number to
# chrGetPadRoom(), which reads it as a room unless it is a pad plus 10000 - the
# AI commands' fault (GE_PADROOM_OPS) over again, so Facility's, Surface 2's,
# Bunker 2's and Archives' could never complete. 0x21 names its item too. And
# 0x25 renames an inventory item for the mission: an item id and five text ids.
GE_ENTERROOM = 0x20
GE_DEPOSITROOM = 0x21
GE_RENAME = 0x25


def objective_room_record(t, raw, numpads):
    """GoldenEye's enter-room or deposit-in-room record as Perfect Dark's."""
    n = PD_SIZES[t]
    rec = bytearray(4 * n)
    keep = min(4 * n, len(raw))
    rec[0:keep] = raw[0:keep]
    rec[3] = t
    at = 4
    if t == GE_DEPOSITROOM:
        struct.pack_into('>i', rec, 4, item_weapon(struct.unpack_from('>i', raw, 4)[0]))
        at = 8
    pad = struct.unpack_from('>i', raw, at)[0]
    if 0 <= pad < NO_PAD:
        struct.pack_into('>i', rec, at, pad_num(pad, numpads) + PD_PADROOM_PAD)
    return bytes(rec)


def rename_record(raw):
    """GoldenEye's rename record: the item as the port's weapon, the five texts
    out of the mission's own bank."""
    n = PD_SIZES[GE_RENAME]
    rec = bytearray(4 * n)
    keep = min(4 * n, len(raw))
    rec[0:keep] = raw[0:keep]
    rec[3] = GE_RENAME
    item = struct.unpack_from('>i', raw, 8)[0]
    struct.pack_into('>i', rec, 8, item_weapon(item) if item > 0 else 0)
    for k in range(5):
        struct.pack_into('>I', rec, 12 + 4 * k, text_id(struct.unpack_from('>I', raw, 12 + 4 * k)[0] & 0xffff))
    return bytes(rec)

def item_weapon(item):
    """A GoldenEye item id as the weapon Perfect Dark equips for it."""
    if item in GE_GADGET_WEAPON:
        return GE_GADGET_WEAPON[item]
    return GE_ITEM_WEAPON[item] if item < len(GE_ITEM_WEAPON) else 0


def STOCK_BODIES(bodyid, headid):
    """A guard's body and head as the file keeps them: GoldenEye's own numbers.

    The port maps them when the mission loads, because what a GoldenEye guard
    can be made of depends on what the player has installed - GoldenEye's own
    characters out of the XBLA release (gebean.c), GE-X's borrowed ones, or
    Perfect Dark's own - and the conversion runs before any of that is known.
    """
    return bodyid & 0xff, (headid & 0xff) if headid >= 0 else 0xff


def ge_records(d):
    """[(index, type, bytes)] of a GoldenEye setup's propdefs."""
    h = struct.unpack_from('>10I', d, 0)
    o, out = h[3], []
    while o + 4 <= len(d):
        t = d[o + 3]
        if t == 48:
            return out
        n = geobjects.GE_SIZES.get(t)
        if n is None:
            raise ValueError('setup: propdef type %d at %#x' % (t, o))
        out.append((len(out), t, d[o:o + 4 * n]))
        o += 4 * n
    return out


NO_PAD = 0xffff

# A converted PlayAnimation carries GoldenEye's own animation id with this bit
# set, since the number an appended animation takes is only known once the port
# has appended it (gexPlusMissionAnim()). The tag is what tells one of
# GoldenEye's from a stock list's own animation number, which the same command
# carries and which a mission's chrs do run.
GE_ANIM_TAG = 0x8000

# GoldenEye's vehicle animations (animation_table_ptrs2[]) take an id space of
# their own, since they share their numbering with the guards' table and only
# an AI list's owner tells the two apart (geanimtable.py, port's geanimtable.h)
GEVEH_ANIM_FIRST = 256
# and the port's own command that plays one on a vehicle's model, past the
# game's table as TriggerFadeAndExitLevelOnButtonPress is
GEVEH_ANIM_CMD = 0x01e2


def pad_num(p, numpads, bound=False):
    """A GoldenEye pad id in the converted level.

    GoldenEye's own pads keep their index and its bound pads are written after
    them, so a bound pad - one at 10000 and up, and a door's pad field, which is
    always one - is numpads plus its own index. 0xffff is not a pad at all: a
    collectable a guard carries has no place to stand, and 36 of Dam's did,
    which came out as pad 55902 and made the loader read past the pad table.
    """
    if p == NO_PAD:
        return NO_PAD
    if bound:
        return p + numpads
    return p + numpads - 10000 if p >= 10000 else p


def pad_of(t, raw, numpads):
    return pad_num(struct.unpack_from('>H', raw, 6)[0], numpads, bound=(t == 1))


def base_record(raw, pdtype, words, padnum):
    """GoldenEye's ObjectRecord as Perfect Dark's defaultobj."""
    out = bytearray(4 * words)
    out[0:3] = raw[0:3]
    out[3] = pdtype
    model = struct.unpack_from('>h', raw, 4)[0]
    struct.pack_into('>hHII', out, 4, MODEL_REMAKE_FIRST + model, padnum,
                     struct.unpack_from('>I', raw, 8)[0], struct.unpack_from('>I', raw, 12)[0])
    struct.pack_into('>hh', out, 0x4c, 0, geobjects.obj_health(raw))   # damage, maxdamage
    struct.pack_into('>I', out, 0x58, 0x0fff0000)    # the floor colour GE-X gives them
    return out


def guard_record(raw, numpads, bodies):
    """GoldenEye's GuardRecord as Perfect Dark's packedchr.

    GoldenEye keeps a guard in seven words - chr, pad, body, ai list, the two
    presets, health, reaction time, its flags and a head - and Perfect Dark's
    chr is eleven and names most of the same things:

        0x00 chrindex   0x02 unk02      0x03 type       0x04 spawnflags
        0x08 chrnum     0x0a padnum     0x0c bodynum    0x0d headnum
        0x0e ailistnum  0x10 padpreset  0x12 chrpreset  0x14 hearscale
        0x16 viewdist   0x18 flags      0x1c flags2     0x20 team
        0x21 squadron   0x22 chair      0x24 convtalk   0x28 tude ...

    GoldenEye's own setup flags are Perfect Dark's spawn flags for the three it
    uses: sunglasses (0x01), sunglasses half the time (0x02) and invincible
    (0x08). Its 0x04 is "this is a clone", which Perfect Dark has no spawn flag
    for.

    The two fields the GoldenEye decomp calls health and reaction time are its
    **hearing scale and vision range** - chraction.c reads them straight into
    the chr's hearingscale (over a thousand) and visionrange - which are Perfect
    Dark's own two fields, so they carry across as they are.

    The body and head are GoldenEye's own character numbers and are left as
    they are: what there is to wear is not known until the mission loads, so the
    port maps them then (gexplus.c's gexPlusMissionChr()).
    """
    (chrnum, padid, bodyid, ailist, preset, chrpreset,
     hearscale, viewdist, flags, headid) = struct.unpack_from('>10h', raw, 4)
    out = bytearray(4 * PD_SIZES[OBJTYPE_CHR])
    out[0:3] = raw[0:3]
    out[3] = OBJTYPE_CHR
    body, head = bodies(bodyid, headid)
    struct.pack_into('>I', out, 0x04, flags & 0x000b)
    struct.pack_into('>hH', out, 0x08, chrnum, pad_num(padid & 0xffff, numpads))
    out[0x0c] = body & 0xff
    out[0x0d] = head & 0xff
    struct.pack_into('>HHHHH', out, 0x0e, global_ai_id(ailist & 0xffff),
                     pad_num(preset & 0xffff, numpads), chrpreset & 0xffff,
                     hearscale & 0xffff, viewdist & 0xffff)
    struct.pack_into('>h', out, 0x22, -1)      # no chair
    return out


def door_record(raw, numpads, recs, index):
    """GoldenEye's DoorRecord as Perfect Dark's, the fields it moved put back.

    Learnt from GE-X's own copies of the eleven GoldenEye setups it kept pad for
    pad (CLAUDE-notes/ge-bean.md, "Runway and Caves"): the two fields at
    GoldenEye's 0x8c and 0x90 are times in a thousandth of Perfect Dark's, the
    flags' top byte moves 0x80 -> 0x40 and 0x08 -> 0x80 and drops 0x40, and the
    sibling is a record offset either way.
    """
    padnum = pad_of(1, raw, numpads)
    flags = geobjects._door_flags(struct.unpack_from('>I', raw, 8)[0])
    out = base_record(raw, 0x01, PD_SIZES[0x01], padnum)
    struct.pack_into('>I', out, 8, flags)
    for ge, pd, mul in ((0x84, 0x5c, 1), (0x88, 0x60, 1), (0x8c, 0x64, 1000), (0x90, 0x68, 1000),
                        (0x94, 0x6c, 1), (0x98, 0x70, 1), (0x9c, 0x74, 1), (0xa0, 0x78, 1)):
        struct.pack_into('>i', out, pd, struct.unpack_from('>i', raw, ge)[0] * mul)
    rel = struct.unpack_from('>i', raw, 0x80)[0]
    sib = index + rel
    struct.pack_into('>i', out, 0xbc, rel if rel and 0 <= sib < len(recs) and recs[sib][1] == 1 else 0)
    out[0xc6] = raw[0xa7]     # the opening sound
    out[0xcc] = 0xff
    return out


def weapon_record(raw, numpads):
    """A GoldenEye collectable as a Perfect Dark weapon prop, on the port's own
    GoldenEye weapons (geguns.c).

    The record's `weaponnum` is one of GoldenEye's **item ids** - its own code
    compares it against ITEM_GRENADE and ITEM_TIMEDMINE (chr.c) - and those are
    not the order the port's twenty-five are in, so it goes through
    GE_ITEM_WEAPON like the two commands that put an item in Bond's hands.
    Reading it as an index into the port's list made every gun on the floor the
    wrong one (the KF7 Soviet, item 8 and 408 of the missions' 908 guns, was a
    Phantom) and left the grenades and the mines - past the twenty-five - as
    nothing at all. An item that is not a weapon a player can hold (a key, a
    briefcase, the unarmed hand) is still nothing.
    """
    out = base_record(raw, 0x08, PD_SIZES[0x08], pad_of(8, raw, numpads))
    item = raw[0x80]
    out[0x5c] = item_weapon(item) if item >= 2 else 0
    out[0x61] = 0xff          # no second gun: dualweaponnum -1 (was 0x5d/0x5e until 72)
    struct.pack_into('>h', out, 0x62, struct.unpack_from('>h', raw, 0x82)[0])
    return out


# The language bank the missions' own text is served as (src/include/lang.h).
LANGBANK_GEMISSION = 0x45


def text_id(geid):
    """A GoldenEye text id as a Perfect Dark one.

    GoldenEye's is `bank * 0x400 + slot` and the bank is always the mission's
    own - the file the converter copies to menu/ and the port loads into
    LANGBANK_GEMISSION - so only the slot carries. Perfect Dark gives a slot
    nine bits; no mission's bank holds more than 108 strings, and a slot that
    would not fit becomes no text rather than another bank's string.
    """
    slot = geid & 0x3ff
    if not geid or slot >= 0x200:
        return 0
    return (LANGBANK_GEMISSION << 9) | slot


def objective_record(raw):
    """GoldenEye's objective heading as Perfect Dark's.

    GoldenEye gives an objective the lowest difficulty it appears at; Perfect
    Dark keeps a bit per difficulty, so the bits from that difficulty up are set.
    Its four difficulties are Perfect Dark's three and PD Mode, which is the
    hardest one again, so 007 takes 00 Agent's bit.
    """
    out = bytearray(4 * PD_SIZES[0x17])
    out[0:3] = raw[0:3]
    out[3] = 0x17
    index, text, mindiff = struct.unpack_from('>3i', raw, 4)
    struct.pack_into('>ii', out, 4, index, text_id(text))
    bits = 0
    # GoldenEye's own 0x100 is not a difficulty: objectiveIsAllComplete() tests
    # `objdiff <= curdiff`, so such an objective is never required and never
    # listed in the mission (its briefing file gives it a difficulty of its own
    # and the briefing screen does show it). Perfect Dark's test is the same
    # shape over difficulty bits, so it gets none.
    if mindiff < 3:
        for d in range(3):
            if d >= min(mindiff, 2):
                bits |= 1 << d
    out[0x0f] = bits
    return out


# GoldenEye's own cutscene camera, the thing a CameraSwitch turns the view to
CUTSCENE_CAMERA = 0x2e


def camera_record(raw, numpads, offset):
    """GoldenEye's cutscene camera, moved into the converted level.

    The record is GoldenEye's CutsceneRecord and Perfect Dark's cameraposobj -
    the same seven words meaning the same things - and **Perfect Dark's own
    setup load still does GoldenEye's conversion on it**: setup.c divides the
    position by 100 and the two angles by 65536 out of the integers the file
    holds, which is prop.c's PROPDEF_CAMERAPOS line for line. So the record is
    left in GoldenEye's own encoding and only the two things the conversion
    itself moved are moved here: the position, which is written as the integer
    whose hundredth is the converted level's own coordinate, and a bound pad's
    number.

    The look the record describes is GoldenEye's own and stays that way;
    playerExecutePreparedWarp() builds the vector from it (and honours the
    look-at-Bond flag) where a converted mission is playing.
    """
    v = struct.unpack_from('>7i', raw, 0)
    out = bytearray(raw[:4 * PD_SIZES[CUTSCENE_CAMERA]])
    out[3] = CUTSCENE_CAMERA
    for i in range(3):
        p = v[1 + i]
        if offset is not None:
            p -= int(round(float(offset[i]) * 100.0))
        struct.pack_into('>i', out, 0x04 + 4 * i, p)
    struct.pack_into('>I', out, 0x18, pad_num(v[6] & 0xffff, numpads))
    return bytes(out)


AMMO_CRATE = 0x07
TINTED_GLASS = 0x2f
MULTI_AMMO_CRATE = 0x14
GE_CRATE_SLOTS = 13
PD_CRATE_SLOTS = 19


def multi_crate_record(raw, numpads):
    """GoldenEye's crate of several kinds of ammunition as Perfect Dark's.

    Both keep a (model, quantity) pair for every ammunition type, indexed by the
    type less one - thirteen of GoldenEye's, nineteen of Perfect Dark's - so a
    pair moves to the slot of the type the port's guns draw on
    (GE_AMMO_TYPES), 9mm filling both of its pools. The model is the box each
    kind would be drawn as and is left at none: setupCreateProps() loads a
    slot's model only to have it ready, and a crate is picked up whole.
    """
    out = base_record(raw, MULTI_AMMO_CRATE, PD_SIZES[MULTI_AMMO_CRATE], pad_of(MULTI_AMMO_CRATE, raw, numpads))
    for i in range(PD_CRATE_SLOTS):
        struct.pack_into('>HH', out, 0x5c + 4 * i, 0xffff, 0)
    for k in range(GE_CRATE_SLOTS):
        qty = struct.unpack_from('>H', raw, 0x80 + 4 * k + 2)[0]
        if not qty:
            continue
        for pdtype in GE_AMMO_TYPES.get(k + 1, ()):
            at = 0x5c + 4 * (pdtype - 1)
            have = struct.unpack_from('>H', out, at + 2)[0]
            struct.pack_into('>HH', out, at, 0xffff, min(0xffff, have + qty))
    return out


def convert_props(d, numpads, bodies, models, stats, offset=None):
    """The mission's props, one Perfect Dark record for each of GoldenEye's."""
    recs = ge_records(d)
    out = []
    for index, t, raw in recs:
        if t == GE_COPYITEM:
            out.append(raw[0:3] + bytes([OBJECTIVETYPE_COMPFLAGS]) + struct.pack('>I', GE_COPYITEM_FLAG))
            stats['kept'][t] = stats['kept'].get(t, 0) + 1
            continue
        if t in (GE_ENTERROOM, GE_DEPOSITROOM):
            out.append(objective_room_record(t, raw, numpads))
            stats['kept'][t] = stats['kept'].get(t, 0) + 1
            continue
        if t == GE_RENAME:
            out.append(rename_record(raw))
            stats['kept'][t] = stats['kept'].get(t, 0) + 1
            continue
        if t in AS_NOTHING or t not in PD_SIZES:
            stats['dropped'][t] = stats['dropped'].get(t, 0) + 1
            out.append(struct.pack('>I', OBJTYPE_NOTHING))
            continue
        if geobjects.GE_SIZES[t] >= 32:
            # every ObjectRecord names a model, doors and collectables included
            models.add(struct.unpack_from('>h', raw, 4)[0])
        if t == 1:
            out.append(bytes(door_record(raw, numpads, recs, index)))
        elif t == OBJTYPE_CHR:
            out.append(bytes(guard_record(raw, numpads, bodies)))
        elif t == 8:
            out.append(bytes(weapon_record(raw, numpads)))
        elif t == 0x17:
            out.append(bytes(objective_record(raw)))
        elif t == CUTSCENE_CAMERA:
            out.append(camera_record(raw, numpads, offset))
        elif t == MULTI_AMMO_CRATE:
            out.append(bytes(multi_crate_record(raw, numpads)))
        elif geobjects.GE_SIZES[t] >= 32:
            # an ObjectRecord and a tail
            rec = base_record(raw, t, PD_SIZES[t], pad_of(t, raw, numpads))
            for ge, pd, w, mul in OBJ_TAILS.get(t, ()):
                v = int.from_bytes(raw[ge:ge + w], 'big') * mul
                rec[pd:pd + w] = v.to_bytes(w, 'big')
            if t == AUTOGUN and len(raw) >= 0x84:
                target = struct.unpack_from('>i', raw, 0x80)[0]
                struct.pack_into('>H', rec, 0x5c, NO_PAD if target < 0 else pad_num(target, numpads))
            if t == MONITOR and len(raw) >= 0xfc:
                # A hanging TV's mount. GoldenEye's MonitorObjRecord ends
                # OwnerOffset, OwnerPart, ImageNum, a word each at 0xf4, and
                # Perfect Dark's singlemonitorobj ends s16 owneroffset, s8
                # ownerpart, u8 imagenum at 0xd0: the same three, narrower. A
                # monitor with a negative pad hangs from the record that many
                # commands away (setup.c still has GoldenEye's branch for it),
                # and with the offset left at nought it hung from **itself** - a
                # prop that is its own child, which objFree() frees for ever on
                # the way out of the level. Both Bunkers ended the game when
                # they were left.
                struct.pack_into('>hb', rec, 0xd0,
                                 struct.unpack_from('>i', raw, 0xf4)[0], struct.unpack_from('>i', raw, 0xf8)[0])
                # and the programme it shows, one of GoldenEye's fifty-two
                # (gemonitortable.py), which the port plays on a remake stage
                rec[0xd3] = raw[0xff]
            if t == MULTI_MONITOR and len(raw) >= 0x254:
                # the four screens of a bank of monitors: a byte each after the
                # four MonitorRecords, which are 0x74 in both games
                rec[0x22c:0x230] = raw[0x250:0x254]
            if t == AMMO_CRATE:
                # the crate's one type, in the port's numbering
                pdtypes = GE_AMMO_TYPES.get(struct.unpack_from('>i', raw, 0x80)[0], (0,))
                struct.pack_into('>i', rec, 0x5c, pdtypes[0])
            if t == TINTED_GLASS and len(raw) >= 0x94:
                # A tinted pane's distances and its portal: a word each at
                # GoldenEye's 0x80, an s16 each at Perfect Dark's 0x5c, then the
                # 16.16 fraction both games divide at the load. Left at nought a
                # pane was opaque at any distance and shut **portal 0** - every
                # one of GoldenEye's own carries -1 (geconvert.c has the rest).
                for k in range(4):
                    v = struct.unpack_from('>i', raw, 0x80 + 4 * k)[0]
                    struct.pack_into('>h', rec, 0x5c + 2 * k, max(-32768, min(32767, v)))
                rec[0x64:0x68] = raw[0x90:0x94]
            out.append(bytes(rec))
        else:
            # a short record: the same fields in the same order on both sides
            n = PD_SIZES[t]
            rec = bytearray(4 * n)
            keep = min(4 * n, len(raw))
            rec[0:keep] = raw[0:keep]
            rec[3] = t
            out.append(bytes(rec))
        stats['kept'][t] = stats['kept'].get(t, 0) + 1
    out.append(struct.pack('>I', OBJTYPE_END))
    return b''.join(out)


# GoldenEye's intro commands are Perfect Dark's own: the same types in the same
# order at the same widths, and only the end differs - GoldenEye stops at 9 and
# Perfect Dark at 12. **Type 6 is ten words**, which is what Perfect Dark's is
# (modrandom.c sizes INTROCMD_6 at 40 bytes) and what Dam's two of them lie at;
# geconvert.read_setup() had it at 13, which no multiplayer setup ever reached
# because none of them carries one.
GE_INTRO_WORDS = {0: 3, 1: 4, 2: 4, 3: 8, 4: 2, 5: 2, 6: 10, 7: 3, 8: 2}
INTROCMD_END = 12


INTROTYPE_CAMERA = 6


def intro_camera(raw, numpads, scale, offset):
    """GoldenEye's own opening camera shot, moved into the converted level.

    The record is where the camera stands (hundredths of a converted unit), the
    yaw and pitch it looks along (16.16 radians), the pad whose room it is in,
    and the one or two lines of text it shows two and five seconds in
    (bondtypes.h's SetupIntroCamera, played in bondview2.c). GoldenEye picks one
    of a level's at random each time the mission starts; GE Plus's Cinema page
    plays them all.

    Nothing in Perfect Dark reads the record - it steps over it by its length
    and never looks inside - so the fields are rewritten as the port wants them:
    the position in the level's own frame as three floats, the angles as
    radians, the pad renumbered and the text ids as the mission's own bank.
    """
    v = struct.unpack('>10i', raw)
    out = bytearray(raw)
    # a hundredth of one of these is already a unit of the *converted* level -
    # GoldenEye's own units times its level scale - so only the level's offset
    # is taken off. Dam's first shot lands 38 units from pad 312, which is the
    # pad the record itself names.
    pos = [v[1] / 100.0, v[2] / 100.0, v[3] / 100.0]
    if offset is not None:
        pos = [float(p) - float(o) for p, o in zip(pos, offset)]
    struct.pack_into('>3f', out, 0x04, *pos)
    struct.pack_into('>2f', out, 0x10, v[4] / 65536.0, v[5] / 65536.0)
    struct.pack_into('>I', out, 0x18, pad_num(v[6] & 0xffff, numpads))
    struct.pack_into('>2I', out, 0x1c, text_id(v[7] & 0xffff), text_id(v[8] & 0xffff))
    return bytes(out)


INTROTYPE_SWIRL = 3


def intro_swirl(raw, numpads):
    """One point of the camera's swirl down to Bond, as floats.

    GoldenEye converts the record in place when the level loads (bondview_r.c:
    the offset from Bond, the spline's scale and the leg's duration, each a
    16.16 fixed point word over 65536) and nothing in Perfect Dark reads an
    INTROCMD_3 at all, so the conversion does it here and the port reads floats
    (gecinema.c). The offset is in GoldenEye's own runtime units, which are the
    converted level's. The last word is a pad or -1.
    """
    v = struct.unpack('>8i', raw)
    out = bytearray(raw)
    struct.pack_into('>5f', out, 0x08, *[x / 65536.0 for x in v[2:7]])
    pad = v[7]
    struct.pack_into('>i', out, 0x1c, pad_num(pad & 0xffff, numpads) if pad >= 0 else -1)
    return bytes(out)


INTROTYPE_ITEM = 1
INTROTYPE_AMMO = 2


def intro_item(raw):
    """What Bond starts with, as the port's own GoldenEye guns.

    The command's two items are GoldenEye's **item ids**, as a collectable's is
    (weapon_record()), and copied as they were they are read as Perfect Dark's
    weapon numbers: Dam's silenced PP7, item 5, was a MagSec 4. An item that is
    not a weapon - the covert modem, the bomb case, the key analyser - is
    nothing the port can put in a hand, and its command is left out rather than
    handed over as whatever Perfect Dark keeps at that number.
    """
    v = list(struct.unpack('>4i', raw))
    right = item_weapon(v[1]) if v[1] >= 0 else 0
    if not right:
        return []
    left = item_weapon(v[2]) if v[2] >= 0 else 0
    return [struct.pack('>4i', v[0], right, left if left else -1, v[3])]


def intro_ammo(raw):
    """Bond's starting ammunition, in the pools the port's guns draw on."""
    v = struct.unpack('>4i', raw)
    return [struct.pack('>4i', v[0], t, v[2], v[3]) for t in GE_AMMO_TYPES.get(v[1], ())]


def convert_intro(d, numpads, scale=None, offset=None):
    h = struct.unpack_from('>10I', d, 0)
    out, spawns = [], 0
    if not h[2]:
        return struct.pack('>i', INTROCMD_END), 0
    o = h[2]
    while o + 4 <= len(d):
        t = struct.unpack_from('>i', d, o)[0] & 0xff
        if t == 9:
            break
        n = GE_INTRO_WORDS.get(t)
        if n is None:
            break
        if t == INTROTYPE_CAMERA:
            out.append(intro_camera(d[o:o + 4 * n], numpads, scale, offset))
        elif t == INTROTYPE_SWIRL:
            out.append(intro_swirl(d[o:o + 4 * n], numpads))
        elif t == INTROTYPE_ITEM:
            out.extend(intro_item(d[o:o + 4 * n]))
        elif t == INTROTYPE_AMMO:
            out.extend(intro_ammo(d[o:o + 4 * n]))
        else:
            out.append(d[o:o + 4 * n])
        if t == 0:
            spawns += 1
        o += 4 * n
    out.append(struct.pack('>i', INTROCMD_END))
    return b''.join(out), spawns


def waypoint_pads(d):
    """The pad each of a GoldenEye setup's waypoints stands on, by its index."""
    h = struct.unpack_from('>10I', d, 0)
    out, o = [], h[0]
    while h[0] and o + 16 <= len(d):
        pad = struct.unpack_from('>i', d, o)[0]
        if pad < 0:
            break
        out.append(pad)
        o += 16
    return out


def convert_paths(d, at, numpads):
    """GoldenEye's patrol paths as Perfect Dark's: the same record - a pointer
    to a -1 terminated list, an id, a loop flag and a length - but **not the
    same list**. GoldenEye's is of *waypoints* (chraction.c's
    chrlvGetPatrolStepPad(): `pads[pathwaypoints[path->data[step]].padID]`, and
    the truck's tick reads its path the same way) and Perfect Dark's is of
    *pads* (`chr->act_patrol.path->pads[step]`, straight into padUnpack()). Copied
    as it was, a waypoint's index was read as a pad's number, so every guard on
    patrol walked for pads that were never on its route and Dam's truck turned
    round and drove the wrong way down the road. Each entry goes through the
    setup's own waypoint table to the pad it stands on."""
    h = struct.unpack_from('>10I', d, 0)
    if not h[4]:
        return struct.pack('>IBBH', 0, 0, 0, 0), b''
    waypads = waypoint_pads(d)
    paths, lists = [], []
    o = h[4]
    while True:
        ptr, pid, flags, ln = struct.unpack_from('>IBBH', d, o)
        if ptr == 0:
            break
        pads = []
        at2 = ptr
        while True:
            v = struct.unpack_from('>i', d, at2)[0]
            if v == -1:
                break
            if 0 <= v < len(waypads):
                pads.append(pad_num(waypads[v], numpads))
            at2 += 4
        paths.append((pid, flags, ln, pads))
        o += 8
    head = 8 * (len(paths) + 1)
    pos = at + head
    body = b''
    out = b''
    for pid, flags, ln, pads in paths:
        out += struct.pack('>IBBH', pos, pid, flags, ln)
        blob = b''.join(struct.pack('>i', p) for p in pads) + struct.pack('>i', -1)
        body += blob
        pos += len(blob)
    out += struct.pack('>IBBH', 0, 0, 0, 0)
    return out, body


def ai_length(d, at):
    """The length of the GoldenEye AI command at `at`.

    Every command has its own fixed length but one: PRINT is a debug comment
    whose text follows the opcode and runs to a NUL (chrai.c's chraiitemsize()).
    A walk that does not measure it lands in the middle of the next command,
    which is what truncated 225 of the missions' lists.
    """
    op = d[at]
    if op >= len(geaitable.TABLE):
        return None
    ln = geaitable.TABLE[op][1]
    if ln is not None:
        return ln
    end = at + 1
    while end < len(d) and d[end]:
        end += 1
    return end - at + 1


def convert_ailist(d, at, stats, numpads, vehicle=False, offset=None):
    """One GoldenEye AI list as Perfect Dark bytecode (geaitable.py).

    A command's pad argument is moved the way a record's is: GoldenEye's bound
    pads are written after its own, so one at 10000 and up becomes numpads plus
    its index.

    `vehicle` says the list belongs to a truck, helicopter or tank rather than
    to a guard. PlayAnimation means a different table there - the three of
    `animation_table_ptrs2[]`, played straight on the vehicle's own model - so
    it becomes the port's own command with an id out of the vehicles' own
    space.
    """
    out = bytearray()
    while at < len(d):
        op = d[at]
        ln = ai_length(d, at)
        if ln is None:
            stats['ai_unknown'] += 1
            break
        name, _, args, pd, spec, why = geaitable.TABLE[op]
        if name == 'PlayAnimation':
            anim = int.from_bytes(d[at + 1:at + 3], 'big')

            if vehicle:
                # the list belongs to a truck or an aircraft, so the id means
                # one of animation_table_ptrs2[]'s three, played straight on
                # the vehicle's model. It becomes the port's own command with
                # the id taken out of the vehicles' own space; GoldenEye's
                # bitfield has no meaning here, its own aircraft branch reading
                # nothing but the interpolation time (chrai.c).
                if anim >= len(geanimtable.VEHICLES):
                    stats['ai_dropped'][name] = stats['ai_dropped'].get(name, 0) + 1
                else:
                    out += struct.pack('>HHHHB', GEVEH_ANIM_CMD,
                                       GE_ANIM_TAG | (GEVEH_ANIM_FIRST + anim),
                                       int.from_bytes(d[at + 3:at + 5], 'big'),
                                       int.from_bytes(d[at + 5:at + 7], 'big'),
                                       d[at + 8])
                    stats['ai_kept'] += 1
                at += ln
                continue

            if anim >= len(geanimtable.TABLE):
                stats['ai_dropped'][name] = stats['ai_dropped'].get(name, 0) + 1
                at += ln
                continue
            stats['anims'].add(anim)
        if op == GE_IFBONDY_OP:
            y = struct.unpack_from('>h', d, at + 1)[0]
            if offset is not None:
                y -= int(round(float(offset[1])))
            out += struct.pack('>HiB', GE_IFBONDY_CMD, y, d[at + 3])
            stats['ai_kept'] += 1
            at += ln
            continue
        if pd is None:
            stats['ai_dropped'][name] = stats['ai_dropped'].get(name, 0) + 1
        else:
            vals, o = [], at + 1
            for a, w in args:
                v = int.from_bytes(d[o:o + w], 'big')
                if 'PAD' in a and w >= 2:
                    v = pad_num(v, numpads)
                    if op in GE_PADROOM_OPS and v != NO_PAD:
                        v += PD_PADROOM_PAD
                elif a == 'TEXT_SLOT':
                    v = text_id(v)
                elif a == 'AI_LIST_ID':
                    v = global_ai_id(v)
                elif a == 'ANIMATION_ID':
                    v = GE_ANIM_TAG | v
                elif a == 'ITEM_NUM' and (op in GE_EQUIP_OPS or op in GE_GIVE_OPS):
                    v = item_weapon(v)
                elif a == 'PROP_NUM' and op in GE_GIVE_OPS:
                    stats.setdefault('models', set()).add(v)
                    v = MODEL_REMAKE_FIRST + v
                elif a == 'HEALTH' and op in GE_BOND_HEALTH_OPS:
                    v = v * GE_BOND_HEALTH_FULL // 255
                elif a == 'CHRFLAGS' and op in GE_CHRFLAG_OPS and v & GE_CHRFLAG_LOCKY:
                    v = (v & ~GE_CHRFLAG_LOCKY) | PD_CHRFLAG_GE_LOCKY
                vals.append(v)
                o += w
            out += struct.pack('>H', pd)
            for s in spec:
                if isinstance(s, tuple) and s and s[0] == '=':
                    out += int(s[1]).to_bytes(s[2], 'big')
                elif isinstance(s, tuple) and s and s[0] == '&':
                    out += (vals[s[1]] & s[2]).to_bytes(args[s[1]][1], 'big')
                elif isinstance(s, tuple):
                    out += (vals[s[0]] & ((1 << (8 * s[1])) - 1)).to_bytes(s[1], 'big')
                else:
                    out += vals[s].to_bytes(args[s][1], 'big')
            stats['ai_kept'] += 1
        at += ln
        if name == 'EndList':
            return bytes(out)
    out += struct.pack('>H', 0x0004)   # aiEndList, whatever the walk ran into
    return bytes(out)


# The propdefs that run an AI list on a vehicle rather than on a guard: truck,
# helicopter and tank, each with its list's id where a guard record has none
# (bondtypes.h, VehichleRecord/AircraftRecord `ailist` at 0x80)
VEHICLE_TYPES = (39, 40, 45)


def vehicle_lists(d):
    """The ids of the AI lists a vehicle prop owns."""
    out = set()
    for _, t, raw in ge_records(d):
        if t in VEHICLE_TYPES and len(raw) >= 0x84:
            out.add(struct.unpack_from('>I', raw, 0x80)[0] & 0xffff)
    return out


def convert_ailists(d, at, stats, numpads, data=None, offset=None):
    """The mission's own AI lists, and GoldenEye's global ones after them.

    The table is **sorted by id and holds each id once**, which GoldenEye's own
    file is not obliged to be: its `ailistFindById` walks the rows and takes the
    first of a duplicate, while Perfect Dark's binary-searches them (lib/ailist.c)
    and can miss a list altogether - Facility carries 1063 twice and Surface has
    1051 before 1049 and 4106 twice. Keeping the first of each id and sorting is
    GoldenEye's own answer in the order Perfect Dark has to have it in.
    """
    h = struct.unpack_from('>10I', d, 0)
    if not h[5]:
        return struct.pack('>Ii', 0, 0), b''
    vehicles = vehicle_lists(d)
    rows = []
    o = h[5]
    while True:
        ptr, lid = struct.unpack_from('>Ii', d, o)
        if not ptr and not lid:
            break
        rows.append((lid, d, ptr, lid in vehicles))
        o += 8
    if data is not None:
        for lid, off in global_lists(data):
            rows.append((global_ai_id(lid), data, off, False))
    seen = set()
    kept = []
    for row in rows:
        if row[0] in seen:
            stats['ai_duplicate'] = stats.get('ai_duplicate', 0) + 1
            continue
        seen.add(row[0])
        kept.append(row)
    kept.sort(key=lambda row: row[0])
    head = 8 * (len(kept) + 1)
    pos = at + head
    table, code = b'', b''
    for lid, buf, ptr, isvehicle in kept:
        blob = convert_ailist(buf, ptr, stats, numpads, isvehicle, offset)
        table += struct.pack('>Ii', pos, lid)
        code += blob
        pos += len(blob)
    table += struct.pack('>Ii', 0, 0)
    return table, code


def convert(d, numpads, bodies, scale=None, offset=None, data=None):
    """A GoldenEye solo setup as a Perfect Dark one.

    `bodies(bodyid, headid)` gives the Perfect Dark body and head a GoldenEye
    character becomes. Returns (bytes, models used, statistics).
    """
    models = set()
    stats = dict(kept={}, dropped={}, ai_kept=0, ai_dropped={}, ai_unknown=0, anims=set())
    props = convert_props(d, numpads, bodies, models, stats, offset)
    intro, stats['spawns'] = convert_intro(d, numpads, scale, offset)
    header = 0x20
    intro_at = header
    props_at = intro_at + len(intro)
    paths_at = props_at + len(props)
    paths, pathpads = convert_paths(d, paths_at, numpads)
    ai_at = paths_at + len(paths) + len(pathpads)
    ailists, aicode = convert_ailists(d, ai_at, stats, numpads, data, offset)
    out = struct.pack('>8I', 0, 0, 0, intro_at, props_at, paths_at, ai_at, 0)
    out += intro + props + paths + pathpads + ailists + aicode
    # and the guns its lists hand out, which no record need name
    models |= stats.get('models', set())
    return out, models, stats
