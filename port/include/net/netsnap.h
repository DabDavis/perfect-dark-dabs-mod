#ifndef _IN_NET_NETSNAP_H
#define _IN_NET_NETSNAP_H

#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netdelta.h"

/**
 * Snapshots on the wire (PLANS/netplay/spec-entities.md §1, §6, §7; the
 * message layout is in netproto.h, SNAP and CMD's ack block).
 *
 * This file is the game-free half: fixed quantized records, spawn
 * descriptors, the local-player block, and the encoder (host, one per
 * client) and decoder (client) that delta every record against the
 * snapshot the client last acked through netdelta's baseline ring. The game
 * half (netents.c) turns props into these and back. Nothing here includes
 * types.h or ENet, so tools/nettest builds it on its own and can throw
 * garbage at it.
 *
 * One baseline per packet, Quake 3's way: snapshot S names B, the newest
 * snapshot the client acked. Every entity present in S is either
 *   - updated: its record follows, XOR'd against its record in B (a
 *     keyframe, against zeros, when B lacks it or holds another generation),
 *   - or carried: unchanged since B, or changed and left for a later packet
 *     by the priority accumulator; the client copies B's record.
 * An entity in S but not in B always has its spawn descriptor in S, and an
 * entity whose descriptor and record did not fit is left out of S entirely
 * (it does not exist for the client yet), so presence never lies.
 */

// Record kinds
#define NETREC_NONE  0
#define NETREC_CHR   1
#define NETREC_OBJ   2
#define NETREC_DOOR  3
#define NETREC_LIFT  4
#define NETREC_COUNT 5

#define NETREC_CHRSIZE  48
#define NETREC_OBJSIZE  24
#define NETREC_DOORSIZE 6
#define NETREC_LIFTSIZE 14
#define NETREC_MAX      48

s32 netRecSize(s32 rec); // 0 for a kind that is not one

// Spawn descriptor kinds
#define NETDESC_SETUPOBJ  1 // key: setup command index
#define NETDESC_SIM       2 // key: bot config slot (aibot->config - g_BotConfigsArray)
#define NETDESC_PLAYER    3 // key: mpindex
#define NETDESC_BODY      4 // a chr that is neither (a corpse left behind)
#define NETDESC_DYNWEAPON 5
#define NETDESC_DYNOBJ    6
#define NETDESC_AMMOCRATE 7
#define NETDESC_HAT       8
#define NETDESC_SCENOBJ   9 // a scenario's prop: a briefcase, the uplink, a terminal (protocol 7)
#define NETDESC_COUNT     10

// SCENOBJ's scenflags
#define NETSCENOBJ_TERMINAL 0x01 // Hacker Central's terminal (OBJFLAG3_HTMTERMINAL)

struct netdesc {
	u8 kind;      // NETDESC_*
	u8 rec;       // NETREC_*
	u16 gen;      // the prop slot's generation on the host
	u16 key;
	s16 modelnum;
	u8 objtype;
	u8 weaponnum;
	u8 gunfunc;
	s16 bodynum;
	s16 headnum;
	u16 extrascale; // SCENOBJ: the object's extrascale (256 = 1)
	u8 team;        // SCENOBJ: a Capture the Case briefcase's team
	u8 scenflags;   // SCENOBJ: NETSCENOBJ_*
};

void netDescWrite(struct netbuf *b, const struct netdesc *d);
void netDescRead(struct netbuf *b, struct netdesc *d); // a bad kind or rec sets the error

/**
 * What both rings store per entity: [0..1] gen (LE), [2] rec, [3] desc kind,
 * [4..] the record (its kind's size; the rest zero)
 */
#define NETSNAP_STOREHDR 4
#define NETSNAP_STORE    (NETSNAP_STOREHDR + NETREC_MAX)

