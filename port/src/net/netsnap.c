#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netdelta.h"
#include "net/netsnap.h"

/**
 * Snapshots on the wire: records, descriptors, the local-player block, the
 * host's encoder and the client's decoder (netsnap.h; the message in
 * netproto.h). Game-free on purpose: tools/nettest links this file alone.
 *
 * The priority accumulator is the Tribes engine's ("The TRIBES Engine
 * Networking Model", ghost priorities): every changed entity's priority
 * grows by its weight each snapshot it waits; the packet takes the highest
 * first until the cap. Our own code.
 */

#define NET_PI    3.14159265358979f
#define NET_TWOPI 6.28318530717959f

static const u8 s_RecSizes[NETREC_COUNT] = { 0, NETREC_CHRSIZE, NETREC_OBJSIZE, NETREC_DOORSIZE, NETREC_LIFTSIZE };

s32 netRecSize(s32 rec)
{
	return rec > 0 && rec < NETREC_COUNT ? s_RecSizes[rec] : 0;
}

/*
 * Quantizers
 */

static s32 netRound(f32 v)
{
	return (s32)(v >= 0 ? v + 0.5f : v - 0.5f);
}

static s32 netClampI(s32 v, s32 lo, s32 hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

// A float that is not a number counts as 0
static f32 netFinite(f32 v)
{
	return v == v && v < 1e30f && v > -1e30f ? v : 0.f;
}

static void put16(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static u32 get16(const u8 *p)
{
	return p[0] | (p[1] << 8);
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u32 get32(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void putF(u8 *p, f32 v)
{
	u32 u;

	memcpy(&u, &v, 4);
	put32(p, u);
}

static f32 getF(const u8 *p)
{
	u32 u = get32(p);
	f32 v;

	memcpy(&v, &u, 4);
	return v;
}

// Position: s24 at 1/8 unit
#define NETPOS_MAX 0x7fffff

static void putPos(u8 *p, f32 v)
{
	s32 q = netClampI(netRound(netFinite(v) * 8.f), -NETPOS_MAX, NETPOS_MAX);
	u32 u = (u32)q;

	p[0] = (u8)u;
	p[1] = (u8)(u >> 8);
	p[2] = (u8)(u >> 16);
}

static f32 getPos(const u8 *p)
{
	s32 q = p[0] | (p[1] << 8) | (p[2] << 16);

	if (q & 0x800000) {
		q -= 0x1000000;
	}

	return q / 8.f;
}

// A full turn as u16
static u32 quantYaw(f32 a)
{
	a = netFinite(a);
	a = fmodf(a, NET_TWOPI);

	if (a < 0) {
		a += NET_TWOPI;
	}

	return (u32)netRound(a * (65536.f / NET_TWOPI)) & 0xffff;
}

static f32 unquantYaw(u32 q)
{
	return (q & 0xffff) * (NET_TWOPI / 65536.f);
}

// ±π as s16
static u32 quantAim(f32 a)
{
	a = netFinite(a);

	while (a > NET_PI) {
		a -= NET_TWOPI;
	}

	while (a < -NET_PI) {
		a += NET_TWOPI;
	}

	return (u32)(u16)(s16)netClampI(netRound(a * (32767.f / NET_PI)), -32767, 32767);
}

static f32 unquantAim(u32 q)
{
	return (s16)(u16)q * (NET_PI / 32767.f);
}

// s16 at 1/8 unit
static u32 quantS16x8(f32 v)
{
	return (u32)(u16)(s16)netClampI(netRound(netFinite(v) * 8.f), -32767, 32767);
}

static f32 unquantS16x8(u32 q)
{
	return (s16)(u16)q / 8.f;
}

// An anim frame: u16 at 1/8
static u32 quantFrame(f32 v)
{
	return (u32)netClampI(netRound(netFinite(v) * 8.f), 0, 0xffff);
}

void netQuatCanon(f32 *q)
{
	f32 len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	s32 big = 0;
	s32 i;

	if (!(len > 1e-6f)) {
		q[0] = q[1] = q[2] = 0;
		q[3] = 1;
		return;
	}

	for (i = 0; i < 4; i++) {
		q[i] /= len;

		if (fabsf(q[i]) > fabsf(q[big])) {
			big = i;
		}
	}

	if (q[big] < 0) {
		for (i = 0; i < 4; i++) {
			q[i] = -q[i];
		}
	}
}

void netQuatFromMatrix(const f32 m[3][3], f32 *q)
{
	const f32 tr = m[0][0] + m[1][1] + m[2][2];
	f32 s;

	if (tr > 0) {
		s = sqrtf(tr + 1.f) * 2.f;
		q[3] = 0.25f * s;
		q[0] = (m[2][1] - m[1][2]) / s;
		q[1] = (m[0][2] - m[2][0]) / s;
		q[2] = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		s = sqrtf(1.f + m[0][0] - m[1][1] - m[2][2]) * 2.f;
		q[3] = (m[2][1] - m[1][2]) / s;
		q[0] = 0.25f * s;
		q[1] = (m[0][1] + m[1][0]) / s;
		q[2] = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		s = sqrtf(1.f + m[1][1] - m[0][0] - m[2][2]) * 2.f;
		q[3] = (m[0][2] - m[2][0]) / s;
		q[0] = (m[0][1] + m[1][0]) / s;
		q[1] = 0.25f * s;
		q[2] = (m[1][2] + m[2][1]) / s;
	} else {
		s = sqrtf(1.f + m[2][2] - m[0][0] - m[1][1]) * 2.f;
		q[3] = (m[1][0] - m[0][1]) / s;
		q[0] = (m[0][2] + m[2][0]) / s;
		q[1] = (m[1][2] + m[2][1]) / s;
		q[2] = 0.25f * s;
	}

	q[0] = netFinite(q[0]);
	q[1] = netFinite(q[1]);
	q[2] = netFinite(q[2]);
	q[3] = netFinite(q[3]);
	netQuatCanon(q);
}

/**
 * Smallest three: the largest component's index (2 bits) and the other
 * three at 15 bits over ±1/√2, 47 bits in six bytes. The largest is put
 * back from the unit length.
 */
#define NETQ_SCALE 0.70710678f

static void putQuat(u8 *p, const f32 *qin)
{
	f32 q[4];
	u64 bits;
	s32 big = 0;
	s32 i;
	s32 k = 0;

	memcpy(q, qin, sizeof(q));
	netQuatCanon(q);

	for (i = 1; i < 4; i++) {
		if (fabsf(q[i]) > fabsf(q[big])) {
			big = i;
		}
	}

	bits = (u64)big;

	for (i = 0; i < 4; i++) {
		if (i != big) {
			f32 v = q[i] / NETQ_SCALE; // -1..1
			s32 qq = netClampI(netRound((v * 0.5f + 0.5f) * 32767.f), 0, 32767);

			bits |= (u64)qq << (2 + 15 * k);
			k++;
		}
	}

	for (i = 0; i < 6; i++) {
		p[i] = (u8)(bits >> (8 * i));
	}
}

static void getQuat(const u8 *p, f32 *q)
{
	u64 bits = 0;
	s32 big;
	s32 i;
	s32 k = 0;
	f32 sum = 0;

	for (i = 0; i < 6; i++) {
		bits |= (u64)p[i] << (8 * i);
	}

	big = bits & 3;

	for (i = 0; i < 4; i++) {
		if (i != big) {
			u32 qq = (bits >> (2 + 15 * k)) & 0x7fff;

			q[i] = ((qq / 32767.f) - 0.5f) * 2.f * NETQ_SCALE;
			sum += q[i] * q[i];
			k++;
		}
	}

	q[big] = sum < 1.f ? sqrtf(1.f - sum) : 0.f;
}

/*
 * Records
 */

void netRecPack(s32 rec, const struct netentstate *s, u8 *out)
{
	s32 i;

	memset(out, 0, netRecSize(rec));

	switch (rec) {
	case NETREC_CHR:
		put16(out + 0, s->flags);
		putPos(out + 2, s->pos[0]);
		putPos(out + 5, s->pos[1]);
		putPos(out + 8, s->pos[2]);
		put16(out + 11, (u16)(s16)s->room);
		put16(out + 13, quantYaw(s->yaw));
		put16(out + 15, quantS16x8(s->rooty));
		put16(out + 17, quantS16x8(s->groundy));
		put16(out + 19, (u16)s->animnum);
		out[21] = s->animflags;
		put16(out + 22, quantFrame(s->frame));
		put16(out + 24, (u16)s->animnum2);
		put16(out + 26, quantFrame(s->frame2));
		out[28] = (u8)netClampI(netRound(netFinite(s->fracmerge) * 255.f), 0, 255);

		for (i = 0; i < 4; i++) {
			put16(out + 29 + i * 2, quantAim(s->aim[i]));
		}

		out[37] = s->weapon[0];
		out[38] = s->weapon[1];
		put16(out + 39, s->hat);
		out[41] = s->fadealpha;
		out[42] = s->cloakfrac;
		out[43] = (u8)netClampI(netRound(netFinite(s->cshield) * 16.f), 0, 255);
		out[44] = (u8)(s8)netClampI(netRound(netFinite(s->drugheadsway) * 100.f), -127, 127);
		out[45] = s->fadeintimer;
		out[46] = s->teleports;
		break;
	case NETREC_OBJ:
		out[0] = (u8)s->flags;
		putPos(out + 1, s->pos[0]);
		putPos(out + 4, s->pos[1]);
		putPos(out + 7, s->pos[2]);
		put16(out + 10, (u16)(s16)s->room);
		putQuat(out + 12, s->quat);
		put16(out + 18, (u16)s->damage);
		out[20] = s->extra[0];
		out[21] = s->extra[1];
		out[22] = s->extra[2];
		break;
	case NETREC_DOOR:
		put16(out + 0, (u32)netClampI(netRound(netFinite(s->doorfrac) * 65535.f), 0, 65535));
		out[2] = (u8)s->doormode;
		out[3] = s->laserfade;
		out[4] = (u8)s->flags;
		break;
	case NETREC_LIFT:
		putPos(out + 0, s->pos[0]);
		putPos(out + 3, s->pos[1]);
		putPos(out + 6, s->pos[2]);
		put16(out + 9, (u16)(s16)s->room);
		out[11] = (u8)s->levelcur;
		out[12] = (u8)s->levelaim;
		out[13] = (u8)s->flags;
		break;
	default:
		break;
	}
}

void netRecUnpack(s32 rec, const u8 *in, struct netentstate *s)
{
	s32 i;

	memset(s, 0, sizeof(*s));
	s->quat[3] = 1;
	s->room = -1;

	switch (rec) {
	case NETREC_CHR:
		s->flags = get16(in + 0);
		s->pos[0] = getPos(in + 2);
		s->pos[1] = getPos(in + 5);
		s->pos[2] = getPos(in + 8);
		s->room = (s16)get16(in + 11);
		s->yaw = unquantYaw(get16(in + 13));
		s->rooty = unquantS16x8(get16(in + 15));
		s->groundy = unquantS16x8(get16(in + 17));
		s->animnum = (s16)get16(in + 19);
		s->animflags = in[21];
		s->frame = get16(in + 22) / 8.f;
		s->animnum2 = (s16)get16(in + 24);
		s->frame2 = get16(in + 26) / 8.f;
		s->fracmerge = in[28] / 255.f;

		for (i = 0; i < 4; i++) {
			s->aim[i] = unquantAim(get16(in + 29 + i * 2));
		}

		s->weapon[0] = in[37];
		s->weapon[1] = in[38];
		s->hat = get16(in + 39);
		s->fadealpha = in[41];
		s->cloakfrac = in[42];
		s->cshield = in[43] / 16.f;
		s->drugheadsway = (s8)in[44] / 100.f;
		s->fadeintimer = in[45];
		s->teleports = in[46];
		break;
	case NETREC_OBJ:
		s->flags = in[0];
		s->pos[0] = getPos(in + 1);
		s->pos[1] = getPos(in + 4);
		s->pos[2] = getPos(in + 7);
		s->room = (s16)get16(in + 10);
		getQuat(in + 12, s->quat);
		s->damage = (s16)get16(in + 18);
		s->extra[0] = in[20];
		s->extra[1] = in[21];
		s->extra[2] = in[22];
		break;
	case NETREC_DOOR:
		s->doorfrac = get16(in + 0) / 65535.f;
		s->doormode = (s8)in[2];
		s->laserfade = in[3];
		s->flags = in[4];
		break;
	case NETREC_LIFT:
		s->pos[0] = getPos(in + 0);
		s->pos[1] = getPos(in + 3);
		s->pos[2] = getPos(in + 6);
		s->room = (s16)get16(in + 9);
		s->levelcur = (s8)in[11];
		s->levelaim = (s8)in[12];
		s->flags = in[13];
		break;
	default:
		break;
	}
}

/*
 * Descriptors
 */

void netDescWrite(struct netbuf *b, const struct netdesc *d)
{
	netBufWriteU8(b, (u8)((d->kind << 4) | (d->rec & 0x0f)));
	netBufWriteU16(b, d->gen);

	switch (d->kind) {
	case NETDESC_SETUPOBJ:
		netBufWriteU16(b, d->key);
		netBufWriteU8(b, d->objtype);
		break;
	case NETDESC_SIM:
	case NETDESC_PLAYER:
		netBufWriteU8(b, (u8)d->key);
		netBufWriteS16(b, d->bodynum);
		netBufWriteS16(b, d->headnum);
		break;
	case NETDESC_BODY:
		netBufWriteS16(b, d->bodynum);
		netBufWriteS16(b, d->headnum);
		break;
	case NETDESC_DYNWEAPON:
		netBufWriteU8(b, d->weaponnum);
		netBufWriteU8(b, d->gunfunc);
		netBufWriteS16(b, d->modelnum);
		netBufWriteU8(b, d->objtype);
		break;
	default:
		netBufWriteS16(b, d->modelnum);
		netBufWriteU8(b, d->objtype);
		break;
	}
}

void netDescRead(struct netbuf *b, struct netdesc *d)
{
	u8 kr = netBufReadU8(b);

	memset(d, 0, sizeof(*d));
	d->kind = kr >> 4;
	d->rec = kr & 0x0f;
	d->gen = netBufReadU16(b);

	if (d->kind < 1 || d->kind >= NETDESC_COUNT || netRecSize(d->rec) == 0 || d->gen == 0) {
		b->error = 1;
		return;
	}

	switch (d->kind) {
	case NETDESC_SETUPOBJ:
		d->key = netBufReadU16(b);
		d->objtype = netBufReadU8(b);
		break;
	case NETDESC_SIM:
	case NETDESC_PLAYER:
		d->key = netBufReadU8(b);
		d->bodynum = netBufReadS16(b);
		d->headnum = netBufReadS16(b);
		break;
	case NETDESC_BODY:
		d->bodynum = netBufReadS16(b);
		d->headnum = netBufReadS16(b);
		break;
	case NETDESC_DYNWEAPON:
		d->weaponnum = netBufReadU8(b);
		d->gunfunc = netBufReadU8(b);
		d->modelnum = netBufReadS16(b);
		d->objtype = netBufReadU8(b);
		break;
	default:
		d->modelnum = netBufReadS16(b);
		d->objtype = netBufReadU8(b);
		break;
	}

	// a chr's record goes with a chr's descriptor and nothing else does
	if ((d->rec == NETREC_CHR) != (d->kind == NETDESC_SIM || d->kind == NETDESC_PLAYER || d->kind == NETDESC_BODY)) {
		b->error = 1;
	}
}

static s32 netDescSize(const struct netdesc *d)
{
	switch (d->kind) {
	case NETDESC_SETUPOBJ: return 3 + 3;
	case NETDESC_SIM:
	case NETDESC_PLAYER: return 3 + 5;
	case NETDESC_BODY: return 3 + 4;
	case NETDESC_DYNWEAPON: return 3 + 5;
	default: return 3 + 3;
	}
}

/*
 * The local-player block (netproto.h, SNAP)
 */

// The movement state, field by field in netsnap.h's order (never the
// struct's bytes: its layout is the compiler's)
#define MVF(f)    do { putF(p, m->f); p += 4; } while (0)
#define MVS(f)    do { put32(p, (u32)m->f); p += 4; } while (0)
#define MVF3(f)   do { MVF(f[0]); MVF(f[1]); MVF(f[2]); } while (0)

static void netMovePack(const struct netmove *m, u8 *out)
{
	u8 *p = out;
	s32 i;

	MVF(speedtheta); MVF(speedverta); MVF(speedthetacontrol);
	MVF(speedsideways); MVF(speedstrafe); MVF(speedforwards); MVF(speedboost); MVF(speedgo);
	MVS(speedmaxtime60);
	MVF3(shotspeed); MVF3(moveinitspeed); MVF3(forcespeed); MVF3(rollspeed);
	MVS(rolltime60);
	MVF(vely); MVF(sumground); MVF(manground); MVF(ground); MVF(onground);
	MVS(fallage);
	MVF(crouchoffset); MVF(crouchspeed); MVF(crouchheight); MVF(crouchfall); MVF(sumcrouch); MVF(crouchoffsetsmall);
	MVS(crouchtime240); MVS(crouchoffsetreal); MVS(crouchoffsetrealsmall);
	MVF(swaytarget); MVF(swayoffset0); MVF(swayoffset2);
	MVF3(laddernormal); MVF(ladderupdown); MVF(liftground);
	MVF(height); MVF(eyeheight); MVF(gunspeed); MVF(breathing);
	MVF3(headpossum);
	MVS(headwalkingtime60);
	put16(p, (u16)m->floorroom); p += 2;
	put16(p, m->floorflags); p += 2;
	p[0] = m->isfalling;
	p[1] = m->onladder;
	p[2] = m->inlift;
	p[3] = m->movemode;
	p[4] = (u8)m->crouchpos;
	p[5] = (u8)m->autocrouchpos;
	p[6] = (u8)m->headanim;
	p[7] = m->floortype;
	p += 8;
	put16(p, (u16)m->animnum); put16(p + 2, (u16)m->animnum2);
	p[4] = (u8)m->flip; p[5] = (u8)m->flip2; p[6] = (u8)m->looping; p[7] = (u8)m->average;
	put16(p + 8, (u16)m->framea); put16(p + 10, (u16)m->frameb);
	put16(p + 12, (u16)m->frame2a); put16(p + 14, (u16)m->frame2b);
	p += 16;
	MVF(frame); MVF(frac); MVF(endframe); MVF(speed); MVF(newspeed); MVF(oldspeed); MVF(timespeed); MVF(elapsespeed);
	MVF(frame2); MVF(frac2); MVF(endframe2); MVF(speed2); MVF(newspeed2); MVF(oldspeed2); MVF(timespeed2); MVF(elapsespeed2);
	MVF(fracmerge); MVF(timemerge); MVF(elapsemerge); MVF(loopframe); MVF(loopmerge);
	MVF(playspeed); MVF(newplay); MVF(oldplay); MVF(timeplay); MVF(elapseplay); MVF(animscale);

	for (i = 0; i < NETMOVE_HEADWORDS; i++) {
		put32(p, m->headsave[i]);
		p += 4;
	}

	MVF(swivelpos[0]); MVF(swivelpos[1]);
	p[0] = m->insightaimmode ? 1 : 0;
	p[1] = p[2] = p[3] = 0;
}

#undef MVF
#undef MVS
#define MVF(f)    do { m->f = getF(p); p += 4; } while (0)
#define MVS(f)    do { m->f = (s32)get32(p); p += 4; } while (0)

static void netMoveUnpack(const u8 *in, struct netmove *m)
{
	const u8 *p = in;
	s32 i;

	MVF(speedtheta); MVF(speedverta); MVF(speedthetacontrol);
	MVF(speedsideways); MVF(speedstrafe); MVF(speedforwards); MVF(speedboost); MVF(speedgo);
	MVS(speedmaxtime60);
	MVF3(shotspeed); MVF3(moveinitspeed); MVF3(forcespeed); MVF3(rollspeed);
	MVS(rolltime60);
	MVF(vely); MVF(sumground); MVF(manground); MVF(ground); MVF(onground);
	MVS(fallage);
	MVF(crouchoffset); MVF(crouchspeed); MVF(crouchheight); MVF(crouchfall); MVF(sumcrouch); MVF(crouchoffsetsmall);
	MVS(crouchtime240); MVS(crouchoffsetreal); MVS(crouchoffsetrealsmall);
	MVF(swaytarget); MVF(swayoffset0); MVF(swayoffset2);
	MVF3(laddernormal); MVF(ladderupdown); MVF(liftground);
	MVF(height); MVF(eyeheight); MVF(gunspeed); MVF(breathing);
	MVF3(headpossum);
	MVS(headwalkingtime60);
	m->floorroom = (s16)get16(p); p += 2;
	m->floorflags = (u16)get16(p); p += 2;
	m->isfalling = p[0];
	m->onladder = p[1];
	m->inlift = p[2];
	m->movemode = p[3];
	m->crouchpos = (s8)p[4];
	m->autocrouchpos = (s8)p[5];
	m->headanim = (s8)p[6];
	m->floortype = p[7];
	p += 8;
	m->animnum = (s16)get16(p); m->animnum2 = (s16)get16(p + 2);
	m->flip = (s8)p[4]; m->flip2 = (s8)p[5]; m->looping = (s8)p[6]; m->average = (s8)p[7];
	m->framea = (s16)get16(p + 8); m->frameb = (s16)get16(p + 10);
	m->frame2a = (s16)get16(p + 12); m->frame2b = (s16)get16(p + 14);
	p += 16;
	MVF(frame); MVF(frac); MVF(endframe); MVF(speed); MVF(newspeed); MVF(oldspeed); MVF(timespeed); MVF(elapsespeed);
	MVF(frame2); MVF(frac2); MVF(endframe2); MVF(speed2); MVF(newspeed2); MVF(oldspeed2); MVF(timespeed2); MVF(elapsespeed2);
	MVF(fracmerge); MVF(timemerge); MVF(elapsemerge); MVF(loopframe); MVF(loopmerge);
	MVF(playspeed); MVF(newplay); MVF(oldplay); MVF(timeplay); MVF(elapseplay); MVF(animscale);

	for (i = 0; i < NETMOVE_HEADWORDS; i++) {
		m->headsave[i] = get32(p);
		p += 4;
	}

	MVF(swivelpos[0]); MVF(swivelpos[1]);
	m->insightaimmode = p[0] ? 1 : 0;
}

#undef MVF
#undef MVS
#undef MVF3

void netLpPack(const struct netlpstate *s, u8 *out)
{
	s32 i;

	memset(out, 0, NETLP_SIZE);
	out[0] = s->flags;
	out[1] = s->respawns;
	out[2] = s->teleports;
	out[3] = s->dual;

	for (i = 0; i < 3; i++) {
		putF(out + 4 + i * 4, s->pos[i]);
	}

	for (i = 0; i < 8; i++) {
		put16(out + 16 + i * 2, (u16)s->rooms[i]);
	}

	putF(out + 32, s->theta);
	putF(out + 36, s->verta);
	putF(out + 40, s->health);
	putF(out + 44, s->shield);
	put16(out + 48, (u16)s->weaponnum);

	for (i = 0; i < 4; i++) {
		put32(out + 52 + i * 4, (u32)s->loaded[i]);
	}

	for (i = 0; i < NETLP_NUMAMMO; i++) {
		put16(out + 68 + i * 2, s->ammo[i]);
	}

	memcpy(out + 136, s->inv, 32);
	memcpy(out + 168, s->invdual, 32);
	netMovePack(&s->mv, out + 208);
}

void netLpUnpack(const u8 *in, struct netlpstate *s)
{
	s32 i;

	memset(s, 0, sizeof(*s));
	s->flags = in[0];
	s->respawns = in[1];
	s->teleports = in[2];
	s->dual = in[3];

	for (i = 0; i < 3; i++) {
		s->pos[i] = getF(in + 4 + i * 4);
	}

	for (i = 0; i < 8; i++) {
		s->rooms[i] = (s16)get16(in + 16 + i * 2);
	}

	s->theta = getF(in + 32);
	s->verta = getF(in + 36);
	s->health = getF(in + 40);
	s->shield = getF(in + 44);
	s->weaponnum = (s16)get16(in + 48);

	for (i = 0; i < 4; i++) {
		s->loaded[i] = (s32)get32(in + 52 + i * 4);
	}

	for (i = 0; i < NETLP_NUMAMMO; i++) {
		s->ammo[i] = get16(in + 68 + i * 2);
	}

	memcpy(s->inv, in + 136, 32);
	memcpy(s->invdual, in + 168, 32);
	netMoveUnpack(in + 208, &s->mv);
}

/*
 * Acks
 */

void netSnapAckWrite(struct netbuf *b, const struct netsnapack *a)
{
	s32 i;
	s32 n = a->nnack > NETSNAP_MAXNACK ? NETSNAP_MAXNACK : a->nnack;

	netBufWriteU16(b, a->seq);
	netBufWriteU32(b, a->bits);
	netBufWriteU8(b, a->flags);
	netBufWriteU8(b, (u8)n);

	for (i = 0; i < n; i++) {
		netBufWriteU16(b, a->nack[i]);
	}
}

void netSnapAckRead(struct netbuf *b, struct netsnapack *a)
{
	s32 i;

	memset(a, 0, sizeof(*a));
	a->seq = netBufReadU16(b);
	a->bits = netBufReadU32(b);
	a->flags = netBufReadU8(b);
	a->nnack = netBufReadU8(b);

	if (a->nnack > NETSNAP_MAXNACK) {
		b->error = 1;
		a->nnack = 0;
		return;
	}

	for (i = 0; i < a->nnack; i++) {
		a->nack[i] = netBufReadU16(b);
	}
}

/*
 * Host
 */

s32 netSnapHostInit(struct netsnaphost *h, s32 maxids)
{
	memset(h, 0, sizeof(*h));

	if (maxids <= 0 || maxids > NETSNAP_MAXIDS) {
		return -1;
	}

	if (netBaselineInit(&h->bl, maxids, maxids, NETSNAP_STORE) != 0) {
		return -1;
	}

	h->prio = calloc(maxids, sizeof(f32));
	h->nack = calloc(maxids, 1);

	if (!h->prio || !h->nack) {
		netSnapHostFree(h);
		return -1;
	}

	h->maxids = maxids;
	h->rate = 2;
	h->bytesmin = 0xffffffff;

	return 0;
}

void netSnapHostFree(struct netsnaphost *h)
{
	netBaselineFree(&h->bl);
	free(h->prio);
	free(h->nack);
	memset(h, 0, sizeof(*h));
}

void netSnapHostReset(struct netsnaphost *h)
{
	if (!h->maxids) {
		return;
	}

	netBaselineReset(&h->bl);
	h->acked = 0;
	memset(h->lpseq, 0, sizeof(h->lpseq));
	memset(h->prio, 0, h->maxids * sizeof(f32));
}

static const struct netbaselineslot *netSlotOf(const struct netbaseline *bl, u16 seq)
{
	const struct netbaselineslot *slot;

	if (seq == 0) {
		return NULL;
	}

	slot = &bl->slots[seq % NETBASELINE_SLOTS];

	return slot->valid && slot->seq == seq ? slot : NULL;
}

static void netSnapHostAckOne(struct netsnaphost *h, u16 seq)
{
	const struct netbaselineslot *slot = netSlotOf(&h->bl, seq);

	if (!slot || slot->acked) {
		return;
	}

	netBaselineAck(&h->bl, seq);
	h->acks++;
	h->winacked++;

	if (h->acked == 0 || netSeqNewer(seq, h->acked)) {
		h->acked = seq;
	}
}

void netSnapHostOnAck(struct netsnaphost *h, const struct netsnapack *a)
{
	s32 i;
	u16 seq;

	if (!h->maxids) {
		return;
	}

	if (a->flags & NETSNAPACK_WANTKEY) {
		// the client lost track: everything again, from nothing. With
		// nothing acked every snapshot is a keyframe already, so the
		// requests still in flight behind the first change nothing
		if (h->acked != 0) {
			h->resets++;
			netSnapHostReset(h);
		}

		return;
	}

	if (a->seq == 0) {
		return;
	}

	netSnapHostAckOne(h, a->seq);

	// plain u16 steps, as the client shifts its bits (seq 0 is never used)
	for (i = 0; i < 32; i++) {
		seq = (u16)(a->seq - 1 - i);

		if (seq != 0 && (a->bits & (1u << i))) {
			netSnapHostAckOne(h, seq);
		}
	}

	for (i = 0; i < a->nnack; i++) {
		if (a->nack[i] < h->maxids && !h->nack[a->nack[i]]) {
			h->nack[a->nack[i]] = 1;
			h->nacks++;
		}
	}
}

// One candidate for the packet
struct netcand {
	s32 ent;      // index into ents
	f32 prio;
	s32 cost;
	u8 isnew;     // not in the baseline (or another generation there)
	u8 changed;
	u8 wantdesc;  // a resend the client asked for
	u8 take;
};

static struct netcand *s_Cands = NULL;
static s32 s_CandsCap = 0;
static u8 s_Present[NETSNAP_MAXBYTES];
static u8 s_BasePresent[NETSNAP_MAXBYTES];
static u8 s_Updated[NETSNAP_MAXBYTES];
static u8 s_Tmp[NETDELTA_MAXENCODED(NETLP_SIZE) + 16];

static s32 netCandCmp(const void *a, const void *b)
{
	const struct netcand *x = a;
	const struct netcand *y = b;

	if (x->prio != y->prio) {
		return x->prio > y->prio ? -1 : 1;
	}

	return x->ent - y->ent;
}

static s32 netCandEntCmp(const void *a, const void *b)
{
	return ((const struct netcand *)a)->ent - ((const struct netcand *)b)->ent;
}

static void netBitSet(u8 *bits, s32 i)
{
	bits[i >> 3] |= (u8)(1 << (i & 7));
}

static s32 netBitGet(const u8 *bits, s32 i)
{
	return (bits[i >> 3] >> (i & 7)) & 1;
}

static const u8 *netStoreFind(const struct netbaselineslot *slot, s32 recordsize, u16 id)
{
	s32 lo = 0;
	s32 hi;

	if (!slot) {
		return NULL;
	}

	hi = slot->count - 1;

	while (lo <= hi) {
		s32 mid = (lo + hi) / 2;

		if (slot->ids[mid] == id) {
			return slot->records + (size_t)mid * recordsize;
		}

		if (slot->ids[mid] < id) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return NULL;
}

static void netSnapWriteHdr(struct netbuf *b, const struct netsnaphdr *hdr)
{
	netBufWriteU32(b, hdr->matchid);
	netBufWriteU16(b, hdr->seq);
	netBufWriteU16(b, hdr->baseline);
	netBufWriteU32(b, hdr->hosttick);
	netBufWriteU8(b, hdr->lvupdate240);
	netBufWriteU8(b, hdr->rate);
	netBufWriteU32(b, hdr->lastcmd);
	netBufWriteU16(b, hdr->maxids);
}

/**
 * Writes the packet for the candidates marked take; returns its length or
 * -1 if it did not fit cap
 */
static s32 netSnapHostWrite(struct netsnaphost *h, const struct netsnaphdr *hdr, const struct netsnapent *ents,
		struct netcand *cands, s32 ncands, const struct netbaselineslot *base, const u8 *lp, const u8 *lpbase,
		u8 *out, s32 cap, u8 msgtype)
{
	struct netbuf b;
	const s32 nbytes = (h->maxids + 7) / 8;
	s32 i;
	s32 ndescs = 0;
	s32 prev = -1;

	memset(s_Present, 0, nbytes);
	memset(s_Updated, 0, nbytes);

	for (i = 0; i < ncands; i++) {
		const struct netcand *c = &cands[i];
		const u16 id = ents[c->ent].id;

		if (!c->isnew || c->take) {
			netBitSet(s_Present, id);
		}

		if (c->take && (c->isnew || c->changed)) {
			netBitSet(s_Updated, id);
		}

		if (c->take && (c->isnew || c->wantdesc)) {
			ndescs++;
		}
	}

	netBufInitWrite(&b, out, cap);
	netBufWriteU8(&b, msgtype);
	netSnapWriteHdr(&b, hdr);
	netDeltaWrite(&b, s_Present, base ? s_BasePresent : NULL, nbytes);
	netDeltaWrite(&b, s_Updated, NULL, nbytes);
	netBufWriteVarU32(&b, ndescs);

	for (i = 0; i < ncands && netBufOk(&b); i++) {
		const struct netcand *c = &cands[i];

		if (c->take && (c->isnew || c->wantdesc)) {
			const u16 id = ents[c->ent].id;

			netBufWriteVarU32(&b, (u32)(id - prev - 1));
			netDescWrite(&b, &ents[c->ent].desc);
			prev = id;
		}
	}

	for (i = 0; i < ncands && netBufOk(&b); i++) {
		const struct netcand *c = &cands[i];
		const struct netsnapent *e = &ents[c->ent];

		if (c->take && (c->isnew || c->changed)) {
			const u8 *bstore = c->isnew ? NULL : netStoreFind(base, h->bl.recordsize, e->id);

			netDeltaWrite(&b, e->record, bstore ? bstore + NETSNAP_STOREHDR : NULL, netRecSize(e->desc.rec));
		}
	}

	if (lp) {
		netBufWriteU8(&b, lpbase ? 2 : 1);
		netDeltaWrite(&b, lp, lpbase, NETLP_SIZE);
	} else {
		netBufWriteU8(&b, 0);
	}

	return netBufOk(&b) ? netBufLen(&b) : -1;
}

#define NETSNAP_MSGTYPE 14 // NETMSG_SNAP (netproto.h)

s32 netSnapHostBuild(struct netsnaphost *h, struct netsnaphdr *hdr, struct netsnapent *ents, s32 n,
		const u8 *lp, u8 *out, s32 cap)
{
	const struct netbaselineslot *base;
	const u8 *lpbase = NULL;
	u16 baseline;
	u16 seq;
	s32 i;
	s32 ncands = 0;
	s32 budget;
	s32 fixed;
	s32 len = -1;
	s32 attempt;
	s32 lplen = 0;
	u32 starvedthis = 0;

	if (!h->maxids || n < 0 || n > h->maxids) {
		return -1;
	}

	for (i = 0; i < n; i++) {
		if (ents[i].id >= h->maxids || (i > 0 && ents[i].id <= ents[i - 1].id) || netRecSize(ents[i].desc.rec) == 0) {
			return -1;
		}
	}

	if (s_CandsCap < n) {
		free(s_Cands);
		s_CandsCap = n + 64;
		s_Cands = malloc(sizeof(*s_Cands) * s_CandsCap);

		if (!s_Cands) {
			s_CandsCap = 0;
			return -1;
		}
	}

	// the baseline: the newest snapshot the client has acked, still held
	baseline = h->acked;
	base = netSlotOf(&h->bl, baseline);

	if (!base || !base->acked) {
		baseline = 0;
		base = NULL;
	}

	seq = netSeqNext(h->seq);
	hdr->seq = seq;
	hdr->baseline = baseline;
	hdr->rate = (u8)h->rate;
	hdr->maxids = (u16)h->maxids;

	memset(s_BasePresent, 0, (h->maxids + 7) / 8);

	if (base) {
		for (i = 0; i < base->count; i++) {
			netBitSet(s_BasePresent, base->ids[i]);
		}

		if (h->lpseq[baseline % NETBASELINE_SLOTS] == baseline) {
			lpbase = h->lp[baseline % NETBASELINE_SLOTS];
		}
	}

	if (lp) {
		lplen = netDeltaEncode(lp, lpbase, NETLP_SIZE, s_Tmp, sizeof(s_Tmp)) + 1;
	}

	// what each entity would cost, and how much it wants to go
	for (i = 0; i < n; i++) {
		struct netsnapent *e = &ents[i];
		struct netcand *c = &s_Cands[ncands];
		const u8 *bstore = netStoreFind(base, h->bl.recordsize, e->id);
		const s32 size = netRecSize(e->desc.rec);
		s32 enc;

		memset(c, 0, sizeof(*c));
		c->ent = i;
		e->status = 0;

		if (bstore && netStoreGen(bstore) == e->desc.gen && netStoreRec(bstore) == e->desc.rec
				&& netStoreKind(bstore) == e->desc.kind) {
			enc = netDeltaEncode(e->record, bstore + NETSNAP_STOREHDR, size, s_Tmp, sizeof(s_Tmp));
			c->changed = enc > 1;
			c->wantdesc = h->nack[e->id];

			if (!c->changed && !c->wantdesc) {
				// the client has it as it is: nothing to send, nothing waits
				h->prio[e->id] = 0;
				e->status = NETSNAPST_SYNC;
				c->cost = 0;
			} else {
				h->prio[e->id] += e->weight;
				// + its bits in the two bitmaps, about
				c->cost = (c->changed ? enc : 0) + (c->wantdesc ? netDescSize(&e->desc) + 1 : 0) + 2;

				if (c->wantdesc) {
					// a stale mapping on the client: its descriptor first
					h->prio[e->id] += e->weight * 4;
				}
			}
		} else {
			enc = netDeltaEncode(e->record, NULL, size, s_Tmp, sizeof(s_Tmp));
			c->isnew = 1;
			h->prio[e->id] += e->weight * 2;
			c->cost = enc + netDescSize(&e->desc) + 3;
		}

		if (enc < 0) {
			return -1;
		}

		c->prio = h->prio[e->id];
		ncands++;
	}

	// the most wanted first, as many as fit
	qsort(s_Cands, ncands, sizeof(*s_Cands), netCandCmp);

	fixed = NETSNAP_HDRSIZE + lplen + 1 + 8;
	budget = cap - fixed;

	for (attempt = 0; attempt < 8; attempt++) {
		s32 used = 0;

		for (i = 0; i < ncands; i++) {
			struct netcand *c = &s_Cands[i];

			c->take = 0;

			if (ents[c->ent].status == NETSNAPST_SYNC) {
				continue;
			}

			if (used + c->cost <= budget) {
				c->take = 1;
				used += c->cost;
			}
		}

		qsort(s_Cands, ncands, sizeof(*s_Cands), netCandEntCmp);
		len = netSnapHostWrite(h, hdr, ents, s_Cands, ncands, base, lp, lpbase, out, cap, NETSNAP_MSGTYPE);

		if (len >= 0 || budget <= 0) {
			break;
		}

		// the bitmaps took more than was left for them: less, and again
		budget -= 64 + budget / 8;
		qsort(s_Cands, ncands, sizeof(*s_Cands), netCandCmp);
	}

	if (len < 0) {
		// nothing new at all, only what the client has: the floor
		for (i = 0; i < ncands; i++) {
			s_Cands[i].take = 0;
		}

		qsort(s_Cands, ncands, sizeof(*s_Cands), netCandEntCmp);
		len = netSnapHostWrite(h, hdr, ents, s_Cands, ncands, base, lp, lpbase, out, cap, NETSNAP_MSGTYPE);

		if (len < 0) {
			return -1;
		}
	}

	// store the snapshot as the client will hold it after decoding it
	if (netBaselineBegin(&h->bl, seq) != 0) {
		return -1;
	}

	for (i = 0; i < ncands; i++) {
		struct netcand *c = &s_Cands[i];
		struct netsnapent *e = &ents[c->ent];
		u8 store[NETSNAP_STORE];
		const u8 *bstore;

		if (c->isnew && !c->take) {
			e->status = NETSNAPST_EXCLUDED;
			h->excluded++;
			continue;
		}

		memset(store, 0, sizeof(store));
		store[0] = (u8)e->desc.gen;
		store[1] = (u8)(e->desc.gen >> 8);
		store[2] = e->desc.rec;
		store[3] = e->desc.kind;

		if (c->take && (c->isnew || c->changed)) {
			memcpy(store + NETSNAP_STOREHDR, e->record, netRecSize(e->desc.rec));
			e->status = NETSNAPST_SENT;
			h->prio[e->id] = 0;
			h->records++;
			h->entkeys += c->isnew;
		} else {
			bstore = netStoreFind(base, h->bl.recordsize, e->id);

			if (bstore) {
				memcpy(store + NETSNAP_STOREHDR, bstore + NETSNAP_STOREHDR, NETREC_MAX);
			}

			if (e->status != NETSNAPST_SYNC) {
				e->status = c->changed ? NETSNAPST_DEFERRED : NETSNAPST_SYNC;
			}

			if (c->changed) {
				h->deferred++;
				starvedthis++;
			}
		}

		if (c->take && (c->isnew || c->wantdesc)) {
			h->nack[e->id] = 0;
			h->descs++;
		}

		netBaselineAdd(&h->bl, e->id, store);
	}

	if (lp) {
		memcpy(h->lp[seq % NETBASELINE_SLOTS], lp, NETLP_SIZE);
		h->lpseq[seq % NETBASELINE_SLOTS] = seq;
	} else {
		h->lpseq[seq % NETBASELINE_SLOTS] = 0;
	}

	h->seq = seq;
	h->sent++;
	h->winsent++;
	h->bytes += len;
	h->keyframes += baseline == 0;

	if ((u32)len < h->bytesmin) {
		h->bytesmin = len;
	}

	if ((u32)len > h->bytesmax) {
		h->bytesmax = len;
	}

	/**
	 * The rate: 20 Hz while snapshots are lost or keep leaving changes
	 * behind, back to 30 Hz after two seconds with neither. The loss is
	 * judged over windows of 30 sent against acks heard meanwhile.
	 */
	h->starved = starvedthis ? h->starved + 1 : 0;

	if (h->winsent >= 30) {
		const s32 lossy = h->winacked * 100 < h->winsent * 85;

		if (lossy || h->starved >= 10) {
			if (h->rate != 3) {
				h->rate = 3;
				h->ratechanges++;
			}

			h->good = 0;
		} else if (h->winacked * 100 >= h->winsent * 95 && !h->starved) {
			h->good += h->winsent;

			if (h->rate != 2 && h->good >= 60) {
				h->rate = 2;
				h->ratechanges++;
			}
		}

		h->winsent = 0;
		h->winacked = 0;
	}

	return len;
}

/*
 * Client
 */

void netSnapClientFree(struct netsnapclient *c)
{
	netBaselineFree(&c->bl);
	free(c->descs);
	free(c->ids);
	free(c->stores);
	memset(c, 0, sizeof(*c));
}

void netSnapClientReset(struct netsnapclient *c)
{
	netSnapClientFree(c);
}

static s32 netSnapClientAlloc(struct netsnapclient *c, s32 maxids)
{
	if (maxids <= 0 || maxids > NETSNAP_MAXIDS) {
		return -1;
	}

	if (netBaselineInit(&c->bl, maxids, maxids, NETSNAP_STORE) != 0) {
		return -1;
	}

	c->descs = calloc(maxids, sizeof(*c->descs));
	c->ids = calloc(maxids, sizeof(u16));
	c->stores = calloc(maxids, NETSNAP_STORE);

	if (!c->descs || !c->ids || !c->stores) {
		netBaselineFree(&c->bl);
		free(c->descs);
		free(c->ids);
		free(c->stores);
		c->descs = NULL;
		c->ids = NULL;
		c->stores = NULL;
		return -1;
	}

	c->maxids = maxids;

	return 0;
}

const struct netsnapinfo *netSnapClientInfo(const struct netsnapclient *c, u16 seq)
{
	const struct netsnapinfo *info;

	if (seq == 0 || !c->maxids) {
		return NULL;
	}

	info = &c->info[seq % NETBASELINE_SLOTS];

	return info->seq == seq && netSlotOf(&c->bl, seq) ? info : NULL;
}

s32 netSnapClientBracket(const struct netsnapclient *c, u32 hosttick, u16 *before, u16 *after)
{
	u32 bt = 0;
	u32 at = 0;
	s32 i;

	*before = 0;
	*after = 0;

	for (i = 0; i < NETBASELINE_SLOTS && c->maxids; i++) {
		const struct netsnapinfo *info = &c->info[i];

		if (!info->seq || !netSlotOf(&c->bl, info->seq)) {
			continue;
		}

		if (info->hosttick <= hosttick) {
			if (!*before || info->hosttick > bt) {
				*before = info->seq;
				bt = info->hosttick;
			}
		} else if (!*after || info->hosttick < at) {
			*after = info->seq;
			at = info->hosttick;
		}
	}

	return *before || *after;
}

static s32 netSnapClientFail(struct netsnapclient *c)
{
	c->malformed++;
	return -1;
}

s32 netSnapClientDecode(struct netsnapclient *c, struct netbuf *b, u32 matchid, struct netsnaphdr *hdr)
{
	const struct netbaselineslot *base = NULL;
	const struct netsnapinfo *baseinfo = NULL;
	struct netsnapinfo *info;
	s32 nbytes;
	s32 ndescs;
	s32 n = 0;
	s32 d = 0;
	s32 id;
	s32 prev = -1;
	s32 i;
	s32 haslp;
	u8 lp[NETLP_SIZE];
	s32 entkeys = 0;

	c->received++;
	c->bytes += b->size;
	c->ndescs = 0;

	memset(hdr, 0, sizeof(*hdr));
	hdr->matchid = netBufReadU32(b);
	hdr->seq = netBufReadU16(b);
	hdr->baseline = netBufReadU16(b);
	hdr->hosttick = netBufReadU32(b);
	hdr->lvupdate240 = netBufReadU8(b);
	hdr->rate = netBufReadU8(b);
	hdr->lastcmd = netBufReadU32(b);
	hdr->maxids = netBufReadU16(b);

	if (!netBufOk(b) || hdr->seq == 0 || hdr->maxids == 0 || hdr->maxids > NETSNAP_MAXIDS) {
		return netSnapClientFail(c);
	}

	if (hdr->matchid != matchid) {
		c->otherMatch++;
		return 0;
	}

	if (c->maxids == 0) {
		if (netSnapClientAlloc(c, hdr->maxids) != 0) {
			return netSnapClientFail(c);
		}
	} else if (c->maxids != hdr->maxids) {
		return netSnapClientFail(c);
	}

	if (c->newest && !netSeqNewer(hdr->seq, c->newest)) {
		c->old++;

		if (c->probe || ++c->oldrun < NETSNAP_RESYNC) {
			return 0;
		}

		// Everything has been old for a while: the newest held came from a
		// corrupt or forged seq the host never sent. Start over from a
		// keyframe (this packet may be one).
		netBaselineReset(&c->bl);
		memset(c->info, 0, sizeof(c->info));
		c->newest = 0;
		c->ackbits = 0;
		c->wantkey = 1;
		c->oldrun = 0;
		c->resyncs++;
	}

	if (hdr->baseline) {
		// the host holds 64 snapshots, so its baseline is never further back
		if (!netSeqNewer(hdr->seq, hdr->baseline) || netSeqDiff(hdr->seq, hdr->baseline) > NETBASELINE_SLOTS) {
			return netSnapClientFail(c);
		}

		base = netSlotOf(&c->bl, hdr->baseline);
		baseinfo = netSnapClientInfo(c, hdr->baseline);

		if (!base || !baseinfo) {
			// a baseline gone from here: ask for everything again
			c->nobase++;
			c->wantkey = 1;
			return 0;
		}
	}

	nbytes = (c->maxids + 7) / 8;
	memset(s_BasePresent, 0, nbytes);

	if (base) {
		for (i = 0; i < base->count; i++) {
			netBitSet(s_BasePresent, base->ids[i]);
		}
	}

	netDeltaRead(b, base ? s_BasePresent : NULL, s_Present, nbytes);
	netDeltaRead(b, NULL, s_Updated, nbytes);

	if (!netBufOk(b)) {
		return netSnapClientFail(c);
	}

	// nothing past the last id, and nothing updated that is not present
	for (i = 0; i < nbytes; i++) {
		if (s_Updated[i] & ~s_Present[i]) {
			return netSnapClientFail(c);
		}
	}

	if ((c->maxids & 7) && (s_Present[nbytes - 1] >> (c->maxids & 7))) {
		return netSnapClientFail(c);
	}

	ndescs = (s32)netBufReadVarU32(b);

	if (!netBufOk(b) || ndescs < 0 || ndescs > c->maxids) {
		return netSnapClientFail(c);
	}

	for (i = 0; i < ndescs; i++) {
		const u32 gap = netBufReadVarU32(b);

		if (!netBufOk(b) || gap >= (u32)c->maxids) {
			return netSnapClientFail(c);
		}

		id = prev + 1 + (s32)gap;

		if (id >= c->maxids || !netBitGet(s_Present, id)) {
			return netSnapClientFail(c);
		}

		c->descs[i].id = (u16)id;
		netDescRead(b, &c->descs[i].desc);

		if (!netBufOk(b)) {
			return netSnapClientFail(c);
		}

		prev = id;
	}

	// every present entity, rising: the baseline's record, or what came
	for (id = 0; id < c->maxids; id++) {
		const u8 *bstore;
		const struct netdesc *desc = NULL;
		u8 *store;
		s32 keyframe;
		s32 rec;
		s32 size;

		if (!netBitGet(s_Present, id)) {
			continue;
		}

		bstore = netStoreFind(base, c->bl.recordsize, (u16)id);

		if (d < ndescs && c->descs[d].id == id) {
			desc = &c->descs[d].desc;
			d++;
		}

		store = c->stores + (size_t)n * NETSNAP_STORE;
		memset(store, 0, NETSNAP_STORE);

		if (desc) {
			keyframe = !bstore || netStoreGen(bstore) != desc->gen || netStoreRec(bstore) != desc->rec
				|| netStoreKind(bstore) != desc->kind;
			rec = desc->rec;
			store[0] = (u8)desc->gen;
			store[1] = (u8)(desc->gen >> 8);
			store[2] = desc->rec;
			store[3] = desc->kind;
		} else {
			if (!bstore) {
				// present, new here, and no descriptor
				return netSnapClientFail(c);
			}

			keyframe = 0;
			rec = netStoreRec(bstore);
			memcpy(store, bstore, NETSNAP_STOREHDR);
		}

		size = netRecSize(rec);

		if (size == 0) {
			return netSnapClientFail(c);
		}

		if (netBitGet(s_Updated, id)) {
			netDeltaRead(b, keyframe ? NULL : bstore + NETSNAP_STOREHDR, store + NETSNAP_STOREHDR, size);

			if (!netBufOk(b)) {
				return netSnapClientFail(c);
			}

			entkeys += keyframe;
		} else {
			if (keyframe) {
				// a new entity must come with its record
				return netSnapClientFail(c);
			}

			memcpy(store + NETSNAP_STOREHDR, bstore + NETSNAP_STOREHDR, NETREC_MAX);
		}

		c->ids[n] = (u16)id;
		n++;
	}

	haslp = netBufReadU8(b);

	if (haslp == 1) {
		netDeltaRead(b, NULL, lp, NETLP_SIZE);
	} else if (haslp == 2) {
		if (!baseinfo || !baseinfo->haslp) {
			return netSnapClientFail(c);
		}

		netDeltaRead(b, baseinfo->lp, lp, NETLP_SIZE);
	} else if (haslp != 0) {
		return netSnapClientFail(c);
	}

	if (!netBufOk(b) || netBufRemaining(b) != 0) {
		return netSnapClientFail(c);
	}

	// all of it checks out: keep it
	if (c->probe) {
		return 1;
	}

	if (netBaselineBegin(&c->bl, hdr->seq) != 0) {
		return netSnapClientFail(c);
	}

	for (i = 0; i < n; i++) {
		netBaselineAdd(&c->bl, c->ids[i], c->stores + (size_t)i * NETSNAP_STORE);
	}

	info = &c->info[hdr->seq % NETBASELINE_SLOTS];
	info->seq = hdr->seq;
	info->hosttick = hdr->hosttick;
	info->lastcmd = hdr->lastcmd;
	info->lvupdate240 = hdr->lvupdate240;
	info->rate = hdr->rate;
	info->haslp = haslp != 0;

	if (haslp) {
		memcpy(info->lp, lp, NETLP_SIZE);
	}

	if (c->newest == 0) {
		c->ackbits = 0;
	} else {
		const s32 diff = netSeqDiff(hdr->seq, c->newest);

		c->ackbits = diff >= 32 ? 0 : (c->ackbits << diff);

		if (diff - 1 < 32) {
			c->ackbits |= 1u << (diff - 1);
		}
	}

	c->newest = hdr->seq;
	c->oldrun = 0;
	c->ndescs = ndescs;
	c->decoded++;
	c->entkeys += entkeys;

	if (hdr->baseline == 0) {
		c->keyframes++;
		c->wantkey = 0;
	}

	return 1;
}

void netSnapClientAck(const struct netsnapclient *c, struct netsnapack *a)
{
	memset(a, 0, sizeof(*a));
	a->seq = c->newest;
	a->bits = c->ackbits;
	a->flags = c->wantkey ? NETSNAPACK_WANTKEY : 0;
}
