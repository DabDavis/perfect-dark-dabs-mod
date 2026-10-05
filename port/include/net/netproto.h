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
#define NET_PROTOCOL_VERSION 4

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
#define NETREFUSE_STARTED   7  // a match is loading or running (join-in-progress is phase 7)
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
 *     str(15) name              "rom", "mod", "mapmods", "added", "geconv", "all"
 *     u64     hash              the first 8 bytes of the component's SHA-256
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
 *
 * VALUE
 *   u8 type                   CONFIG_TYPE_S32/F32/U32/STR (config.h)
 *   s32 | f32 | u32 | str(63)
 *
 * ACCEPT (host -> client)
 *   u8      NETMSG_ACCEPT
 *   u8      slot              the client's mpindex slot (0-3)
 *   u32     hosttick          the host's g_NetTick now
 *   u8      dedicated         1 if the host has no player of its own
 *   str(31) hostname          the host's Net.Name
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
 *   u32     scenario save bits    scenarioWriteSave()'s 32
 *   s32     weaponsetnum          g_MpWeaponSetNum
 *   u64     randomfilters         g_MpWeaponSetRandomFilters, one bit each
 *   u8      simslots[80]          g_MpSimSlots
 *   u8      nsims                 sims that follow (slots with simslots[i])
 *     u8 index, u8 type, u8 difficulty, u8 mpheadnum, u8 mpbodynum, u8 team,
 *     u32 displayoptions, str(14) name, s8 stats[5]
 *   u8      difficulties[80][4]   g_MpSimulantDifficultiesPerNumPlayers
 *   4 x human slot (g_PlayerConfigsArray[0-3]):
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
 *   u8      numplayers            humans in the match (chrslots bits 0-3)
 *   u8      yourplayer            this client's player number in the match
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
 *   u32     hosttick              g_NetTick the host starts from (0)
 *
 * MATCH_END (host -> client; also to a client still loading or at the
 * barrier, which then runs on to its end screen without a GO)
 *   u8      NETMSG_MATCH_END
 *   u32     matchid
 *   u8      numplayers
 *   per player (g_Vars.players[i]):
 *     u8 award1, u8 award2        index into g_AwardNames, 0xff for none
 *   per human slot (g_PlayerConfigsArray[0-3]): u8 medals, u8 title
 *   u8      nchrs                 MAX_MPCHRS
 *   per mpchr slot (MPCHR(i)):
 *     s8 placement, s32 rankablescore, s16 numdeaths, s16 numpoints,
 *     s16 killcounts[MAX_MPCHRS]
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
 *     u32 buttons                 OSContPad.button, START already cleared
 *     s8 stick_x, s8 stick_y, s8 rstick_x, s8 rstick_y
 *     f32 mdx, f32 mdy            inputMouseGetScaledDelta: the client's own
 *                                 sensitivity is in it, 0 when unlocked
 *     u8 flags                    NETCMD_MOUSELOCKED
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
 *     BODY:      s16 bodynum, s16 headnum
 *     DYNWEAPON: u8 weaponnum, u8 gunfunc, s16 modelnum, u8 objtype
 *     others:    s16 modelnum, u8 objtype
 *   records, for each updated id rising: its record (CHR 48, OBJ 24, DOOR
 *     6, LIFT 14 bytes; layouts in netsnap.c netRecPack) XOR'd against the
 *     baseline's, or against zeros when the baseline lacks the id or holds
 *     another generation, rec or kind there (then a descriptor came too)
 *     (CHR byte 46 counts the chr's teleports, wrapping: snap when it
 *     changes, by any amount; a toggled bit would lose two in one gap)
 *   u8      haslp                 0 none, 1 keyframe, 2 against the baseline's
 *   delta   localplayer           NETLP_SIZE (208) bytes, full precision:
 *     u8 flags (1 dead, 2 invincible), u8 respawns, u8 teleports (counters),
 *     u8 dualwielding, f32 pos[3], s16 rooms[8], f32 theta, f32 verta,
 *     f32 health, f32 shield, s16 weaponnum, s16 0, s32 loadedammo[4]
 *     (right 0, 1, left 0, 1), u16 ammoheld[33], u16 0, u8 weapons[32]
 *     (a bit per weapon number), u8 dualweapons[32], u8 0[8]
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
 *        machine); 2 entity: u16 id, u16 gen (the host prop and generation)
 *   POS  f32 x, y, z (finite, |v| < 2^20)
 *   DIR  s16 x, y, z at 1/8192
 *   1 FIRESLOT   (E1, chrUpdateFireslot)  REF chr, u8 hand | 2 sound | 4 beam,
 *                POS from, POS to
 *   2 PLAYERSHOT (E2, handTickAttack)     u8 playernum, u8 hand | 4 beam,
 *                POS from (the body's muzzle), POS to (the hit); never to
 *                the shooter's own machine
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
 *                players 0-3, sims 4+, -1 none), s8 attacker given (-1 none,
 *                -2 outside the match), s8 victim given (the same)
 *  10 HUDMSG     (E7, hudmsgCreateFromArgs; only to the player's machine)
 *                str(255) text, u8 type (<= 11), s32 conf00, conf01, conf02,
 *                u32 textcolour, glowcolour, alignh, s32 conf16, u32 alignv,
 *                s32 conf18, s32 duration, u32 flags (fonts: the type's)
 *  11 PICKUPSFX  (E8, objPlayPickupSfx; only to the player's machine) s16 sound
 *  12 NBOMB      (E9, nbombCreateStorm)   POS, REF owner
 *  13 GAS        (E9, gasReleaseFromPos)  POS
 * Each event must parse to exactly its len; an unknown type or a field past
 * its bound drops that event (counted), never the message's others.
 * The events the pass of a remote player made on the host that its machine
 * made itself (its shots, their sparks and flames) are not sent to it.
 */

#define NET_MAXCMDSEND    16
#define NETCMD_MOUSELOCKED 0x01

#endif
