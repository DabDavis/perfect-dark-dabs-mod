/**
 * pd-nettest: snapshots (port/src/net/netsnap.c) with no game.
 *
 *   - the quantizers' error bounds;
 *   - a stream: a random world of chrs, objects, doors and lifts moving,
 *     appearing, vanishing and coming back in reused slots, built by the
 *     host for one client over a channel that loses and delays snapshots
 *     and acks. After every decode the client's snapshot must equal what
 *     the host stored for that sequence byte for byte, and every SENT or
 *     SYNC record must equal the world's;
 *   - hostile input: every truncation, random bit flips, random bytes and
 *     forged headers into the decoder; garbage acks and nacks into the host.
 *     Nothing may crash or read out of bounds (run it under ASan), and the
 *     honest stream must still decode afterwards.
 *
 * Each test returns its count of failed checks; nettest.c prints verdicts.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netdelta.h"
#include "net/netsnap.h"

#define SCHECK(cond) do { \
	if (!(cond)) { \
		if (fails < 10) { \
			printf("  %s:%d: %s: check failed: %s\n", __FILE__, __LINE__, name, #cond); \
		} \
		fails++; \
	} \
} while (0)

static u32 s_Rng = 777;

static u32 srnd(void)
{
	s_Rng ^= s_Rng << 13;
	s_Rng ^= s_Rng >> 17;
	s_Rng ^= s_Rng << 5;
	return s_Rng;
}

static f32 srndf(f32 lo, f32 hi)
{
	return lo + (hi - lo) * (srnd() % 100000) / 100000.f;
}

static f32 angdiff(f32 a, f32 b)
{
	f32 d = fmodf(a - b, 6.28318530718f);

	if (d > 3.14159265f) {
		d -= 6.28318530718f;
	} else if (d < -3.14159265f) {
		d += 6.28318530718f;
	}

	return fabsf(d);
}

s32 snapTestQuant(void)
{
	const char *name = "snapshot quantizers";
	s32 fails = 0;
	s32 i;

	for (i = 0; i < 20000; i++) {
		struct netentstate s;
		struct netentstate o;
		u8 rec[NETREC_MAX];
		f32 m[3][3];
		f32 ax;
		f32 ay;
		f32 az;
		f32 q[4];
		f32 dot;

		memset(&s, 0, sizeof(s));
		s.pos[0] = srndf(-60000, 60000);
		s.pos[1] = srndf(-60000, 60000);
		s.pos[2] = srndf(-60000, 60000);
		s.yaw = srndf(0, 6.2831853f);
		s.frame = srndf(0, 4000);
		s.frame2 = srndf(0, 4000);
		s.rooty = srndf(-300, 300);
		s.groundy = srndf(-300, 300);
		s.aim[0] = srndf(-3.14f, 3.14f);
		s.aim[3] = srndf(-3.14f, 3.14f);
		s.fracmerge = srndf(0, 1);
		s.room = (s32)(srnd() % 300) - 1;
		s.animnum = (s16)(srnd() % 2000);
		s.teleports = (u8)srnd();

		netRecPack(NETREC_CHR, &s, rec);
		netRecUnpack(NETREC_CHR, rec, &o);
		SCHECK(fabsf(o.pos[0] - s.pos[0]) <= 0.0626f);
		SCHECK(fabsf(o.pos[1] - s.pos[1]) <= 0.0626f);
		SCHECK(fabsf(o.pos[2] - s.pos[2]) <= 0.0626f);
		SCHECK(angdiff(o.yaw, s.yaw) <= 3.1416f / 65536.f + 1e-5f);
		SCHECK(fabsf(o.frame - s.frame) <= 0.0626f);
		SCHECK(fabsf(o.frame2 - s.frame2) <= 0.0626f);
		SCHECK(fabsf(o.rooty - s.rooty) <= 0.0626f);
		SCHECK(fabsf(o.groundy - s.groundy) <= 0.0626f);
		SCHECK(angdiff(o.aim[0], s.aim[0]) <= 1e-4f);
		SCHECK(angdiff(o.aim[3], s.aim[3]) <= 1e-4f);
		SCHECK(fabsf(o.fracmerge - s.fracmerge) <= 0.0021f);
		SCHECK(o.room == s.room);
		SCHECK(o.animnum == s.animnum);
		SCHECK(o.teleports == s.teleports);

		// a rotation from three random angles, to a quaternion and back
		ax = srndf(-3.14f, 3.14f);
		ay = srndf(-3.14f, 3.14f);
		az = srndf(-3.14f, 3.14f);
		{
			const f32 cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay), cz = cosf(az), sz = sinf(az);

			m[0][0] = cy * cz; m[0][1] = -cy * sz; m[0][2] = sy;
			m[1][0] = sx * sy * cz + cx * sz; m[1][1] = -sx * sy * sz + cx * cz; m[1][2] = -sx * cy;
			m[2][0] = -cx * sy * cz + sx * sz; m[2][1] = cx * sy * sz + sx * cz; m[2][2] = cx * cy;
		}
		netQuatFromMatrix(m, s.quat);
		netRecPack(NETREC_OBJ, &s, rec);
		netRecUnpack(NETREC_OBJ, rec, &o);
		memcpy(q, o.quat, sizeof(q));
		dot = q[0] * s.quat[0] + q[1] * s.quat[1] + q[2] * s.quat[2] + q[3] * s.quat[3];
		SCHECK(fabsf(dot) >= 0.99999f);
	}

	return fails;
}

/**
 * The local-player block round trip: every field of it, the movement state
 * (protocol 5) included, comes back bit for bit, and the block is all its
 * bytes and no more (a field left out of the packing would come back zero)
 */
