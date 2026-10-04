#ifndef _IN_NET_NETDELTA_H
#define _IN_NET_NETDELTA_H

#include <PR/ultratypes.h>
#include "net/netbuf.h"

/**
 * The snapshot delta codec: an entity's state record XOR'd against the copy
 * of it the peer is known to hold, then the zero runs squeezed out. The idea
 * is EQOA's (a record that did not change is all zeros after the XOR, and
 * one that changed a little is mostly zeros); the code and the format are
 * our own.
 *
 * Records are opaque fixed-size byte arrays. Which record, against which
 * baseline, is the caller's header's business: the caller writes the
 * baseline's snapshot sequence beside each record, and sequence 0 means
 * "keyframe, no baseline" - the record is XOR'd with zeros, i.e. sent as it
 * is. A keyframe is always flagged that way, never guessed from the bytes.
 *
 * WIRE FORMAT (one encoded record)
 *
 * A run of control bytes, each followed by its literal bytes, ending with a
 * 0x00 control byte. Decoding fills the XOR delta from the front; each
 * control says to skip Z bytes (they are zero in the delta: unchanged from
 * the baseline) and then take C literal bytes (the delta's bytes, i.e.
 * current XOR baseline).
 *
 *   0x00            end. Every delta byte not yet filled is zero.
 *   0b0zzzcccc      short: Z = zzz (0-7), C = cccc (0-15); not both zero
 *                   (that is the end byte)
 *   0b1ccccccc zz   long: C = ccccccc (0-127), Z = the following byte
 *                   (0-255); not both zero
 *   then C literal bytes.
 *
 * So an unchanged record is the one byte 0x00. A record of N bytes encodes in
 * at most NETDELTA_MAXENCODED(N) bytes. A decoder refuses (returns -1) a
 * control that would fill past the record's end, a zero-length long control,
 * and input that ends before the end byte; it never writes outside the
 * record and never reads outside the input. Bytes after the end byte are not
 * looked at: the return value says where the next thing starts.
 */

#define NETDELTA_MAXRECORD 4096

// Worst case: every byte changed, so literal runs of 127 behind a two-byte
// control, plus the end byte
#define NETDELTA_MAXENCODED(len) ((len) + 2 * (((len) + 126) / 127) + 1)

/**
 * Encodes cur against base (NULL: a keyframe, against zeros) into out.
 * Returns the bytes written, or -1 if they would not fit in outcap or len is
 * not 1..NETDELTA_MAXRECORD.
 */
s32 netDeltaEncode(const u8 *cur, const u8 *base, s32 len, u8 *out, s32 outcap);

/**
 * Decodes in against base (NULL: a keyframe) into out, len bytes. out may be
 * the same buffer as base, but on failure its contents are then undefined;
 * decode into scratch first if the baseline must survive bad input. in must
 * not overlap out. Returns the input bytes used, or -1 if the input is
 * malformed.
 */
s32 netDeltaDecode(const u8 *in, s32 inlen, const u8 *base, u8 *out, s32 len);

/**
 * The same through a netbuf: written at its position, or read from it and
 * stepped past. A failure sets the netbuf's error.
 */
void netDeltaWrite(struct netbuf *b, const u8 *cur, const u8 *base, s32 len);
void netDeltaRead(struct netbuf *b, const u8 *base, u8 *out, s32 len);

/**
 * Snapshot sequence numbers: u16, wrapping, 0 never used (it is the keyframe
 * marker). Newer means "ahead by less than half the circle".
 */
static inline u16 netSeqNext(u16 seq) { seq++; return seq ? seq : 1; }
static inline s32 netSeqNewer(u16 a, u16 b) { return (s16)(u16)(a - b) > 0; }
static inline s32 netSeqDiff(u16 a, u16 b) { return (s16)(u16)(a - b); }

/**
 * A per-peer ring of the last NETBASELINE_SLOTS snapshots, by sequence: which
 * entities each carried and the record each had. The host keeps one per
 * client and encodes against the newest record the client acked; a client
 * keeps one of what it decoded and looks a record up by the sequence the
 * host named.
 *
 * Entities are numbered 0..maxentities-1 by the caller (phase 4's entity
 * table). A snapshot is stored with netBaselineBegin and one netBaselineAdd
 * per entity in rising id order, its sequence newer than the last one
 * stored. A slot is recycled when a snapshot 64 sequences on is stored, and
 * whatever was acked in it goes with it: an entity whose newest acked record
 * has gone is sent as a keyframe again.
 */

#define NETBASELINE_SLOTS 64

struct netbaselineslot {
	u16 seq;
	u8 valid;
	u8 acked;
	s32 count;
	u16 *ids;    // [maxpersnap], rising
	u8 *records; // [maxpersnap * recordsize]
};

struct netbaseline {
	s32 maxentities;
	s32 maxpersnap;
	s32 recordsize;
	u16 newest;   // last sequence stored, 0 for none
	s32 building; // the slot netBaselineAdd fills, -1 when none
	u16 *ackedseq; // [maxentities]: newest acked sequence holding it, 0 for none
	struct netbaselineslot slots[NETBASELINE_SLOTS];
};

s32 netBaselineInit(struct netbaseline *bl, s32 maxentities, s32 maxpersnap, s32 recordsize);
void netBaselineFree(struct netbaseline *bl);

// Forgets every snapshot and every ack: everything goes as a keyframe next
void netBaselineReset(struct netbaseline *bl);

// Forgets one entity (it was freed, or its id reused): out of every snapshot
// held and its ack cleared, so it goes as a keyframe next
void netBaselineForget(struct netbaseline *bl, u16 entity);

s32 netBaselineBegin(struct netbaseline *bl, u16 seq);
s32 netBaselineAdd(struct netbaseline *bl, u16 entity, const u8 *record);

// The peer says it has snapshot seq. Returns 1 if that snapshot is still held
s32 netBaselineAck(struct netbaseline *bl, u16 seq);

// The newest record of entity the peer acked, and its sequence; NULL if none
const u8 *netBaselineGetAcked(const struct netbaseline *bl, u16 entity, u16 *seqout);

// entity's record in snapshot seq, acked or not; NULL if not held
const u8 *netBaselineGet(const struct netbaseline *bl, u16 seq, u16 entity);

#endif