static inline u16 netStoreGen(const u8 *s) { return (u16)(s[0] | (s[1] << 8)); }
static inline s32 netStoreRec(const u8 *s) { return s[2]; }
static inline s32 netStoreKind(const u8 *s) { return s[3]; }

// Flag bits: CHR record bytes 0-1
#define NETCHR_LIFEMASK  0x0003 // 0 alive, 1 dying, 2 dead
#define NETCHR_CLOAKED   0x0004
#define NETCHR_FIRINGL   0x0008
#define NETCHR_FIRINGR   0x0010
#define NETCHR_SPARE20   0x0020 // unused (teleports are counted in byte 46)
#define NETCHR_HIDDEN    0x0040
#define NETCHR_PERIMOFF  0x0080
#define NETCHR_AUTOANIM  0x0100
#define NETCHR_LADDER    0x0200
#define NETCHR_ENABLED   0x0400

// OBJ record byte 0
#define NETOBJ_ENABLED    0x01
#define NETOBJ_GONE       0x02
#define NETOBJ_INVISIBLE  0x04
#define NETOBJ_PROJECTILE 0x08
#define NETOBJ_EMBEDDED   0x10

// CHR anim flags byte
#define NETANIM_FLIP    0x01
#define NETANIM_FLIP2   0x02
#define NETANIM_LOOP    0x04
#define NETANIM_REV     0x08 // speed < 0
#define NETANIM_REV2    0x10
#define NETANIM_HAS2    0x20 // animnum2 is in use

/**
 * An entity's state unquantized: what the host reads from the game and what
 * the client gets back from a record (dumps, and phase 4b's puppets)
 */
struct netentstate {
	u16 flags;     // NETCHR_* or NETOBJ_* or a door's/lift's
	f32 pos[3];
	s32 room;
	f32 yaw;       // radians, rendered yaw (chr)
	f32 rooty;     // root height above prop->pos.y (chr)
	f32 groundy;   // ground below prop->pos.y (chr)
	s16 animnum;
	u8 animflags;
	f32 frame;
	s16 animnum2;
	f32 frame2;
	f32 fracmerge;
	f32 aim[4];    // aimendlshoulder, aimendrshoulder, aimendback, aimendsideback + angleoffset
	u8 weapon[2];  // held right, left (0xff none)
	u16 hat;       // hat model, 0xffff none
	u8 fadealpha;
	u8 cloakfrac;  // 7 bits, 0x80 = finished
	f32 cshield;
	f32 drugheadsway;
	u8 fadeintimer;
	u8 teleports;  // chr: counts up on every teleport (a toggled bit loses two)
	f32 quat[4];   // x y z w (obj)
	s16 damage;
	u8 extra[3];
	f32 doorfrac;  // frac / maxfrac
	s8 doormode;
	u8 laserfade;
	s8 levelcur;
	s8 levelaim;
};

void netRecPack(s32 rec, const struct netentstate *s, u8 *out);   // out: netRecSize(rec) bytes
void netRecUnpack(s32 rec, const u8 *in, struct netentstate *out);

// Quantizers, shared so a test can bound the error
void netQuatFromMatrix(const f32 m[3][3], f32 *q);  // q x y z w, unit, largest component >= 0
void netQuatCanon(f32 *q);

/**
 * The local-player block: the receiving client's own player at full
 * precision (f32 bits as they are), delta'd against the block in the
 * baseline snapshot. Layout in netproto.h.
 */
#define NETLP_SIZE     (208 + NETMOVE_SIZE)
#define NETLP_NUMAMMO  33
#define NETMOVE_SIZE   480
#define NETMOVE_HEADWORDS 30 // struct player bondheadsave: the head model's rwdata

/**
 * The movement state in the local-player block (bytes 208..687): what the
 * walk carries from one tick into the next, at full precision, so a client
 * can start its replay of the commands the host has not played yet from the
 * host's own state (netpredict.c, PLANS/NETPLAY.md "Prediction"). Fields
 * are struct player's of the same name; fallage is lvframe60 - fallstart,
 * as the two machines' level clocks differ.
 */