s32 snapTestLocalPlayer(void)
{
	const char *name = "local-player block";
	s32 fails = 0;
	s32 n;

	for (n = 0; n < 2000; n++) {
		struct netlpstate s;
		struct netlpstate o;
		u8 a[NETLP_SIZE + 8];
		u8 b[NETLP_SIZE];
		u8 *raw = (u8 *)&s.mv;
		size_t k;
		s32 i;

		memset(&s, 0, sizeof(s));
		s.flags = (u8)srnd();
		s.respawns = (u8)srnd();
		s.teleports = (u8)srnd();
		s.dual = (u8)srnd();

		for (i = 0; i < 3; i++) {
			s.pos[i] = srndf(-60000, 60000);
		}

		for (i = 0; i < 8; i++) {
			s.rooms[i] = (s16)(srnd() % 400) - 1;
		}

		s.theta = srndf(0, 360);
		s.verta = srndf(-90, 90);
		s.health = srndf(0, 1);
		s.shield = srndf(0, 8);
		s.weaponnum = (s16)(srnd() % 256);

		for (i = 0; i < 4; i++) {
			s.loaded[i] = (s32)srnd();
		}

		for (i = 0; i < NETLP_NUMAMMO; i++) {
			s.ammo[i] = (u16)srnd();
		}

		for (i = 0; i < 32; i++) {
			s.inv[i] = (u8)srnd();
			s.invdual[i] = (u8)srnd();
		}

		// the movement state: random bits in every field (the struct's own
		// padding is whatever it is, and is not compared)
		for (k = 0; k < sizeof(s.mv); k++) {
			raw[k] = (u8)srnd();
		}

		memset(a, 0xa5, sizeof(a));
		netLpPack(&s, a);
		SCHECK(a[NETLP_SIZE] == 0xa5 && a[NETLP_SIZE + 7] == 0xa5);
		netLpUnpack(a, &o);
		netLpPack(&o, b);
		SCHECK(memcmp(a, b, NETLP_SIZE) == 0);
		SCHECK(o.pos[0] == s.pos[0] && o.theta == s.theta && o.weaponnum == s.weaponnum);
		SCHECK(memcmp(o.ammo, s.ammo, sizeof(s.ammo)) == 0 && memcmp(o.invdual, s.invdual, sizeof(s.invdual)) == 0);
		SCHECK(memcmp(&o.mv.speedtheta, &s.mv.speedtheta, 4) == 0);
		SCHECK(o.mv.fallage == s.mv.fallage && o.mv.headwalkingtime60 == s.mv.headwalkingtime60);
		SCHECK(o.mv.floorroom == s.mv.floorroom && o.mv.floorflags == s.mv.floorflags);
		SCHECK(o.mv.isfalling == s.mv.isfalling && o.mv.movemode == s.mv.movemode && o.mv.crouchpos == s.mv.crouchpos);
		SCHECK(o.mv.headanim == s.mv.headanim && o.mv.floortype == s.mv.floortype);
		SCHECK(o.mv.animnum == s.mv.animnum && o.mv.frame2b == s.mv.frame2b && o.mv.flip2 == s.mv.flip2);
		SCHECK(memcmp(&o.mv.frame, &s.mv.frame, 4) == 0 && memcmp(&o.mv.animscale, &s.mv.animscale, 4) == 0);
		SCHECK(memcmp(&o.mv.headpossum, &s.mv.headpossum, sizeof(s.mv.headpossum)) == 0);
		SCHECK(memcmp(o.mv.headsave, s.mv.headsave, sizeof(s.mv.headsave)) == 0);
		SCHECK(memcmp(&o.mv.laddernormal, &s.mv.laddernormal, sizeof(s.mv.laddernormal)) == 0);
		SCHECK(memcmp(o.mv.swivelpos, s.mv.swivelpos, sizeof(s.mv.swivelpos)) == 0);
		SCHECK(o.mv.insightaimmode == (s.mv.insightaimmode ? 1 : 0));
		// the aim is the last of the packed movement, then 3 bytes of 0
		SCHECK(a[208 + NETMOVE_SIZE - 4] == (s.mv.insightaimmode ? 1 : 0));
		SCHECK(a[208 + NETMOVE_SIZE - 3] == 0 && a[208 + NETMOVE_SIZE - 2] == 0 && a[208 + NETMOVE_SIZE - 1] == 0);
	}

	return fails;
}

