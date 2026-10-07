#ifndef _IN_NET_NETPROTO_H
#define _IN_NET_NETPROTO_H

/**
 * Netplay's session messages, byte by byte (PLANS/netplay/spec-stage.md).
 *
 * Every message is written and read field by field through netbuf (netbuf.h),
 * never a struct copied whole. Notation:
 *
 *   u8/u16/u32/s8/s16/s32  little-endian, one byte at a time
 *   f32                    the IEEE bits as a u32
 *   u64                    two u32s, low word first
 *   varu32                 netbuf's LEB128 varint
 *   str(N)                 varu32 length L (L <= N), then L bytes, no NUL;
 *                          a longer string is cut to N bytes by the writer
 *   bytes(N)               N raw bytes
 *
 * Every message starts with one u8, its type. A reader that finds the
 * message short, long, or with a field past its bound drops the peer with
 * REFUSE BADMSG; nothing from the wire is trusted as an index unchecked.
 *
 * Channels (nettransport.h): CONNECT, ACCEPT, REFUSE, LOADED, GO,
 * MATCH_END and LEAVE go on NET_CHAN_RELIABLE. RULES and STAGE_LOAD go on
 * NET_CHAN_BULK, RULES first: ENet orders each channel on its own, so the
 * two must share one for STAGE_LOAD never to arrive before its RULES.
 *
 * Flow:
 *
 *   client                         host
 *   (ENet connect, data 0) ------>
 *   CONNECT ---------------------->  checks: protocol, build, region,
 *                                    geconvert, session hash, ticket,
 *                                    MUST/REFUSE keys, a free slot
 *          <---------------------- ACCEPT, or REFUSE then disconnect
 *                                  (match starts: mpStartMatch, hook H1)
 *          <---------------------- RULES, STAGE_LOAD          (BULK)
 *   loads the stage (H3..H6)
 *   LOADED ----------------------->  compares the stage hash components
 *          <---------------------- GO, or REFUSE STAGEHASH / TIMEOUT
 *   ... match ...
 *          <---------------------- MATCH_END (host's mainEndStage, H9)
 *   LEAVE (End Game, quit) ------->  or a timeout: the host drops the slot
 */

// Bumped whenever any message below changes shape or meaning
// 10 (phase 8, MAX_PLAYERS 4 -> 12): RULES carries 12 human slots and a
//    u16 humanslotshi after chrslots, ROSTER 12 seats, MATCH_END 12 award and
//    medal pairs and 92 mpchrs, the scenario block is 768 bytes with 12
//    players' tokenheld/holds (OFF_BODY 224), spectator views are 12-13,
//    every mpchr slot is players 0-11, sims 12+
// 11: a command's buttons keep START; the host plays it only for a dead
//    player (the death screen's respawn), never for a living one (the
//    client's pause menu is its own)
#define NET_PROTOCOL_VERSION 11

#define NETMSG_CONNECT    1
#define NETMSG_ACCEPT     2
#define NETMSG_REFUSE     3
#define NETMSG_RULES      4
#define NETMSG_STAGE_LOAD 5
#define NETMSG_LOADED     6
#define NETMSG_GO         7
#define NETMSG_MATCH_END  8
#define NETMSG_LEAVE      9
#define NETMSG_LOBBY      10
#define NETMSG_CMD        11
#define NETMSG_CMDACK     12
#define NETMSG_SLOTCFG    13
#define NETMSG_SNAP       14 // netsnap.c writes this number itself (NETSNAP_MSGTYPE)
#define NETMSG_EVENTS     15
#define NETMSG_ROSTER     16 // protocol 9: the match's seats, host -> client

/**
 * Refusal and leave reasons: REFUSE's and LEAVE's code byte, and the u32
 * the transport's disconnect carries. The human text beside them names the
 * component, key or file that differed.
 */