struct netmove {
	f32 speedtheta;
	f32 speedverta;
	f32 speedthetacontrol;
	f32 speedsideways;
	f32 speedstrafe;
	f32 speedforwards;
	f32 speedboost;
	f32 speedgo;
	s32 speedmaxtime60;
	f32 shotspeed[3];     // bondshotspeed
	f32 moveinitspeed[3];
	f32 forcespeed[3];    // bondforcespeed
	f32 rollspeed[3];
	s32 rolltime60;
	f32 vely;             // bdeltapos.y: the vertical speed
	f32 sumground;
	f32 manground;        // vv_manground
	f32 ground;           // vv_ground
	f32 onground;         // bondonground
	s32 fallage;
	f32 crouchoffset;
	f32 crouchspeed;
	f32 crouchheight;
	f32 crouchfall;
	f32 sumcrouch;
	f32 crouchoffsetsmall;
	s32 crouchtime240;
	s32 crouchoffsetreal;
	s32 crouchoffsetrealsmall;
	f32 swaytarget;       // the lean
	f32 swayoffset0;
	f32 swayoffset2;
	f32 laddernormal[3];
	f32 ladderupdown;
	f32 liftground;
	f32 height;           // vv_height
	f32 eyeheight;        // vv_eyeheight
	f32 gunspeed;
	f32 breathing;        // bondbreathing
	f32 headpossum[3];    // the head bob's sum, which walks the player
	s32 headwalkingtime60;
	s16 floorroom;
	u16 floorflags;
	u8 isfalling;
	u8 onladder;
	u8 inlift;
	u8 movemode;          // bondmovemode
	s8 crouchpos;
	s8 autocrouchpos;
	s8 headanim;
	u8 floortype;
	// the head's animation (struct anim, player->unk01c0): the bob that
	// walks the player is read off it. Not its frame slots (this machine's
	// cache, loaded again from framea/frameb whenever it is posed) nor its
	// functions
	s16 animnum;
	s16 animnum2;
	s8 flip;
	s8 flip2;
	s8 looping;
	s8 average;
	s16 framea;
	s16 frameb;
	s16 frame2a;
	s16 frame2b;
	f32 frame;
	f32 frac;
	f32 endframe;
	f32 speed;
	f32 newspeed;
	f32 oldspeed;
	f32 timespeed;
	f32 elapsespeed;
	f32 frame2;
	f32 frac2;
	f32 endframe2;
	f32 speed2;
	f32 newspeed2;
	f32 oldspeed2;
	f32 timespeed2;
	f32 elapsespeed2;
	f32 fracmerge;
	f32 timemerge;
	f32 elapsemerge;
	f32 loopframe;
	f32 loopmerge;
	f32 playspeed;
	f32 newplay;
	f32 oldplay;
	f32 timeplay;
	f32 elapseplay;
	f32 animscale;
	u32 headsave[NETMOVE_HEADWORDS]; // the head model's rwdata (its root's place)
	// the aim: the mouse's crosshair, which turns the view past the edge
	// boundary while aiming (bmoveProcessInput)
	f32 swivelpos[2];
	u8 insightaimmode;
	// (3 bytes of 0 on the wire: NETMOVE_SIZE)
};

#define NETLP_DEAD       0x01
#define NETLP_INVINCIBLE 0x02

struct netlpstate {
	u8 flags;
	u8 respawns;   // counts up on every respawn (a flag would be lost with its packet)
	u8 teleports;  // counts up on every teleport or position jump
	u8 dual;
	f32 pos[3];
	s16 rooms[8];
	f32 theta;
	f32 verta;
	f32 health;
	f32 shield;
	s16 weaponnum;
	s32 loaded[4];          // hands[0].loadedammo[0..1], hands[1].loadedammo[0..1]
	u16 ammo[NETLP_NUMAMMO];
	u8 inv[32];             // a bit per weapon number held
	u8 invdual[32];         // a bit per weapon number held twice
	struct netmove mv;
};