/*
 * The stream
 */

#define W_MAX     700
#define W_PRESENT 260

struct went {
	s32 alive;
	u16 gen;
	struct netdesc desc;
	struct netentstate st;
	u8 rec[NETREC_MAX];
	s32 moving;
};

static struct went s_World[W_MAX];

static void worldSpawn(s32 id)
{
	struct went *e = &s_World[id];
	const u32 r = srnd() % 10;

	e->alive = 1;
	e->gen = (u16)(e->gen + 1 ? e->gen + 1 : 1);
	memset(&e->desc, 0, sizeof(e->desc));
	memset(&e->st, 0, sizeof(e->st));
	e->desc.gen = e->gen;

	if (r < 2) {
		// a sim, or (protocol 12) a mission's setup chr keyed by its command
		e->desc.kind = srnd() % 3 == 0 ? NETDESC_SETUPCHR : NETDESC_SIM;
		e->desc.rec = NETREC_CHR;
		e->desc.key = e->desc.kind == NETDESC_SETUPCHR ? (u16)id : id % 32;
		e->desc.bodynum = (s16)(srnd() % 60);
		e->desc.headnum = (s16)(srnd() % 60);
		e->moving = 1;
	} else if (r < 3) {
		e->desc.kind = NETDESC_SETUPOBJ;
		e->desc.rec = NETREC_DOOR;
		e->desc.key = (u16)id;
		e->desc.objtype = 1;
		e->moving = srnd() % 3 == 0;
	} else if (r < 4) {
		e->desc.kind = NETDESC_SETUPOBJ;
		e->desc.rec = NETREC_LIFT;
		e->desc.key = (u16)id;
		e->desc.objtype = 0x30;
		e->moving = srnd() % 2;
	} else {
		e->desc.kind = r < 7 ? NETDESC_SETUPOBJ : NETDESC_DYNWEAPON;
		e->desc.rec = NETREC_OBJ;
		e->desc.key = (u16)id;
		e->desc.objtype = 8;
		e->desc.weaponnum = (u8)(srnd() % 60);
		e->desc.modelnum = (s16)(srnd() % 500);
		e->moving = srnd() % 6 == 0;

		if (r == 9 && srnd() % 2) {
			// a scenario's prop (protocol 7): its scale and team come along
			e->desc.kind = NETDESC_SCENOBJ;
			e->desc.extrascale = (u16)(51 + srnd() % 462);
			e->desc.team = (u8)(srnd() % 4);
			e->desc.scenflags = (u8)(srnd() % 2);
		}
	}

	e->st.pos[0] = srndf(-5000, 5000);
	e->st.pos[1] = srndf(-500, 500);
	e->st.pos[2] = srndf(-5000, 5000);
	e->st.room = (s32)(srnd() % 200);
	e->st.quat[3] = 1;
	e->st.yaw = srndf(0, 6.28f);
	e->st.animnum = (s16)(srnd() % 300);
	e->st.weapon[0] = 0xff;
	e->st.weapon[1] = 0xff;
	e->st.hat = 0xffff;
	e->st.flags = 0x01;
}