#define NETREFUSE_NONE      0
#define NETREFUSE_PROTOCOL  1  // NET_PROTOCOL_VERSION differs
#define NETREFUSE_BUILD     2  // VERSION_HASH differs
#define NETREFUSE_CONTENT   3  // a session hash component differs (named)
#define NETREFUSE_REGION    4  // the ROM's version (NTSC/PAL/JPN) differs
#define NETREFUSE_GECONVERT 5  // GECONVERT_VERSION_STR differs
#define NETREFUSE_FULL      6  // no free player slot
#define NETREFUSE_STARTED   7  // a match is loading or ending: join once it runs, or is over
#define NETREFUSE_TICKET    8  // Net.RequireTicket and no valid lobby ticket
#define NETREFUSE_MUST      9  // a MUST key differs (named)
#define NETREFUSE_NOTSTOCK  10 // a REFUSE key is not stock on one side (named)
#define NETREFUSE_STAGEHASH 11 // a LOADED component differs (named)
#define NETREFUSE_NOSTAGE   12 // the stage key does not resolve here (client's LEAVE)
#define NETREFUSE_TIMEOUT   13 // not LOADED within NET_LOAD_TIMEOUT_MS
#define NETREFUSE_BADMSG    14 // a message that does not parse
#define NETREFUSE_SHUTDOWN  15 // the host is going away
#define NETREFUSE_LEFT      16 // the player left (End Game, quit)
#define NETREFUSE_COUNT     17

#define NET_DEFAULT_PORT     27100
#define NET_LOAD_TIMEOUT_MS  15000 // H7: the host waits this long for LOADED
#define NET_CONNECT_TIMEOUT_MS 10000
#define NET_CONNECT_WINDOW_MS  30000 // a client tries to connect this long (a host may still be booting)

// String bounds on the wire
#define NET_MAXNAME      31  // a player's name (an account is 3-15; the game shows 14)
#define NET_MAXBUILD     40  // VERSION_HASH
#define NET_MAXCOMPNAME  15  // a hash component's name
#define NET_MAXKEY       47  // an ini key
#define NET_MAXSTRVAL    63  // an ini string value
#define NET_MAXTEXT      255 // a refusal's human text
#define NET_MAXMAPDIR    127 // a mod dir's basename in a stage key
#define NET_MAXMAPNAME   31  // a map's name (modloader.c, char[32])
#define NET_MAXTICKET    133 // TICKET_MAX in tools/pdlobbyd
#define NET_MAXCOMPS     12  // hash components in one message
#define NET_MAXKEYS      48  // ini keys in one message

