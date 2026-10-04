#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netdelta.h"

/**
 * See netdelta.h for the wire format and what the baseline ring is for.
 */

static inline u8 deltaByte(const u8 *cur, const u8 *base, s32 i)
{
	return base ? cur[i] ^ base[i] : cur[i];
}

s32 netDeltaEncode(const u8 *cur, const u8 *base, s32 len, u8 *out, s32 outcap)
{
	s32 last;
	s32 i = 0;
	s32 op = 0;

	if (cur == NULL || out == NULL || len <= 0 || len > NETDELTA_MAXRECORD) {
		return -1;
	}

	// Everything after the last changed byte is left to the end byte
	for (last = len - 1; last >= 0 && deltaByte(cur, base, last) == 0; last--);

	while (i <= last) {
		s32 z = 0;
		s32 c = 0;
		s32 start;
		s32 k;

		while (z < 255 && deltaByte(cur, base, i + z) == 0) {
			z++;
		}

		start = i + z;

		// A literal run carries on over a lone unchanged byte: one zero
		// inside a run costs a byte, splitting the run there costs a control
		while (c < 127 && start + c <= last) {
			if (deltaByte(cur, base, start + c) != 0
					|| (start + c + 1 <= last && deltaByte(cur, base, start + c + 1) != 0)) {
				c++;
			} else {
				break;
			}
		}

		if (z <= 7 && c <= 15) {
			if (op + 1 + c > outcap) {
				return -1;
			}

			out[op++] = (z << 4) | c;
		} else {
			if (op + 2 + c > outcap) {
				return -1;
			}

			out[op++] = 0x80 | c;
			out[op++] = z;
		}

		for (k = 0; k < c; k++) {
			out[op++] = deltaByte(cur, base, start + k);
		}

		i = start + c;
	}

	if (op + 1 > outcap) {
		return -1;
	}

	out[op++] = 0x00;

	return op;
}

s32 netDeltaDecode(const u8 *in, s32 inlen, const u8 *base, u8 *out, s32 len)
{
	s32 ip = 0;
	s32 pos = 0;
	s32 k;

	if (in == NULL || out == NULL || inlen <= 0 || len <= 0 || len > NETDELTA_MAXRECORD) {
		return -1;
	}

	while (1) {
		u8 ctl;
		s32 z;
		s32 c;

		if (ip >= inlen) {
			return -1;
		}

		ctl = in[ip++];

		if (ctl == 0x00) {
			break;
		}

		if (ctl & 0x80) {
			if (ip >= inlen) {
				return -1;
			}

			c = ctl & 0x7f;
			z = in[ip++];

			if (c == 0 && z == 0) {
				return -1;
			}
		} else {
			z = ctl >> 4;
			c = ctl & 0x0f;
		}

		if (z + c > len - pos || c > inlen - ip) {
			return -1;
		}

		for (k = 0; k < z; k++, pos++) {
			out[pos] = base ? base[pos] : 0;
		}

		for (k = 0; k < c; k++, pos++) {
			out[pos] = base ? base[pos] ^ in[ip++] : in[ip++];
		}
	}

	for (; pos < len; pos++) {
		out[pos] = base ? base[pos] : 0;
	}

	return ip;
}

void netDeltaWrite(struct netbuf *b, const u8 *cur, const u8 *base, s32 len)
{
	s32 written;

	if (b->error) {
		return;
	}

	written = netDeltaEncode(cur, base, len, b->data + b->pos, b->size - b->pos);

	if (written < 0) {
		b->error = 1;
		return;
	}

	netBufReserve(b, written);
}

void netDeltaRead(struct netbuf *b, const u8 *base, u8 *out, s32 len)
{
	s32 used;

	if (b->error) {
		if (out && len > 0) {
			memset(out, 0, len);
		}
		return;
	}

	used = netDeltaDecode(b->data + b->pos, b->size - b->pos, base, out, len);

	if (used < 0) {
		b->error = 1;
		return;
	}

	netBufSkip(b, used);
}