static void worldStep(void)
{
	s32 i;

	for (i = 0; i < W_MAX; i++) {
		struct went *e = &s_World[i];

		if (!e->alive) {
			// slots are reused, as propAllocate reuses them
			if (srnd() % 400 == 0) {
				worldSpawn(i);
			}

			continue;
		}

		if (srnd() % 600 == 0 && e->desc.kind != NETDESC_SIM) {
			e->alive = 0;
			continue;
		}

		if (e->moving) {
			e->st.pos[0] += srndf(-12, 12);
			e->st.pos[2] += srndf(-12, 12);
			e->st.yaw = fmodf(e->st.yaw + srndf(0, 0.1f), 6.28f);
			e->st.frame = fmodf(e->st.frame + 0.5f, 60.f);
			e->st.doorfrac = srndf(0, 1);
			e->st.aim[2] = srndf(-1, 1);

			if (srnd() % 50 == 0) {
				e->st.animnum = (s16)(srnd() % 300);
			}
		}

		netRecPack(e->desc.rec, &e->st, e->rec);
	}
}

// One host->client or client->host message in flight
struct inflight {
	s32 at;     // the step it arrives
	s32 len;
	u8 data[1200];
	struct netsnapack ack;
};

#define FLIGHT_MAX 64

static s32 sameStoredSnapshot(const struct netsnaphost *h, const struct netsnapclient *c, u16 seq)
{
	const struct netbaselineslot *hs = &h->bl.slots[seq % NETBASELINE_SLOTS];
	const struct netbaselineslot *cs = &c->bl.slots[seq % NETBASELINE_SLOTS];

	if (!hs->valid || !cs->valid || hs->seq != seq || cs->seq != seq || hs->count != cs->count) {
		return 0;
	}

	return memcmp(hs->ids, cs->ids, hs->count * sizeof(u16)) == 0
		&& memcmp(hs->records, cs->records, (size_t)hs->count * NETSNAP_STORE) == 0;
}