void netLpPack(const struct netlpstate *s, u8 *out);
void netLpUnpack(const u8 *in, struct netlpstate *out);

/**
 * The scenario block (protocol 7): the match's scenario state, the same for
 * every client, delta'd against the block in the baseline snapshot like the
 * local-player block. Its layout is netscen.c's (netproto.h, SNAP); this
 * file carries its bytes only.
 */
#define NETSCEN_SIZE 768 // protocol 10 (was 640): MAX_MPCHRS 92, MAX_PLAYERS 12

/**
 * The ack block in each CMD (client -> host): the newest snapshot decoded
 * and the 32 before it as bits, reliable.io style, plus descriptors the
 * client needs again (its mapping went stale)
 */
#define NETSNAP_MAXNACK   8
#define NETSNAPACK_WANTKEY 0x01 // the client lacks a baseline: start over

struct netsnapack {
	u16 seq;    // 0: none yet
	u32 bits;   // bit i: snapshot seq-1-i decoded too
	u8 flags;
	u8 nnack;
	u16 nack[NETSNAP_MAXNACK];
};

void netSnapAckWrite(struct netbuf *b, const struct netsnapack *a);
void netSnapAckRead(struct netbuf *b, struct netsnapack *a);

struct netsnaphdr {
	u32 matchid;
	u16 seq;
	u16 baseline;   // 0: a keyframe
	u32 hosttick;
	u8 lvupdate240; // the host's timescale for the tick
	u8 rate;        // host ticks per snapshot now (2: 30 Hz, 3: 20 Hz)
	u32 lastcmd;    // the last of this client's commands played, 0xffffffff none
	u16 maxids;     // entity ids run 0..maxids-1 (the host's g_Vars.maxprops)
};

#define NETSNAP_HDRSIZE  (1 + 4 + 2 + 2 + 4 + 1 + 1 + 4 + 2)
#define NETSNAP_MAXIDS   8192
#define NETSNAP_MAXBYTES (NETSNAP_MAXIDS / 8)

/**
 * Host: what it offers one client in one snapshot. ents[] rise by id with
 * no id twice; the builder writes each one's status.
 */
#define NETSNAPST_SENT     1 // its record is in the packet (new or changed)
#define NETSNAPST_SYNC     2 // unchanged since the baseline: the client has it exactly
#define NETSNAPST_DEFERRED 3 // changed, carried stale for a later packet (the cap)
#define NETSNAPST_EXCLUDED 4 // new and did not fit: not in this snapshot

struct netsnapent {
	u16 id;
	struct netdesc desc;
	u8 record[NETREC_MAX];
	f32 weight;     // added to its priority each snapshot it waits
	u8 status;      // out
};

struct netsnaphost {
	s32 maxids;
	struct netbaseline bl;
	u16 seq;        // the last one built
	u16 acked;      // the newest the client acked (0 none)
	f32 *prio;      // [maxids]
	u8 *nack;       // [maxids]: resend the descriptor
	u8 lp[NETBASELINE_SLOTS][NETLP_SIZE];
	u16 lpseq[NETBASELINE_SLOTS];
	u8 scen[NETBASELINE_SLOTS][NETSCEN_SIZE];
	u16 scenseq[NETBASELINE_SLOTS];
	s32 rate;
	// the adaptive rate's window
	u32 winsent;
	u32 winacked;
	u32 starved;    // snapshots in a row that left something changed behind
	u32 good;       // snapshots in a row with no loss to speak of and nothing left
	// totals
	u32 sent;
	u32 bytes;
	u32 bytesmin;
	u32 bytesmax;
	u32 keyframes;  // whole snapshots with no baseline
	u32 entkeys;    // records sent as keyframes
	u32 records;
	u32 descs;
	u32 deferred;
	u32 excluded;
	u32 acks;       // snapshots acked
	u32 nacks;      // descriptor resends asked for
	u32 resets;     // keyframes the client asked for
	u32 ratechanges;
};