/**
 * The baseline ring.
 *
 * Each slot holds one snapshot's entity ids, rising, and their records side
 * by side. ackedseq[] remembers, per entity, the newest acked snapshot that
 * held it, so the host's question - what does this client have for entity
 * N - is one lookup and a binary search rather than a walk over 64 slots.
 *
 * ackedseq[] only ever names a live slot: when a slot is recycled or falls
 * 64 behind, every entity it held that pointed at it is cleared. That keeps
 * every sequence compared within 64 of the newest, well inside the half of
 * the u16 circle where netSeqNewer can tell older from newer.
 */

s32 netBaselineInit(struct netbaseline *bl, s32 maxentities, s32 maxpersnap, s32 recordsize)
{
	s32 i;

	memset(bl, 0, sizeof(*bl));

	if (maxentities <= 0 || maxentities > 0x10000 || maxpersnap <= 0 || maxpersnap > maxentities
			|| recordsize <= 0 || recordsize > NETDELTA_MAXRECORD) {
		return -1;
	}

	bl->maxentities = maxentities;
	bl->maxpersnap = maxpersnap;
	bl->recordsize = recordsize;
	bl->building = -1;
	bl->ackedseq = calloc(maxentities, sizeof(u16));

	if (!bl->ackedseq) {
		netBaselineFree(bl);
		return -1;
	}

	for (i = 0; i < NETBASELINE_SLOTS; i++) {
		bl->slots[i].ids = calloc(maxpersnap, sizeof(u16));
		bl->slots[i].records = calloc(maxpersnap, recordsize);

		if (!bl->slots[i].ids || !bl->slots[i].records) {
			netBaselineFree(bl);
			return -1;
		}
	}

	return 0;
}

void netBaselineFree(struct netbaseline *bl)
{
	s32 i;

	free(bl->ackedseq);

	for (i = 0; i < NETBASELINE_SLOTS; i++) {
		free(bl->slots[i].ids);
		free(bl->slots[i].records);
	}

	memset(bl, 0, sizeof(*bl));
	bl->building = -1;
}

void netBaselineReset(struct netbaseline *bl)
{
	s32 i;

	if (!bl->ackedseq) {
		return;
	}

	for (i = 0; i < NETBASELINE_SLOTS; i++) {
		bl->slots[i].seq = 0;
		bl->slots[i].valid = 0;
		bl->slots[i].acked = 0;
		bl->slots[i].count = 0;
	}

	memset(bl->ackedseq, 0, bl->maxentities * sizeof(u16));
	bl->newest = 0;
	bl->building = -1;
}

static s32 baselineFindEntity(const struct netbaselineslot *slot, u16 entity);

/**
 * Takes the entity out of every snapshot held as well as clearing its ack,
 * so an ack still in flight for an older snapshot cannot bring the old
 * record back as its baseline.
 */
void netBaselineForget(struct netbaseline *bl, u16 entity)
{
	s32 i;

	if (!bl->ackedseq || entity >= bl->maxentities) {
		return;
	}

	bl->ackedseq[entity] = 0;

	for (i = 0; i < NETBASELINE_SLOTS; i++) {
		struct netbaselineslot *slot = &bl->slots[i];
		s32 index;
		s32 after;

		if (!slot->valid) {
			continue;
		}

		index = baselineFindEntity(slot, entity);

		if (index < 0) {
			continue;
		}

		after = slot->count - index - 1;

		memmove(&slot->ids[index], &slot->ids[index + 1], after * sizeof(u16));
		memmove(slot->records + (size_t)index * bl->recordsize,
				slot->records + (size_t)(index + 1) * bl->recordsize,
				(size_t)after * bl->recordsize);
		slot->count--;
	}
}

static void baselineDropSlot(struct netbaseline *bl, struct netbaselineslot *slot)
{
	s32 i;

	if (!slot->valid) {
		return;
	}

	if (slot->acked) {
		for (i = 0; i < slot->count; i++) {
			if (bl->ackedseq[slot->ids[i]] == slot->seq) {
				bl->ackedseq[slot->ids[i]] = 0;
			}
		}
	}

	slot->valid = 0;
	slot->acked = 0;
	slot->count = 0;
	slot->seq = 0;
}