/**
 * CONNECT (client -> host)
 *   u8      NETMSG_CONNECT
 *   u16     protocol          NET_PROTOCOL_VERSION
 *   str(40) build             VERSION_HASH
 *   str(15) geconvert         GECONVERT_VERSION_STR
 *   u8      romversion        VERSION (the ROM's region and revision)
 *   u8      ncomps            <= NET_MAXCOMPS; session hash components:
 *     str(15) name              "rom", "mod", "mapmods", "borrow", "added", "geconv", "all"
 *     u64     hash              the first 8 bytes of the component's SHA-256
 *                               (protocol 8: "mapmods", the Stage Loader's
 *                               other mods, is compared and logged, never
 *                               refused: a map the client lacks is refused
 *                               at STAGE_LOAD, NOSTAGE, with the map and its
 *                               mod named, and the stage hash checks the one
 *                               played; "all", the lobby's, leaves it out)
 *   str(31) name              Net.Name, or the agent's name
 *   per-slot settings (spec-players.md §2):
 *     u8 mpheadnum, u8 mpbodynum   g_PlayerConfigsArray[0].base
 *     u8 controlmode, u16 options  g_PlayerConfigsArray[0]
 *     f32 fovy, f32 fovzoommult, s32 fovzoom, s32 mouseaimmode,
 *     f32 mouseaimspeedx, f32 mouseaimspeedy, s32 crouchmode,
 *     f32 radialmenuspeed, f32 crosshairsway, s32 extcontrols,
 *     u32 crosshaircolour, u32 crosshairsize, f32 crosshairedgeboundary,
 *     s32 crosshairhealth, s32 usereloads      g_PlayerExtCfg[0], in that order
 *     f32 aspect                   videoGetAspect()
 *     s8 sensxsign, s8 sensysign   the signs of the mouse speeds (-1, 0, 1)
 *     u8 aimlock                   modIsCodAimLockOn()
 *     u8 akimbotriggers            Mod.AkimboTriggers
 *   u8      nkeys             <= NET_MAXKEYS; this machine's MUST and REFUSE
 *                             keys (netrules.c's table), each a value:
 *     str(47) key
 *     value                   see VALUE below
 *   u16     ticketlen         0, or <= NET_MAXTICKET
 *   bytes   ticket            the lobby's join ticket, ASCII (README.md)
 *   u8      flags             (protocol 9) NETCONN_SPECTATE: a spectator,
 *                             never a player's seat
 *
 * VALUE
 *   u8 type                   CONFIG_TYPE_S32/F32/U32/STR (config.h)
 *   s32 | f32 | u32 | str(63)
 *
 * ACCEPT (host -> client)
 *   u8      NETMSG_ACCEPT
 *   u8      slot              the client's mpindex slot (0-11), or
 *                             NETSLOT_SPECTATOR for a spectator
 *   u32     hosttick          the host's g_NetTick now
 *   u8      dedicated         1 if the host has no player of its own
 *   str(31) hostname          the host's Net.Name
 *   u8      flags             (protocol 9) NETACC_INPROGRESS: a match is
 *                             running; its RULES and STAGE_LOAD follow at
 *                             once. NETACC_RESUMED: the seat is the one
 *                             this account held when it dropped (its score
 *                             kept). NETACC_SPECTATOR: a spectator.
 *
 * Join in progress (protocol 9). A CONNECT while a match runs (after its
 * GO, before its MATCH_END; else REFUSE STARTED, which a lobby member
 * tries again on while its room stays launched) is given a seat of the
 * match: the one its account held, else an open one no other client keeps
 * as its slot (Net.JoinInProgress 1, and every lobby room, has the host
 * start each match with all four seats, the ones nobody took out of play:
 * dead and hidden, never respawned, passed over as Pop a Cap's victim),
 * else REFUSE FULL. The account is a lobby ticket's user: only a ticketed
 * seat is held when it drops, Net.ReconnectHold seconds (30) from its
 * game's last message, and only a ticket for that user takes it back (a
 * seat without one opens when it drops). A seat that opens mid-match drops
 * what its player carries, and its scenario counts go with its row. A spectator takes no
 * seat (at most NET_MAXSPECS). The joiner then loads as at a match's start:
 * RULES, STAGE_LOAD (its seat's player, or NETSLOT_SPECTATOR), LOADED
 * checked against the host's own stage hash, and GO with the host's tick
 * now; its first EVENTS message holds one SCORES event, the match's table
 * as it stands, and its first snapshot is a keyframe. A seat that comes
 * back to a player has its player respawned (at the next respawn point)
 * if it was out of play. A seat whose hold runs out is opened: its player
 * goes out of play and its row of the table is cleared (SCORES to all).
 *
 * REFUSE (host -> client; the host disconnects after it)
 *   u8      NETMSG_REFUSE
 *   u8      code              NETREFUSE_*
 *   str(47) component         the component or ini key that differed, or ""
 *   str(255) text             what to tell the player
 *
 * LEAVE (either way; the sender disconnects after it)
 *   u8      NETMSG_LEAVE
 *   u8      code              NETREFUSE_*
 *   str(255) text
 *
 * RULES (host -> client, BULK) - the match state (spec-stage.md §3a) then
 * the SYNC ini values (§3b):
 *   u8      NETMSG_RULES
 *   u32     matchid
 *   g_MpSetup:
 *     str(17) name, u32 options, u8 scenario, u8 timelimit, u8 scorelimit,
 *     u16 teamscorelimit, u16 chrslots, u8 weapons[6]
 *   u16     humanslotshi          (protocol 10) human slots 4-11, bit k =
 *                                 slot 4+k (chrslots keeps bits 0-3 humans,
 *                                 4-11 sims: the ROM/save layout)
 *   u32     scenario save bits    scenarioWriteSave()'s 32
 *   s32     weaponsetnum          g_MpWeaponSetNum
 *   u64     randomfilters         g_MpWeaponSetRandomFilters, one bit each
 *   u8      simslots[80]          g_MpSimSlots
 *   u8      nsims                 sims that follow (slots with simslots[i])
 *     u8 index, u8 type, u8 difficulty, u8 mpheadnum, u8 mpbodynum, u8 team,
 *     u32 displayoptions, str(14) name, s8 stats[5]
 *   u8      difficulties[80][4]   g_MpSimulantDifficultiesPerNumPlayers (pinned at 4)
 *   12 x human slot (g_PlayerConfigsArray[0-11]; protocol 10, was 4):
 *     str(14) name, u8 mpheadnum, u8 mpbodynum, u8 team, u32 displayoptions,
 *     u8 handicap, u16 options, u8 gunfuncs[6]
 *     (never fileguid or career stats)
 *   8 x str(11) teamnames         g_BossFile.teamnames
 *   u8      unlocked[80]          g_MpFeaturesUnlocked
 *   u32     modunlocks            g_ModUnlocks
 *   u8      gexplusmode           g_GexPlusMode
 *   u8      gexplusscenario       gexPlusGetScenario()
 *   u8      endless               g_MpEndlessMatch
 *   s32     maxexplosions         Game.MaxExplosions (a client keeps the larger)
 *   u8      nkeys                 <= NET_MAXKEYS SYNC ini keys
 *     str(47) key, VALUE
 *   str(15) tickratediv           unused, ""
 *
 * STAGE_LOAD (host -> client, BULK, after its RULES)
 *   u8      NETMSG_STAGE_LOAD
 *   u32     matchid
 *   stage key (spec-stage.md §4):
 *     u8 kind                     0 stock, 1 mod or Stage Loader map, 2 overlay arena
 *     kind 0: u8 id
 *     kind 1: str(127) moddir basename, str(31) map name
 *     kind 2: str(127) overlay mod dir basename, u8 id
 *   str(31) label                 the arena's name, for the lobby and logs
 *   u64     seed                  rngSetSeed at H4
 *   u64     seed2                 rng2SetSeed at H4
 *   u8      numplayers            humans in the match (chrslots bits 0-3
 *                                 and humanslotshi)
 *   u8      yourplayer            this client's player number in the match;
 *                                 NETSLOT_SPECTATOR: none, a spectator
 *                                 (protocol 9; it watches through player 0)
 *
 * LOADED (client -> host)
 *   u8      NETMSG_LOADED
 *   u32     matchid
 *   u8      ncomps                <= NET_MAXCOMPS; the stage hash components:
 *     str(15) name                "setup", "pads", "tiles", "bg", "xblatiles",
 *                                 "stan", "models", "rng"
 *     u64     hash
 *   ("models" is diagnostic only and never refused: model packs and XBLA
 *   meshes may differ; "rng" is g_RngSeed right after lvReset)
 *
 * GO (host -> client)
 *   u8      NETMSG_GO
 *   u32     matchid
 *   u32     hosttick              g_NetTick the host starts from: 0, or for
 *                                 a join in progress the tick the host runs
 *                                 next (the client starts a little past it)
 *   s32     stagetime60           (protocol 9) the match's clock then
 *                                 (g_StageTimeElapsed60): the time limit
 *                                 and the HUD's clock read it
 *
 * ROSTER (host -> client, RELIABLE; protocol 9) - the match's seats, on GO
 * of a join in progress and to everyone whenever one changes hands:
 *   u8      NETMSG_ROSTER
 *   u32     matchid
 *   u8      nseats                MAX_PLAYERS (12; protocol 10, was 4)
 *   per seat (mpindex 0-11):
 *     u8 state                    0 not in the match, 1 the host's, 2 a
 *                                 client's, 3 open, 4 held for a dropped one
 *     str(14) name                the player's name (g_PlayerConfigsArray)
 *
 * MATCH_END (host -> client; also to a client still loading or at the
 * barrier, which then runs on to its end screen without a GO)
 *   u8      NETMSG_MATCH_END
 *   u32     matchid
 *   u8      numplayers
 *   per player (g_Vars.players[i]):
 *     u8 award1, u8 award2        index into g_AwardNames, 0xff for none
 *   per human slot (g_PlayerConfigsArray[0-11]): u8 medals, u8 title
 *   u8      nchrs                 MAX_MPCHRS (92; protocol 10, was 84)
 *   per mpchr slot (MPCHR(i)):
 *     s8 placement, s32 rankablescore, s16 numdeaths, s16 numpoints,
 *     s16 killcounts[MAX_MPCHRS]
 *   u16     scenlen               NETSCEN_SIZE (protocol 7)
 *   bytes   scenario              the host's scenario block at the end (the
 *                                 SNAP block below), applied with the table
 *
 * LOBBY (client -> host) - the client's match stage has stopped (its H12,
 * past its end screen) and it is back in the menus. The host sends RULES and
 * STAGE_LOAD only to clients that are in the lobby; one still on the last
 * match's end screen when the host starts the next sits that match out.
 *   u8      NETMSG_LOBBY
 *   u32     matchid               the match it has left
 *
 * A LEAVE from a client frees its slot at once (the host disconnects it and
 * the load barrier stops waiting for it).
 *
 * SLOTCFG (client -> host, RELIABLE) - the per-slot settings again, sent on
 * ACCEPT and on every LOBBY (CONNECT's go out before the window is up, so
 * its aspect is no use; and the player may change them between matches).
 * The host takes the last one it has at H1, and one mid-match at once.
 *   u8      NETMSG_SLOTCFG
 *   per-slot settings             exactly CONNECT's block
 *
 * CMD (client -> host, UNRELIABLE, once a tick from GO) - the client's
 * usercmds (spec-players.md §2): every one the host has not acked, newest
 * last, at most NET_MAXCMDSEND (the oldest are dropped past that; the host
 * then skips them). A command's tick is the client's g_NetTick it was
 * captured on; the host plays them in tick order, one per host tick.
 *   u8      NETMSG_CMD
 *   u32     matchid
 *   snapshot ack (netsnap.h, reliable.io's scheme):
 *     u16 snapack                 the newest SNAP decoded, 0 none yet
 *     u32 ackbits                 bit i: snapack-1-i decoded as well
 *     u8 ackflags                 0x01 WANTKEY: a baseline the host named
 *                                 is gone here; send keyframes
 *     u8 nnack                    <= NETSNAP_MAXNACK (8)
 *     u16 nack[nnack]             entity ids whose descriptor this client
 *                                 needs again (its local prop went stale)
 *   u32     first                 the first command's tick
 *   u8      count                 1..NET_MAXCMDSEND
 *   per command, ticks first, first+1, ...:
 *     u32 buttons                 OSContPad.button (protocol 11: START too;
 *                                 the host clears it unless the player is
 *                                 dead, where it respawns as offline)
 *     s8 stick_x, s8 stick_y, s8 rstick_x, s8 rstick_y
 *     f32 mdx, f32 mdy            inputMouseGetScaledDelta: the client's own
 *                                 sensitivity is in it, 0 when unlocked
 *     u8 flags                    NETCMD_MOUSELOCKED
 *     u32 viewtick                (protocol 6) the host tick this client's
 *                                 puppets were drawn at on this command's
 *                                 tick (its render clock), 0xffffffff none
 *                                 yet: the host's lag-compensated hits
 *                                 rewind the others to it (netlagcomp.c;
 *                                 never past now, at most Net.LagCompMaxMs
 *                                 plus the interpolation delay below)
 *     u8 viewfrac                 ... and the fraction past it, 1/256ths
 *     u8 viewdelay                the render clock's delay behind the newest
 *                                 snapshot then, 1/8 ticks (capped at 19
 *                                 ticks on the host)
 *
 * A spectator sends CMD too (protocol 9): the host takes its snapshot ack
 * and nothing else (its commands drive nothing), and its CMDACK names the
 * newest it has, with as depth how far that is ahead of the host's tick.
 *
 * CMDACK (host -> client, UNRELIABLE, once a host tick) - what the host has
 * of this client's commands, and the clock (spec-tick.md §3b)
 *   u8      NETMSG_CMDACK
 *   u32     matchid
 *   u32     hosttick              the host's g_NetTick
 *   u32     ack                   every command up to this tick has arrived
 *                                 (0xffffffff: none yet)
 *   u8      depth                 commands arrived and not yet played, after
 *                                 this tick's: the client trims its clock
 *                                 to keep it at 1-2 (Overwatch time dilation)
 *
 * SNAP (host -> client, UNRELIABLE sequenced, every 2 host ticks, 3 while
 * snapshots are lost or the cap leaves changes behind; at most
 * NET_MAXUNRELIABLE bytes) - the world as this client may see it
 * (PLANS/netplay/spec-entities.md §7; netsnap.c builds and checks it)
 *   u8      NETMSG_SNAP
 *   u32     matchid
 *   u16     seq                   per client, wraps, never 0
 *   u16     baseline              the snapshot every delta below is against:
 *                                 the newest this client acked; 0 = keyframe
 *   u32     hosttick              the host's g_NetTick the state is from
 *   u8      lvupdate240           the host's timescale that tick
 *   u8      rate                  host ticks per snapshot now (2 or 3)
 *   u32     lastcmd               this client's last command played (0xffffffff none)
 *   u16     maxids                entity ids run 0..maxids-1 (<= 8192)
 *   delta   presence              a bit per id present, XOR'd against the
 *                                 baseline's presence (netdelta.h encoding,
 *                                 (maxids+7)/8 bytes)
 *   delta   updated               a bit per id whose record follows (against zeros)
 *   varu32  ndescs                spawn descriptors, ids rising:
 *     varu32 gap                  id - previous id - 1 (previous starts at -1)
 *     u8 kind<<4 | rec            NETDESC_*, NETREC_*
 *     u16 gen                     the host prop slot's generation (never 0)
 *     SETUPOBJ:  u16 setup command index, u8 objtype
 *     SIM:       u8 bot config slot, s16 bodynum, s16 headnum
 *     PLAYER:    u8 mpindex, s16 bodynum, s16 headnum
 *     BODY:      s16 bodynum, s16 headnum (a Mod.Bodies corpse: the dead
 *                sim's host prop, handed to a chr of its own; the client
 *                builds one from the body and head a chr there wears)
 *     DYNWEAPON: u8 weaponnum, u8 gunfunc, s16 modelnum, u8 objtype
 *     SCENOBJ:   (protocol 7; kind 9, OBJ records only) a scenario's prop,
 *                a briefcase, the uplink or a terminal: s16 modelnum,
 *                u8 objtype, u8 weaponnum, u16 extrascale (1-4096, 256 = 1),
 *                u8 team (0-3, a Capture the Case briefcase's), u8 flags
 *                (1 a Hacker Central terminal). Always in scope.
 *     others:    s16 modelnum, u8 objtype
 *   records, for each updated id rising: its record (CHR 48, OBJ 24, DOOR
 *     6, LIFT 14 bytes; layouts in netsnap.c netRecPack) XOR'd against the
 *     baseline's, or against zeros when the baseline lacks the id or holds
 *     another generation, rec or kind there (then a descriptor came too)
 *     (CHR byte 46 counts the chr's teleports, wrapping: snap when it
 *     changes, by any amount; a toggled bit would lose two in one gap)
 *   u8      haslp                 0 none, 1 keyframe, 2 against the baseline's
 *   delta   localplayer           NETLP_SIZE (688) bytes, full precision:
 *     u8 flags (1 dead, 2 invincible), u8 respawns, u8 teleports (counters),
 *     u8 dualwielding, f32 pos[3], s16 rooms[8], f32 theta, f32 verta,
 *     f32 health, f32 shield, s16 weaponnum, s16 0, s32 loadedammo[4]
 *     (right 0, 1, left 0, 1), u16 ammoheld[33], u16 0, u8 weapons[32]
 *     (a bit per weapon number), u8 dualweapons[32], u8 0[8],
 *     then the movement state (protocol 5; struct netmove, netsnap.h), the
 *     player after command lastcmd, from which the client replays the
 *     commands after it (netpredict.c): f32 speedtheta, speedverta,
 *     speedthetacontrol, speedsideways, speedstrafe, speedforwards,
 *     speedboost, speedgo, s32 speedmaxtime60, f32 bondshotspeed[3],
 *     moveinitspeed[3], bondforcespeed[3], rollspeed[3], s32 rolltime60,
 *     f32 bdeltapos.y, sumground, vv_manground, vv_ground, bondonground,
 *     s32 fallage (lvframe60 - fallstart), f32 crouchoffset, crouchspeed,
 *     crouchheight, crouchfall, sumcrouch, crouchoffsetsmall, s32
 *     crouchtime240, crouchoffsetreal, crouchoffsetrealsmall, f32
 *     swaytarget, swayoffset0, swayoffset2, laddernormal[3], ladderupdown,
 *     liftground, vv_height, vv_eyeheight, gunspeed, bondbreathing,
 *     headpossum[3], s32 headwalkingtime60, s16 floorroom, u16 floorflags,
 *     u8 isfalling, onladder, inlift, bondmovemode, s8 crouchpos,
 *     autocrouchpos, headanim, u8 floortype (224 bytes); the head bob's
 *     animation (struct anim, player->unk01c0): s16 animnum, animnum2, s8
 *     flip, flip2, looping, average, s16 framea, frameb, frame2a, frame2b,
 *     f32 frame, frac, endframe, speed, newspeed, oldspeed, timespeed,
 *     elapsespeed, frame2, frac2, endframe2, speed2, newspeed2, oldspeed2,
 *     timespeed2, elapsespeed2, fracmerge, timemerge, elapsemerge,
 *     loopframe, loopmerge, playspeed, newplay, oldplay, timeplay,
 *     elapseplay, animscale (124 bytes); u32 headsave[30], the head model's
 *     rwdata (bondheadsave, 120 bytes); the aim: f32 swivelpos[2], u8
 *     insightaimmode, u8 0[3] (12 bytes). NETMOVE_SIZE 480 in all, at
 *     bytes 208..687
 *   u8      hasscen               (protocol 7) 0 none, 1 keyframe, 2 against
 *                                 the baseline's
 *   delta   scenario              NETSCEN_SIZE (768; protocol 10, was 640)
 *     bytes, the match's scenario state, the same for every client
 *     (netscen.c), every index an mpchr config slot (players 0-11, sims
 *     12+); REF5 is u8 kind (0 none,
 *     1 a chr: u8 slot, 2 an entity: u16 id, u16 gen) and 4 bytes:
 *       0  u8 scenario (0xff outside a match), u8 0[3]
 *       4  s16 numpoints[92]       MPCHR(slot)->numpoints
 *     188  u16 tokenheld[12]       each player's tokenheldtime
 *     212  u8 holds[12]            each player's inventory: 1 briefcase, 2 uplink
 *     224  Hold the Briefcase: REF5 token, f32 pos[3]
 *          Hacker Central: s8 download slot, s8 in-range slot, s8 terminal,
 *            u8 0, REF5 uplink, REF5 terminal, u8 terminal team, u8 0,
 *            s16 downloads[92], u16 dltime240[92]
 *          Pop a Cap: s16 victimindex, u16 age240, u8 nvictims,
 *            u8 victims[92] (slots, 0xff none), s16 caps[92], s16 survivals[92]
 *          King of the Hill: s16 occupiedteam, elapsed240, movehill,
 *            hillindex, hillcount, s16 hillrooms[2], f32 hillpos[3],
 *            f32 colourfrac[3]
 *          Capture the Case: s16 teamindexes[4], s16 playercounts[4],
 *            s16 baserooms[4], REF5 tokens[4]
 *   u32     evseq                 (protocol 7, only with hasscen != 0) the
 *                                 EVENTS messages the host had sent this
 *                                 client in the match before this block
 *     The client applies the block of host tick T once its render clock
 *     reaches T and it has received evseq EVENTS messages (a lost one sent
 *     again; at most 120 ticks' wait), after every event of T and before
 *     any later one
 *
 * Present and not updated: the client copies the baseline's record (it is
 * unchanged, or changed and left by the 1100-byte cap for a later packet).
 * Present in this snapshot and not in the baseline: always a descriptor and
 * a record; one that does not fit is not present at all yet. A client that
 * lacks the named baseline drops the packet and sets WANTKEY. A delta's
 * baseline is never more than 64 behind its seq (malformed otherwise); a
 * client that finds NETSNAP_RESYNC snapshots in a row older than its newest
 * (a corrupt seq got in) forgets its baselines and sets WANTKEY.
 *
 * EVENTS (host -> client, RELIABLE, at the end of a host tick that had any
 * for this client, before that tick's SNAP; and before MATCH_END) - what
 * happened in the tick (PLANS/netplay/spec-entities.md §5; netevents.c).
 * The client applies a tick's events once its render clock reaches the
 * tick, after the snapshot records it poses for it, in the order sent.
 *   u8      NETMSG_EVENTS
 *   u32     matchid
 *   u32     hosttick              the host's g_NetTick they happened on
 *   u16     count                 events that follow (a tick's events may
 *                                 take several messages of <= 16 KB)
 *   per event:
 *     varu32 len                  the event's bytes, its type included
 *     u8     type, then its fields:
 *   REF  u8 kind: 0 none; 1 player: u8 playernum (the same slots on every
 *        machine); 2 entity: u16 id, u16 gen (the host prop and generation);
 *        3 (protocol 8) a setup object: u16 id, u16 gen, u16 its setup
 *        command index, by which a client that never had it in scope finds
 *        its own (glass broken across the map); a Mod.Bodies corpse or a
 *        dropped gun resolves to the prop the client made for the entity
 *   POS  f32 x, y, z (finite, |v| < 2^20)
 *   DIR  s16 x, y, z at 1/8192
 *   1 FIRESLOT   (E1, chrUpdateFireslot)  REF chr, u8 hand | 2 sound | 4 beam,
 *                POS from, POS to
 *   2 PLAYERSHOT (E2, handTickAttack)     u8 playernum, u8 hand | 4 beam,
 *                POS from (the body's muzzle), POS to (the hit), u32 the
 *                shooter's command tick the shot came from (protocol 6; the
 *                host's own player's: the host tick). The shooter's own
 *                machine gets it too and drops it when it fired on that
 *                command itself (its flash, tracer and impact were drawn
 *                then); one it did not fire is counted, not drawn
 *   3 EXPLOSION  (E3, explosionCreate)    REF source, POS, u8 nrooms (1-8),
 *                s16 rooms[nrooms], s16 type, s8 playernum, u8 1 scorch |
 *                2 arg6 | 4 arg8, [POS arg6], s16 room, [POS arg8]: the
 *                scorch's place, room and normal, sent with a scorch only
 *                (both or neither; room 0 without); never a bullet hole's flame
 *   4 SPARKS     (sparksCreate)           s16 room, REF prop, POS, u8 type,
 *                u8 1 dir | 2 normal, [DIR], [DIR]; never a pad's or the
 *                rain's
 *   5 CHRDAMAGE  (E4, chrDamage)          REF victim, REF attacker, s8 hitpart,
 *                u8 1 shield hit | 2 explosion | 4 shield up
 *   6 CHOKE      (E4, chrChoke)           REF chr, s8 choketype (0-8)
 *   7 DEFORM     (E5, objDeform)          REF obj, s16 level
 *   8 GLASS      (E5, glassDestroy)       REF obj
 *   9 DEATH      (E6, mpstatsRecordDeath) s8 attacker, s8 victim (mpchr slots:
 *                players 0-11, sims 12+, -1 none), s8 attacker given (-1 none,
 *                -2 outside the match), s8 victim given (the same)
 *  10 HUDMSG     (E7, hudmsgCreateFromArgs; only to the player's machine)
 *                str(255) text, u8 type (<= 11), s32 conf00, conf01, conf02,
 *                u32 textcolour, glowcolour, alignh, s32 conf16, u32 alignv,
 *                s32 conf18, s32 duration, u32 flags (fonts: the type's)
 *  11 PICKUPSFX  (E8, objPlayPickupSfx; only to the player's machine) s16 sound
 *  12 NBOMB      (E9, nbombCreateStorm)   POS, REF owner
 *  13 GAS        (E9, gasReleaseFromPos)  POS
 *  14 SCORES     (protocol 9) the match's kill table as it stands before
 *                the tick: u8 flags (1 a joiner's catch-up), u8 n, n x
 *                { u8 mpchr slot, s16 numdeaths, s16 numpoints, u8 nk,
 *                nk x { u8 j, s16 killcounts[j] } } (rows and counts left
 *                out are 0); the client's table is replaced by it
 * Each event must parse to exactly its len; an unknown type or a field past
 * its bound drops that event (counted), never the message's others.
 * The events the pass of a remote player made on the host that its machine
 * made itself (its shots' sparks and flames) are not sent to it; its
 * PLAYERSHOTs are, for it to drop (above).
 */

#define NET_MAXCMDSEND    16
#define NETCMD_MOUSELOCKED 0x01

#define NETCONN_SPECTATE   0x01 // CONNECT's flags
#define NETACC_INPROGRESS  0x01 // ACCEPT's flags
#define NETACC_RESUMED     0x02
#define NETACC_SPECTATOR   0x04
#define NETSLOT_SPECTATOR  0xff // ACCEPT's slot, STAGE_LOAD's yourplayer
#define NET_MAXSPECS       2    // spectators a host takes

#endif