s32 netSnapHostInit(struct netsnaphost *h, s32 maxids);
void netSnapHostFree(struct netsnaphost *h);
void netSnapHostReset(struct netsnaphost *h); // everything goes as a keyframe next
void netSnapHostOnAck(struct netsnaphost *h, const struct netsnapack *a);

/**
 * Builds snapshot hdr->seq (set here, with hdr->baseline and hdr->rate)
 * into out, at most cap bytes, and stores it as the client will hold it.
 * lp is this client's NETLP_SIZE block or NULL, scen the NETSCEN_SIZE
 * scenario block or NULL, evseq the EVENTS messages sent to this client
 * before it (sent with the block). Returns the length, or -1 if nothing could be
 * built.
 */
s32 netSnapHostBuild(struct netsnaphost *h, struct netsnaphdr *hdr, struct netsnapent *ents, s32 n,
		const u8 *lp, const u8 *scen, u32 evseq, u8 *out, s32 cap);

/**
 * Client
 */
struct netsnapinfo {
	u16 seq;
	u32 hosttick;
	u32 lastcmd;
	u8 lvupdate240;
	u8 rate;
	u8 haslp;
	u8 hasscen;
	u32 evseq;      // with the scenario block: EVENTS messages the host had sent before it
	u8 lp[NETLP_SIZE];
	u8 scen[NETSCEN_SIZE];
};

struct netsnapdescin {
	u16 id;
	struct netdesc desc;
};

// This many old drops in a row: the newest held is not the host's (a forged
// or corrupt seq got in); forget it and ask for a keyframe
#define NETSNAP_RESYNC 32

struct netsnapclient {
	s32 maxids;      // 0 until the first snapshot
	struct netbaseline bl;
	struct netsnapinfo info[NETBASELINE_SLOTS];
	u16 newest;
	u32 ackbits;
	s32 wantkey;
	// the last snapshot's descriptors
	struct netsnapdescin *descs; // [maxids]
	s32 ndescs;
	// scratch
	u16 *ids;        // [maxids]
	u8 *stores;      // [maxids * NETSNAP_STORE]
	// counts
	u32 received;
	u32 decoded;
	u32 old;         // behind the newest, or a duplicate
	u32 otherMatch;
	u32 nobase;      // named a baseline no longer held
	u32 malformed;
	u32 keyframes;
	u32 entkeys;
	u32 bytes;
	u32 resyncs;     // a run of NETSNAP_RESYNC old drops: started over
	s32 oldrun;
	s32 probe;       // decode and check only, store nothing (hostile tests)
};

void netSnapClientFree(struct netsnapclient *c);
void netSnapClientReset(struct netsnapclient *c);

/**
 * Decodes a SNAP message (b is past its type byte). 1: a new snapshot is
 * stored (c->newest; hdr filled), 0: dropped (old, another match, no
 * baseline), -1: malformed. Nothing is stored unless the whole message
 * checks out.
 */
s32 netSnapClientDecode(struct netsnapclient *c, struct netbuf *b, u32 matchid, struct netsnaphdr *hdr);

// The ack fields for the next CMD (nack left to the caller)
void netSnapClientAck(const struct netsnapclient *c, struct netsnapack *a);

const struct netsnapinfo *netSnapClientInfo(const struct netsnapclient *c, u16 seq);

/**
 * The interpolation buffer's lookup: the newest decoded snapshot at or
 * before hosttick and the oldest after it (0 where there is none). Returns
 * 1 if either was found.
 */
s32 netSnapClientBracket(const struct netsnapclient *c, u32 hosttick, u16 *before, u16 *after);

#endif