static const struct netbaselineslot *baselineFindSlot(const struct netbaseline *bl, u16 seq)
{
	const struct netbaselineslot *slot;

	if (seq == 0 || !bl->ackedseq) {
		return NULL;
	}

	slot = &bl->slots[seq % NETBASELINE_SLOTS];

	return slot->valid && slot->seq == seq ? slot : NULL;
}

static s32 baselineFindEntity(const struct netbaselineslot *slot, u16 entity)
{
	s32 lo = 0;
	s32 hi = slot->count - 1;

	while (lo <= hi) {
		s32 mid = (lo + hi) / 2;

		if (slot->ids[mid] == entity) {
			return mid;
		}

		if (slot->ids[mid] < entity) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return -1;
}

/**
 * Starts storing snapshot seq. It must be nonzero and newer than the last
 * one stored (a client drops a snapshot that arrives behind a newer one
 * before it gets here). Returns 0, or -1 if refused.
 */
s32 netBaselineBegin(struct netbaseline *bl, u16 seq)
{
	struct netbaselineslot *slot;
	s32 i;

	bl->building = -1;

	if (!bl->ackedseq || seq == 0 || (bl->newest != 0 && !netSeqNewer(seq, bl->newest))) {
		return -1;
	}

	// Whatever is now 64 or more behind goes, the slot about to be reused
	// with it
	for (i = 0; i < NETBASELINE_SLOTS; i++) {
		slot = &bl->slots[i];

		if (slot->valid && netSeqDiff(seq, slot->seq) >= NETBASELINE_SLOTS) {
			baselineDropSlot(bl, slot);
		}
	}

	slot = &bl->slots[seq % NETBASELINE_SLOTS];
	baselineDropSlot(bl, slot);

	slot->seq = seq;
	slot->valid = 1;
	slot->acked = 0;
	slot->count = 0;

	bl->newest = seq;
	bl->building = seq % NETBASELINE_SLOTS;

	return 0;
}

/**
 * Adds entity's record to the snapshot being stored. Ids must rise. Returns
 * 0, or -1 if refused (no snapshot begun, slot full, id out of range or not
 * above the last).
 */
s32 netBaselineAdd(struct netbaseline *bl, u16 entity, const u8 *record)
{
	struct netbaselineslot *slot;

	if (bl->building < 0 || entity >= bl->maxentities || record == NULL) {
		return -1;
	}

	slot = &bl->slots[bl->building];

	if (slot->count >= bl->maxpersnap || (slot->count > 0 && slot->ids[slot->count - 1] >= entity)) {
		return -1;
	}

	slot->ids[slot->count] = entity;
	memcpy(slot->records + (size_t)slot->count * bl->recordsize, record, bl->recordsize);
	slot->count++;

	// A snapshot acked while still being built would be a caller bug; an
	// entity added after an ack would not be covered by it. Not supported.
	return 0;
}

s32 netBaselineAck(struct netbaseline *bl, u16 seq)
{
	struct netbaselineslot *slot = (struct netbaselineslot *)baselineFindSlot(bl, seq);
	s32 i;

	if (!slot) {
		return 0;
	}

	if (slot->acked) {
		return 1;
	}

	slot->acked = 1;

	for (i = 0; i < slot->count; i++) {
		u16 *acked = &bl->ackedseq[slot->ids[i]];

		if (*acked == 0 || netSeqNewer(seq, *acked)) {
			*acked = seq;
		}
	}

	return 1;
}

const u8 *netBaselineGetAcked(const struct netbaseline *bl, u16 entity, u16 *seqout)
{
	u16 seq;
	const u8 *record;

	if (seqout) {
		*seqout = 0;
	}

	if (!bl->ackedseq || entity >= bl->maxentities) {
		return NULL;
	}

	seq = bl->ackedseq[entity];
	record = netBaselineGet(bl, seq, entity);

	if (record && seqout) {
		*seqout = seq;
	}

	return record;
}

const u8 *netBaselineGet(const struct netbaseline *bl, u16 seq, u16 entity)
{
	const struct netbaselineslot *slot = baselineFindSlot(bl, seq);
	s32 index;

	if (!slot) {
		return NULL;
	}

	index = baselineFindEntity(slot, entity);

	return index >= 0 ? slot->records + (size_t)index * bl->recordsize : NULL;
}