static s32 snapStream(const char *name, s32 losspct, s32 steps, s32 hostile, u32 *bytesavg, u32 *keyframes)
{
	static struct netsnaphost h;
	static struct netsnapclient c;
	static struct netsnapent ents[W_MAX];
	static struct inflight down[FLIGHT_MAX];
	static struct inflight up[FLIGHT_MAX];
	static u8 pkt[1200];
	static u8 mut[1300];
	s32 fails = 0;
	s32 step;
	s32 i;
	s32 decodedafter = 0;
	s32 poisoned = 0;
	s32 decodedpoison = 0;
	u32 probes[3] = {0, 0, 0};
	u8 lp[NETLP_SIZE];
	static u8 scen[NETSCEN_SIZE];
	struct netlpstate lps;

	memset(s_World, 0, sizeof(s_World));
	memset(down, 0, sizeof(down));
	memset(up, 0, sizeof(up));
	memset(&c, 0, sizeof(c));

	for (i = 0; i < W_PRESENT; i++) {
		worldSpawn(i * (W_MAX / W_PRESENT));
	}

	SCHECK(netSnapHostInit(&h, W_MAX) == 0);
	memset(&lps, 0, sizeof(lps));

	for (step = 0; step < steps; step++) {
		struct netsnaphdr hdr;
		s32 n = 0;
		s32 len;

		worldStep();

		for (i = 0; i < W_MAX; i++) {
			if (s_World[i].alive) {
				ents[n].id = (u16)i;
				ents[n].desc = s_World[i].desc;
				memcpy(ents[n].record, s_World[i].rec, NETREC_MAX);
				ents[n].weight = s_World[i].desc.rec == NETREC_CHR ? 8.f : s_World[i].moving ? 2.f : 1.f;
				n++;
			}
		}

		lps.pos[0] += 1.5f;
		lps.theta = srndf(0, 360);
		lps.ammo[step % NETLP_NUMAMMO] = (u16)step;
		netLpPack(&lps, lp);

		// the scenario block: a few counters moving, a holder now and then
		scen[0] = 3;
		scen[4 + (step % 40) * 2] = (u8)step;
		scen[200 + step % 3] = (u8)(step / 7);

		if (srnd() % 10 == 0) {
			scen[184 + srnd() % 400] = (u8)srnd();
		}

		memset(&hdr, 0, sizeof(hdr));
		hdr.matchid = 42;
		hdr.hosttick = step * 2;
		hdr.lvupdate240 = 4;
		hdr.lastcmd = step * 2 - 3;
		len = netSnapHostBuild(&h, &hdr, ents, n, lp, step % 50 == 7 ? NULL : scen, (u32)step * 3 + 1, pkt, 1100);
		SCHECK(len > 0 && len <= 1100);

		// what the host claims is SENT or SYNC is the world as it is
		for (i = 0; i < n && len > 0; i++) {
			const u8 *stored = netBaselineGet(&h.bl, hdr.seq, ents[i].id);

			SCHECK(ents[i].status >= NETSNAPST_SENT && ents[i].status <= NETSNAPST_EXCLUDED);

			if (ents[i].status == NETSNAPST_SENT || ents[i].status == NETSNAPST_SYNC) {
				SCHECK(stored && memcmp(stored + NETSNAP_STOREHDR, ents[i].record, netRecSize(ents[i].desc.rec)) == 0);
			} else if (ents[i].status == NETSNAPST_EXCLUDED) {
				SCHECK(stored == NULL);
			} else {
				SCHECK(stored != NULL);
			}
		}

		if (hostile && step == steps / 2) {
			// a forged ack of this snapshot, which never reaches the client:
			// the host deltas against it, the client finds no baseline, asks
			// for a keyframe, and the stream recovers
			struct netsnapack forged;

			memset(&forged, 0, sizeof(forged));
			forged.seq = hdr.seq;
			netSnapHostOnAck(&h, &forged);
		} else if (len > 0 && (s32)(srnd() % 100) >= losspct) {
			for (i = 0; i < FLIGHT_MAX; i++) {
				if (!down[i].len) {
					down[i].at = step + 1 + srnd() % 3;
					down[i].len = len;
					memcpy(down[i].data, pkt, len);
					break;
				}
			}
		}

		// arrivals at the client, in order of arrival step (older in the
		// same step first)
		for (i = 0; i < FLIGHT_MAX; i++) {
			struct netbuf b;
			struct netsnaphdr got;
			s32 r;

			if (!down[i].len || down[i].at > step) {
				continue;
			}

			if (hostile) {
				// first the garbage, then the honest packet
				s32 k;

				for (k = 0; k < 6; k++) {
					s32 mlen = down[i].len;
					struct netsnapclient *cc = &c;

					memcpy(mut, down[i].data, mlen);

					switch (srnd() % 5) {
					case 0: mlen = srnd() % mlen; break;            // cut short
					case 1: mut[srnd() % mlen] ^= (u8)(1 << (srnd() % 8)); break;
					case 2: { s32 j; for (j = 0; j < 8; j++) mut[srnd() % mlen] = (u8)srnd(); } break;
					case 3: { s32 j; mlen = 1 + srnd() % sizeof(mut); for (j = 0; j < mlen; j++) mut[j] = (u8)srnd(); } break;
					case 4: mut[1 + 4 + 4 + srnd() % 8] = (u8)srnd(); break; // the header after the seqs
					}

					// garbage that parses would be kept: decode it into a
					// throwaway copy of nothing (a fresh decoder), and into
					// the real one only when it cannot be a newer snapshot
					netBufInitRead(&b, mut + 1, mlen > 0 ? mlen - 1 : 0);

					if (srnd() % 3 == 0) {
						// the real decoder in probe mode (as the game's
						// --net-test-hostile does): checked against the real
						// baselines, nothing kept, state put back
						static struct netsnapclient save;

						save = c;
						c.probe = 1;
						r = netSnapClientDecode(&c, &b, 42, &got);
						SCHECK(r >= -1 && r <= 1);
						probes[r + 1]++;
						c = save;
					} else if (srnd() % 2) {
						static struct netsnapclient fresh;

						r = netSnapClientDecode(&fresh, &b, 42, &got);
						SCHECK(r >= -1 && r <= 1);
						netSnapClientFree(&fresh);
					} else {
						// another match's id: the header is read, the rest dropped
						r = netSnapClientDecode(cc, &b, 0xdeadbeef, &got);
						SCHECK(r == 0 || r == -1);
					}
				}
			}

			if (hostile && step >= steps / 4 && !poisoned && down[i].data[1 + 4 + 2] == 0 && down[i].data[1 + 4 + 3] == 0) {
				// a keyframe (it needs no baseline) whose seq got corrupted
				// far ahead, so it parses: every honest one after it is
				// "old" until the client gives up on it and starts over
				memcpy(mut, down[i].data, down[i].len);
				mut[1 + 4 + 1] ^= 0x40;  // seq + 0x4000

				{
					netBufInitRead(&b, mut + 1, down[i].len - 1);
					r = netSnapClientDecode(&c, &b, 42, &got);
					SCHECK(r == 1);
					poisoned = 1;
					decodedpoison = 0;
				}
			}

			netBufInitRead(&b, down[i].data + 1, down[i].len - 1);
			r = netSnapClientDecode(&c, &b, 42, &got);
			SCHECK(r >= 0);

			if (r == 1 && poisoned) {
				decodedpoison++;
			}

			if (r == 1) {
				const struct netsnapinfo *info = netSnapClientInfo(&c, got.seq);

				SCHECK(sameStoredSnapshot(&h, &c, got.seq));

				// the buffer by host tick: this one is found at its tick,
				// and an earlier tick finds nothing newer before it
				{
					u16 before;
					u16 after;

					SCHECK(netSnapClientBracket(&c, got.hosttick, &before, &after) && before == got.seq && after == 0);

					if (got.hosttick > 0 && netSnapClientBracket(&c, got.hosttick - 1, &before, &after)) {
						SCHECK(after != 0);
						SCHECK(before == 0 || netSnapClientInfo(&c, before)->hosttick < got.hosttick);
					}
				}
				SCHECK(info && info->haslp && memcmp(info->lp, h.lp[got.seq % NETBASELINE_SLOTS], NETLP_SIZE) == 0);
				SCHECK(info->hasscen == (h.scenseq[got.seq % NETBASELINE_SLOTS] == got.seq));
				SCHECK(!info->hasscen || memcmp(info->scen, h.scen[got.seq % NETBASELINE_SLOTS], NETSCEN_SIZE) == 0);
				SCHECK(!info->hasscen || info->evseq == info->hosttick / 2 * 3 + 1);
				decodedafter++;
			}

			down[i].len = 0;

			// the ack goes back, maybe
			if ((s32)(srnd() % 100) >= losspct) {
				s32 j;

				for (j = 0; j < FLIGHT_MAX; j++) {
					if (!up[j].len) {
						up[j].len = 1;
						up[j].at = step + 1 + srnd() % 4;
						netSnapClientAck(&c, &up[j].ack);

						// a nack now and then, as a stale mapping sends one
						if (srnd() % 20 == 0) {
							up[j].ack.nnack = 1;
							up[j].ack.nack[0] = (u16)(srnd() % W_MAX);
						}
						break;
					}
				}
			}
		}

		for (i = 0; i < FLIGHT_MAX; i++) {
			if (up[i].len && up[i].at <= step) {
				if (hostile && srnd() % 3 == 0) {
					// forged acks: seqs never sent, bits everywhere, nacks out of range
					struct netsnapack bad;
					u8 raw[64];
					struct netbuf rb;
					s32 j;

					for (j = 0; j < (s32)sizeof(raw); j++) {
						raw[j] = (u8)srnd();
					}

					raw[6] &= 0x07; // flags low; nnack whatever (the reader caps it)
					netBufInitRead(&rb, raw, 6 + srnd() % 30);
					netSnapAckRead(&rb, &bad);

					if (netBufOk(&rb) && !(bad.flags & NETSNAPACK_WANTKEY)) {
						// only forged nacks reach the host here: a forged ack of a
						// snapshot the client never had would (rightly) make the
						// host delta against it, which the client then reports
						// with WANTKEY; that path is tested below
						bad.seq = 0;
						netSnapHostOnAck(&h, &bad);
					}
				}

				netSnapHostOnAck(&h, &up[i].ack);
				up[i].len = 0;
			}
		}

	}

	SCHECK(c.decoded > (u32)steps / 2);
	SCHECK(decodedafter > 0);

	if (hostile) {
		// a poisoned newest was given up on and the stream came back;
		// the probes went deep (some parsed whole, some refused)
		SCHECK(poisoned && c.resyncs >= 1 && decodedpoison > 50);
		SCHECK(probes[2] > 0 && probes[0] > 0);
		printf("  %s: poisoned %d, resyncs %u, %d decoded after; probes: %u refused, %u dropped, %u parsed whole\n",
				name, poisoned, c.resyncs, decodedpoison, probes[0], probes[1], probes[2]);
	}

	if (bytesavg) {
		*bytesavg = h.sent ? h.bytes / h.sent : 0;
	}

	if (keyframes) {
		*keyframes = h.keyframes;
	}

	printf("  %s: %u sent (%u..%u B, mean %u), %u decoded, %u keyframes, %u entity keyframes, %u deferred, %u excluded, %u old, %u no baseline, %u malformed, %u resets, rate %d (%u changes)\n",
			name, h.sent, h.bytesmin, h.bytesmax, h.sent ? h.bytes / h.sent : 0, c.decoded, h.keyframes, h.entkeys,
			h.deferred, h.excluded, c.old, c.nobase, c.malformed, h.resets, h.rate, h.ratechanges);

	netSnapHostFree(&h);
	netSnapClientFree(&c);

	return fails;
}

s32 snapTestStreamClean(void)
{
	return snapStream("snapshot stream, no loss", 0, 1500, 0, NULL, NULL);
}

s32 snapTestStreamLossy(void)
{
	return snapStream("snapshot stream, 10% loss each way", 10, 3000, 0, NULL, NULL);
}

s32 snapTestHostile(void)
{
	const char *name = "snapshot hostile input";
	s32 fails = 0;
	static u8 buf[2000];
	s32 i;

	fails += snapStream("snapshot stream under garbage", 10, 1500, 1, NULL, NULL);

	// raw garbage of every length into a fresh decoder
	for (i = 0; i < 20000; i++) {
		struct netsnapclient c;
		struct netsnaphdr hdr;
		struct netbuf b;
		s32 len = srnd() % sizeof(buf);
		s32 j;
		s32 r;

		memset(&c, 0, sizeof(c));

		for (j = 0; j < len; j++) {
			buf[j] = (u8)srnd();
		}

		// a plausible header half the time, so the body gets parsed
		if (len > 22 && srnd() % 2) {
			buf[0] = 42; buf[1] = 0; buf[2] = 0; buf[3] = 0;
			buf[4] = (u8)(1 + srnd() % 200); buf[5] = 0;
			buf[6] = 0; buf[7] = 0;
			buf[20] = (u8)srnd(); buf[21] = (u8)(srnd() % 4);
		}

		netBufInitRead(&b, buf, len);
		r = netSnapClientDecode(&c, &b, 42, &hdr);
		SCHECK(r >= -1 && r <= 1);
		netSnapClientFree(&c);
	}

	// the ack block: every length of garbage
	for (i = 0; i < 20000; i++) {
		struct netsnapack a;
		struct netbuf b;
		s32 len = srnd() % 40;
		s32 j;

		for (j = 0; j < len; j++) {
			buf[j] = (u8)srnd();
		}

		netBufInitRead(&b, buf, len);
		netSnapAckRead(&b, &a);
		SCHECK(a.nnack <= NETSNAP_MAXNACK);
	}

	// descriptors: garbage kinds and records refused
	for (i = 0; i < 20000; i++) {
		struct netdesc d;
		struct netbuf b;
		s32 len = srnd() % 12;
		s32 j;

		for (j = 0; j < len; j++) {
			buf[j] = (u8)srnd();
		}

		netBufInitRead(&b, buf, len);
		netDescRead(&b, &d);

		if (netBufOk(&b)) {
			SCHECK(d.kind >= 1 && d.kind < NETDESC_COUNT && netRecSize(d.rec) > 0 && d.gen != 0);
		}
	}

	return fails;
}
