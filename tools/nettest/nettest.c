/**
 * pd-nettest: the netplay transport and codec on their own, no game.
 *
 * Built from port/src/net/{netbuf,netdelta,nettransport}.c and
 * port/external/enet.c only (the pd-nettest target in CMakeLists.txt). It
 * lives here rather than under port/ because the game globs every .c there.
 *
 * Prints PASS or FAIL per test and exits nonzero if any failed. Everything
 * random is seeded, so a failure repeats.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netdelta.h"
#include "net/nettransport.h"

static s32 g_Failures = 0;
static s32 g_Checks = 0;
static const char *g_Test = "";

#define CHECK(cond) do { \
	g_Checks++; \
	if (!(cond)) { \
		if (testfails < 10) { \
			printf("  %s:%d: %s: check failed: %s\n", __FILE__, __LINE__, g_Test, #cond); \
		} \
		testfails++; \
	} \
} while (0)

#define TEST_BEGIN(name) s32 testfails = 0; g_Test = name
#define TEST_END() do { \
	printf("%s %s\n", testfails ? "FAIL" : "PASS", g_Test); \
	if (testfails) g_Failures++; \
	return testfails; \
} while (0)

static u32 g_Rng = 12345;

// The loopback test's simulator settings: --loss, --delay, --jitter override
static s32 g_SimLoss = 20;
static s32 g_SimDelay = 10;
static s32 g_SimJitter = 20;

static u32 rnd(void)
{
	// xorshift32
	g_Rng ^= g_Rng << 13;
	g_Rng ^= g_Rng >> 17;
	g_Rng ^= g_Rng << 5;
	return g_Rng;
}

static u32 rndRange(u32 n)
{
	return n ? rnd() % n : 0;
}

/**
 * netbuf
 */

static s32 testNetBufRoundTrip(void)
{
	TEST_BEGIN("netbuf round trip");
	u8 data[256];
	char str[32];
	struct netbuf w;
	struct netbuf r;
	static const u32 varints[] = { 0, 1, 127, 128, 255, 16383, 16384, 0x1fffff, 0x200000, 0x0fffffff, 0x10000000, 0xffffffff };
	s32 i;

	netBufInitWrite(&w, data, sizeof(data));
	netBufWriteU8(&w, 0xab);
	netBufWriteU16(&w, 0xbeef);
	netBufWriteU32(&w, 0xdeadbeef);
	netBufWriteS8(&w, -5);
	netBufWriteS16(&w, -12345);
	netBufWriteS32(&w, -123456789);
	netBufWriteF32(&w, -1.5f);
	netBufWriteF32(&w, 0.1f);

	for (i = 0; i < (s32)(sizeof(varints) / sizeof(varints[0])); i++) {
		netBufWriteVarU32(&w, varints[i]);
	}

	netBufWriteString(&w, "Joanna", 16);
	netBufWriteString(&w, "", 16);
	netBufWriteBits(&w, 0x5, 3);
	netBufWriteBits(&w, 0x1ff, 9);
	netBufWriteBits(&w, 0xdeadbeef, 32);
	netBufWriteU8(&w, 0x77); // byte-aligns after the bit run
	netBufWriteBits(&w, 1, 1);
	netBufWriteBytes(&w, "xyz", 3);
	CHECK(netBufOk(&w));

	// Exact little-endian layout of the first fields
	CHECK(data[0] == 0xab);
	CHECK(data[1] == 0xef && data[2] == 0xbe);
	CHECK(data[3] == 0xef && data[4] == 0xbe && data[5] == 0xad && data[6] == 0xde);

	netBufInitRead(&r, data, netBufLen(&w));
	CHECK(netBufReadU8(&r) == 0xab);
	CHECK(netBufReadU16(&r) == 0xbeef);
	CHECK(netBufReadU32(&r) == 0xdeadbeef);
	CHECK(netBufReadS8(&r) == -5);
	CHECK(netBufReadS16(&r) == -12345);
	CHECK(netBufReadS32(&r) == -123456789);
	CHECK(netBufReadF32(&r) == -1.5f);
	CHECK(netBufReadF32(&r) == 0.1f);

	for (i = 0; i < (s32)(sizeof(varints) / sizeof(varints[0])); i++) {
		CHECK(netBufReadVarU32(&r) == varints[i]);
	}

	CHECK(netBufReadString(&r, str, sizeof(str)) == 6 && strcmp(str, "Joanna") == 0);
	CHECK(netBufReadString(&r, str, sizeof(str)) == 0 && str[0] == '\0');
	CHECK(netBufReadBits(&r, 3) == 0x5);
	CHECK(netBufReadBits(&r, 9) == 0x1ff);
	CHECK(netBufReadBits(&r, 32) == 0xdeadbeef);
	CHECK(netBufReadU8(&r) == 0x77);
	CHECK(netBufReadBits(&r, 1) == 1);
	netBufReadBytes(&r, str, 3);
	CHECK(memcmp(str, "xyz", 3) == 0);
	CHECK(netBufOk(&r));
	CHECK(netBufRemaining(&r) == 0);

	// Random mixed sequences
	for (i = 0; i < 2000; i++) {
		u8 buf[512];
		u32 vals[64];
		s32 kinds[64];
		s32 n = 1 + rndRange(64);
		s32 k;

		netBufInitWrite(&w, buf, sizeof(buf));

		for (k = 0; k < n; k++) {
			kinds[k] = rndRange(5);
			vals[k] = rnd() >> rndRange(32);

			switch (kinds[k]) {
			case 0: netBufWriteU8(&w, vals[k]); vals[k] &= 0xff; break;
			case 1: netBufWriteU16(&w, vals[k]); vals[k] &= 0xffff; break;
			case 2: netBufWriteU32(&w, vals[k]); break;
			case 3: netBufWriteVarU32(&w, vals[k]); break;
			case 4: netBufWriteBits(&w, vals[k] & 0x7ff, 11); vals[k] &= 0x7ff; break;
			}
		}

		CHECK(netBufOk(&w));
		netBufInitRead(&r, buf, netBufLen(&w));

		for (k = 0; k < n; k++) {
			u32 v = 0;

			switch (kinds[k]) {
			case 0: v = netBufReadU8(&r); break;
			case 1: v = netBufReadU16(&r); break;
			case 2: v = netBufReadU32(&r); break;
			case 3: v = netBufReadVarU32(&r); break;
			case 4: v = netBufReadBits(&r, 11); break;
			}

			CHECK(v == vals[k]);
		}

		CHECK(netBufOk(&r) && netBufRemaining(&r) == 0);
	}

	TEST_END();
}

static s32 testNetBufOverflow(void)
{
	TEST_BEGIN("netbuf overflow and malformed input");
	u8 data[8];
	u8 guard[16];
	char str[8];
	struct netbuf w;
	struct netbuf r;

	memset(guard, 0xcc, sizeof(guard));

	// A write that does not fit sets the error, writes nothing, and sticks
	netBufInitWrite(&w, data, 5);
	netBufWriteU32(&w, 1);
	CHECK(netBufOk(&w));
	netBufWriteU16(&w, 2);
	CHECK(!netBufOk(&w));
	CHECK(netBufLen(&w) == 4);
	netBufWriteU8(&w, 3); // would fit, but the error sticks
	CHECK(netBufLen(&w) == 4);

	// Bits past the end
	netBufInitWrite(&w, data, 1);
	netBufWriteBits(&w, 0xff, 8);
	CHECK(netBufOk(&w));
	netBufWriteBits(&w, 1, 1);
	CHECK(!netBufOk(&w));

	// A string over its bound is an error, not truncated
	netBufInitWrite(&w, data, sizeof(data));
	netBufWriteString(&w, "toolongstring", 4);
	CHECK(!netBufOk(&w));
	CHECK(netBufLen(&w) == 0);

	// Reads past the end return zero and set the error
	data[0] = 0x12;
	data[1] = 0x34;
	netBufInitRead(&r, data, 2);
	CHECK(netBufReadU32(&r) == 0);
	CHECK(!netBufOk(&r));
	CHECK(netBufReadU8(&r) == 0); // sticky even though a byte is there

	netBufInitRead(&r, data, 2);
	CHECK(netBufReadU16(&r) == 0x3412);
	CHECK(netBufReadU8(&r) == 0);
	CHECK(!netBufOk(&r));

	netBufInitRead(&r, data, 2);
	netBufReadBytes(&r, guard, 3); // fails, zero-fills, writes no further
	CHECK(!netBufOk(&r));
	CHECK(guard[0] == 0 && guard[1] == 0 && guard[2] == 0 && guard[3] == 0xcc);

	// Varint too long, and a fifth byte that would overflow
	{
		u8 bad1[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
		u8 bad2[5] = { 0xff, 0xff, 0xff, 0xff, 0x1f };
		u8 good[5] = { 0xff, 0xff, 0xff, 0xff, 0x0f };
		u8 cut[2] = { 0x80, 0x80 };

		netBufInitRead(&r, bad1, sizeof(bad1));
		CHECK(netBufReadVarU32(&r) == 0 && !netBufOk(&r));
		netBufInitRead(&r, bad2, sizeof(bad2));
		CHECK(netBufReadVarU32(&r) == 0 && !netBufOk(&r));
		netBufInitRead(&r, good, sizeof(good));
		CHECK(netBufReadVarU32(&r) == 0xffffffff && netBufOk(&r));
		netBufInitRead(&r, cut, sizeof(cut));
		CHECK(netBufReadVarU32(&r) == 0 && !netBufOk(&r));
	}

	// Strings: too long for the destination, length past the data, embedded NUL
	{
		u8 s1[] = { 8, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
		u8 s2[] = { 200, 'a', 'b' };
		u8 s3[] = { 3, 'a', 0, 'b' };
		u8 s4[] = { 7, 'a', 'b', 'c', 'd', 'e', 'f', 'g' };

		netBufInitRead(&r, s1, sizeof(s1));
		CHECK(netBufReadString(&r, str, sizeof(str)) == -1 && str[0] == '\0' && !netBufOk(&r));
		netBufInitRead(&r, s2, sizeof(s2));
		CHECK(netBufReadString(&r, str, sizeof(str)) == -1 && !netBufOk(&r));
		netBufInitRead(&r, s3, sizeof(s3));
		CHECK(netBufReadString(&r, str, sizeof(str)) == -1 && !netBufOk(&r));
		netBufInitRead(&r, s4, sizeof(s4));
		CHECK(netBufReadString(&r, str, sizeof(str)) == 7 && strcmp(str, "abcdefg") == 0 && netBufOk(&r));
	}

	// Empty and NULL buffers
	netBufInitRead(&r, NULL, 10);
	CHECK(netBufReadU8(&r) == 0 && !netBufOk(&r));
	netBufInitWrite(&w, NULL, 10);
	netBufWriteU8(&w, 1);
	CHECK(!netBufOk(&w));

	TEST_END();
}

/**
 * netdelta codec
 */

static void mutate(u8 *rec, s32 len)
{
	s32 kind = rndRange(5);
	s32 n;
	s32 i;

	switch (kind) {
	case 0: // a few scattered bytes
		n = 1 + rndRange(4);
		for (i = 0; i < n; i++) {
			rec[rndRange(len)] ^= 1 + rndRange(255);
		}
		break;
	case 1: // a contiguous run
		n = 1 + rndRange(len);
		i = rndRange(len - n + 1);
		for (; n > 0; n--, i++) {
			rec[i] = rnd();
		}
		break;
	case 2: // a little-endian counter ticking over
		if (len >= 4) {
			u32 v;
			i = rndRange(len - 3);
			v = rec[i] | (rec[i + 1] << 8) | (rec[i + 2] << 16) | ((u32)rec[i + 3] << 24);
			v += 1 + rndRange(1000);
			rec[i] = v; rec[i + 1] = v >> 8; rec[i + 2] = v >> 16; rec[i + 3] = v >> 24;
		}
		break;
	case 3: // nothing
		break;
	case 4: // every byte
		for (i = 0; i < len; i++) {
			rec[i] = rnd();
		}
		break;
	}
}

/**
 * Decodes into a heap buffer sized exactly to the input, so a read past the
 * end is caught by ASan, and an output with guard bytes either side.
 */
static s32 decodeGuarded(const u8 *in, s32 inlen, const u8 *base, u8 *out, s32 len, s32 *guardok)
{
	u8 *inheap = malloc(inlen > 0 ? inlen : 1);
	u8 *outheap = malloc(len + 64);
	s32 result;
	s32 i;

	memcpy(inheap, in, inlen > 0 ? inlen : 0);
	memset(outheap, 0xa5, len + 64);

	result = netDeltaDecode(inheap, inlen, base, outheap + 32, len);

	*guardok = 1;

	for (i = 0; i < 32; i++) {
		if (outheap[i] != 0xa5 || outheap[32 + len + i] != 0xa5) {
			*guardok = 0;
		}
	}

	memcpy(out, outheap + 32, len);
	free(inheap);
	free(outheap);

	return result;
}

static s32 testDeltaRoundTrip(void)
{
	TEST_BEGIN("netdelta random records, 20000 mutations");
	u8 base[1024];
	u8 cur[1024];
	u8 dec[1024];
	u8 enc[NETDELTA_MAXENCODED(1024)];
	s32 iter;
	s64 totalraw = 0;
	s64 totalenc = 0;

	for (iter = 0; iter < 20000; iter++) {
		s32 len = 1 + rndRange(iter % 10 == 0 ? 1024 : 96);
		s32 n;
		s32 used;
		s32 guardok;
		s32 i;

		for (i = 0; i < len; i++) {
			base[i] = rndRange(4) == 0 ? rnd() : 0; // records are mostly zeros
		}

		memcpy(cur, base, len);
		mutate(cur, len);

		n = netDeltaEncode(cur, base, len, enc, sizeof(enc));
		CHECK(n >= 1 && n <= NETDELTA_MAXENCODED(len));

		used = decodeGuarded(enc, n, base, dec, len, &guardok);
		CHECK(used == n);
		CHECK(guardok);
		CHECK(memcmp(dec, cur, len) == 0);

		// Too small an output is refused, never overrun
		if (n > 1) {
			CHECK(netDeltaEncode(cur, base, len, enc, n - 1) == -1);
		}

		// Keyframe of the same record
		n = netDeltaEncode(cur, NULL, len, enc, sizeof(enc));
		CHECK(n >= 1 && n <= NETDELTA_MAXENCODED(len));
		used = decodeGuarded(enc, n, NULL, dec, len, &guardok);
		CHECK(used == n && guardok && memcmp(dec, cur, len) == 0);

		// In place over the baseline
		{
			u8 inplace[1024];

			n = netDeltaEncode(cur, base, len, enc, sizeof(enc));
			memcpy(inplace, base, len);
			CHECK(netDeltaDecode(enc, n, inplace, inplace, len) == n);
			CHECK(memcmp(inplace, cur, len) == 0);
		}

		totalraw += len;
		totalenc += netDeltaEncode(cur, base, len, enc, sizeof(enc));
	}

	printf("  %lld record bytes delta-coded to %lld\n", (long long)totalraw, (long long)totalenc);

	TEST_END();
}

static s32 testDeltaEdges(void)
{
	TEST_BEGIN("netdelta keyframe, identical, random, long runs");
	u8 rec[NETDELTA_MAXRECORD];
	u8 base[NETDELTA_MAXRECORD];
	u8 dec[NETDELTA_MAXRECORD];
	u8 enc[NETDELTA_MAXENCODED(NETDELTA_MAXRECORD)];
	s32 n;
	s32 i;
	s32 guardok;

	// Identical: one byte
	for (i = 0; i < 64; i++) {
		rec[i] = rnd();
	}

	n = netDeltaEncode(rec, rec, 64, enc, sizeof(enc));
	CHECK(n == 1 && enc[0] == 0x00);
	CHECK(decodeGuarded(enc, n, rec, dec, 64, &guardok) == 1 && guardok && memcmp(dec, rec, 64) == 0);

	// All-zero keyframe: one byte too
	memset(rec, 0, 64);
	n = netDeltaEncode(rec, NULL, 64, enc, sizeof(enc));
	CHECK(n == 1);

	// Fully random, every size up to the limit at a stride: within the bound
	for (i = 1; i <= NETDELTA_MAXRECORD; i += (i < 300 ? 1 : 97)) {
		s32 k;

		for (k = 0; k < i; k++) {
			rec[k] = 1 + rndRange(255); // no zeros at all: the worst case
			base[k] = rnd();
		}

		n = netDeltaEncode(rec, NULL, i, enc, sizeof(enc));
		CHECK(n > 0 && n <= NETDELTA_MAXENCODED(i));
		CHECK(decodeGuarded(enc, n, NULL, dec, i, &guardok) == n && guardok && memcmp(dec, rec, i) == 0);

		n = netDeltaEncode(rec, base, i, enc, sizeof(enc));
		CHECK(n > 0 && n <= NETDELTA_MAXENCODED(i));
		CHECK(decodeGuarded(enc, n, base, dec, i, &guardok) == n && guardok && memcmp(dec, rec, i) == 0);
	}

	// Zero runs longer than one long control can say
	memset(rec, 0, sizeof(rec));
	rec[0] = 1;
	rec[700] = 2;
	rec[3000] = 3;
	rec[NETDELTA_MAXRECORD - 1] = 4;
	n = netDeltaEncode(rec, NULL, NETDELTA_MAXRECORD, enc, sizeof(enc));
	CHECK(n > 0 && n < 64);
	CHECK(decodeGuarded(enc, n, NULL, dec, NETDELTA_MAXRECORD, &guardok) == n && guardok
			&& memcmp(dec, rec, NETDELTA_MAXRECORD) == 0);

	// Lone zeros inside a run, and runs at each form's limits
	for (i = 0; i < 200; i++) {
		s32 len = 1 + rndRange(600);
		s32 k;

		for (k = 0; k < len; k++) {
			s32 pick = rndRange(10);
			rec[k] = pick < 3 ? 0 : rnd();
		}

		// Some exact-length zero runs: 7, 8, 255, 256
		if (len > 300) {
			static const s32 runs[] = { 7, 8, 15, 16, 127, 128, 255, 256 };
			s32 run = runs[rndRange(8)];
			s32 at = rndRange(len - run);

			memset(rec + at, 0, run);
		}

		n = netDeltaEncode(rec, NULL, len, enc, sizeof(enc));
		CHECK(n > 0 && n <= NETDELTA_MAXENCODED(len));
		CHECK(decodeGuarded(enc, n, NULL, dec, len, &guardok) == n && guardok && memcmp(dec, rec, len) == 0);
	}

	// Bad arguments
	CHECK(netDeltaEncode(rec, NULL, 0, enc, sizeof(enc)) == -1);
	CHECK(netDeltaEncode(rec, NULL, NETDELTA_MAXRECORD + 1, enc, sizeof(enc)) == -1);
	CHECK(netDeltaDecode(enc, 0, NULL, dec, 10) == -1);
	CHECK(netDeltaDecode(enc, 1, NULL, dec, 0) == -1);

	TEST_END();
}

static s32 testDeltaMalformed(void)
{
	TEST_BEGIN("netdelta truncation and garbage");
	u8 cur[256];
	u8 base[256];
	u8 dec[256];
	u8 enc[NETDELTA_MAXENCODED(256)];
	u8 junk[600];
	s32 iter;
	s32 guardok;
	s32 accepted = 0;

	// Every proper prefix of a valid encoding lacks its end byte and must fail
	for (iter = 0; iter < 500; iter++) {
		s32 len = 1 + rndRange(256);
		s32 n;
		s32 cut;
		s32 i;

		for (i = 0; i < len; i++) {
			base[i] = rnd();
			cur[i] = rndRange(3) ? base[i] : rnd();
		}

		n = netDeltaEncode(cur, base, len, enc, sizeof(enc));
		CHECK(n > 0);

		for (cut = 0; cut < n; cut++) {
			CHECK(decodeGuarded(enc, cut, base, dec, len, &guardok) == -1);
			CHECK(guardok);
		}

		// The same encoding decoded as a shorter record must fail if it would
		// fill past the end, and never overrun
		if (len > 1) {
			s32 r = decodeGuarded(enc, n, base, dec, len - 1, &guardok);

			CHECK(guardok);
			CHECK(r == -1 || r == n);
		}
	}

	// Random garbage: must fail or decode within bounds
	for (iter = 0; iter < 200000; iter++) {
		s32 inlen = 1 + rndRange(iter % 100 == 0 ? sizeof(junk) : 24);
		s32 len = 1 + rndRange(iter % 7 == 0 ? 256 : 32);
		s32 r;
		s32 i;

		for (i = 0; i < inlen; i++) {
			// Biased toward control-looking bytes and zeros
			s32 pick = rndRange(4);
			junk[i] = pick == 0 ? 0 : pick == 1 ? (rnd() & 0x8f) : rnd();
		}

		r = decodeGuarded(junk, inlen, (iter & 1) ? base : NULL, dec, len, &guardok);
		CHECK(guardok);
		CHECK(r == -1 || (r >= 1 && r <= inlen));

		if (r > 0) {
			accepted++;
		}
	}

	// Specific bad forms
	{
		u8 longzero[] = { 0x80, 0x00, 0x00 };   // long control saying nothing
		u8 overrun[] = { 0x8f, 0x00 };          // 15 literals, none given
		u8 pastend[] = { 0x80, 0xff, 0x00 };    // 255 zeros into a 10 byte record
		u8 shortpast[] = { 0x7f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0 }; // 7+15 > 20
		u8 ok[] = { 0x7f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0 };
		u8 lone[] = { 0x81 };                   // long control with no count byte

		CHECK(netDeltaDecode(longzero, sizeof(longzero), NULL, dec, 10) == -1);
		CHECK(netDeltaDecode(overrun, sizeof(overrun), NULL, dec, 100) == -1);
		CHECK(netDeltaDecode(pastend, sizeof(pastend), NULL, dec, 10) == -1);
		CHECK(netDeltaDecode(shortpast, sizeof(shortpast), NULL, dec, 20) == -1);
		CHECK(netDeltaDecode(ok, sizeof(ok), NULL, dec, 22) == (s32)sizeof(ok));
		CHECK(dec[6] == 0 && dec[7] == 1 && dec[21] == 15);
		CHECK(netDeltaDecode(lone, sizeof(lone), NULL, dec, 10) == -1);
	}

	printf("  %d of 200000 garbage inputs happened to be well formed\n", accepted);

	TEST_END();
}

static s32 testDeltaNetBuf(void)
{
	TEST_BEGIN("netdelta through netbuf");
	u8 recs[20][40];
	u8 bases[20][40];
	u8 out[40];
	u8 packet[1200];
	struct netbuf w;
	struct netbuf r;
	s32 i;

	netBufInitWrite(&w, packet, sizeof(packet));

	for (i = 0; i < 20; i++) {
		s32 k;

		for (k = 0; k < 40; k++) {
			bases[i][k] = rnd();
		}

		memcpy(recs[i], bases[i], 40);
		mutate(recs[i], 40);

		netBufWriteU16(&w, i);
		netBufWriteU16(&w, (i & 1) ? 0 : 7); // the caller's header: 0 = keyframe
		netDeltaWrite(&w, recs[i], (i & 1) ? NULL : bases[i], 40);
	}

	CHECK(netBufOk(&w));

	netBufInitRead(&r, packet, netBufLen(&w));

	for (i = 0; i < 20; i++) {
		u16 id = netBufReadU16(&r);
		u16 baseseq = netBufReadU16(&r);

		netDeltaRead(&r, baseseq ? bases[id < 20 ? id : 0] : NULL, out, 40);
		CHECK(netBufOk(&r) && id == i && memcmp(out, recs[i], 40) == 0);
	}

	CHECK(netBufRemaining(&r) == 0);

	// A packet that does not have room fails cleanly
	netBufInitWrite(&w, packet, 10);

	for (i = 0; i < 5; i++) {
		netDeltaWrite(&w, recs[0], NULL, 40);
	}

	CHECK(!netBufOk(&w) && netBufLen(&w) <= 10);

	TEST_END();
}

/**
 * Baseline ring
 */

static s32 testBaselineRing(void)
{
	TEST_BEGIN("baseline ring: acks, wraparound, expiry");
	struct netbaseline bl;
	u8 rec[16];
	u16 seq;
	u16 got;
	const u8 *p;
	s32 i;

	CHECK(netBaselineInit(&bl, 100, 50, 16) == 0);

	// Begin before anything: refused for 0, fine otherwise
	CHECK(netBaselineBegin(&bl, 0) == -1);
	CHECK(netBaselineAdd(&bl, 1, rec) == -1); // nothing begun

	// Start just short of the wrap so it is crossed
	seq = 65530;

	for (i = 0; i < 10; i++) {
		CHECK(netBaselineBegin(&bl, seq) == 0);
		memset(rec, i + 1, sizeof(rec));
		CHECK(netBaselineAdd(&bl, 3, rec) == 0);
		CHECK(netBaselineAdd(&bl, 3, rec) == -1); // not rising
		CHECK(netBaselineAdd(&bl, 2, rec) == -1);
		CHECK(netBaselineAdd(&bl, 100, rec) == -1); // out of range
		if (i % 2 == 0) {
			CHECK(netBaselineAdd(&bl, 7, rec) == 0);
		}
		seq = netSeqNext(seq);
	}

	// 65530..65535 then 1..4 (0 is skipped)
	CHECK(bl.newest == 4);
	CHECK(netSeqNewer(1, 65535));
	CHECK(!netSeqNewer(65535, 1));
	CHECK(netBaselineBegin(&bl, 65535) == -1); // older than newest
	CHECK(netBaselineBegin(&bl, 4) == -1);     // same

	// Nothing acked yet
	CHECK(netBaselineGetAcked(&bl, 3, &got) == NULL && got == 0);

	// Ack 65533 (i = 3): entity 3 there, entity 7 not
	CHECK(netBaselineAck(&bl, 65533) == 1);
	p = netBaselineGetAcked(&bl, 3, &got);
	CHECK(p && got == 65533 && p[0] == 4);
	CHECK(netBaselineGetAcked(&bl, 7, &got) == NULL);

	// Ack across the wrap: 2 (i = 7) is newer than 65533
	CHECK(netBaselineAck(&bl, 2) == 1);
	p = netBaselineGetAcked(&bl, 3, &got);
	CHECK(p && got == 2 && p[0] == 8);

	// A late ack for an older one does not take over
	CHECK(netBaselineAck(&bl, 65534) == 1);
	p = netBaselineGetAcked(&bl, 3, &got);
	CHECK(got == 2);
	p = netBaselineGetAcked(&bl, 7, &got);
	CHECK(p && got == 65534 && p[0] == 5);

	// Acks for what was never stored, or for 0
	CHECK(netBaselineAck(&bl, 0) == 0);
	CHECK(netBaselineAck(&bl, 100) == 0);
	CHECK(netBaselineAck(&bl, 65000) == 0);

	// Get by sequence, acked or not
	p = netBaselineGet(&bl, 4, 3);
	CHECK(p && p[0] == 10);
	CHECK(netBaselineGet(&bl, 4, 7) == NULL);
	CHECK(netBaselineGet(&bl, 3, 7) != NULL);

	// Forget: gone from the acks and from every snapshot, keyframe next
	netBaselineForget(&bl, 7);
	CHECK(netBaselineGetAcked(&bl, 7, &got) == NULL);
	CHECK(netBaselineGet(&bl, 3, 7) == NULL);
	CHECK(netBaselineAck(&bl, 3) == 1);
	CHECK(netBaselineGetAcked(&bl, 7, &got) == NULL);
	p = netBaselineGetAcked(&bl, 3, &got);
	CHECK(p && got == 3 && p[0] == 9);

	// The ring holds the newest 64: 62 more snapshots without entity 3 (5..66)
	// and seq 3 is still the oldest held...
	for (i = 0; i < 62; i++) {
		CHECK(netBaselineBegin(&bl, seq) == 0);
		seq = netSeqNext(seq);
	}

	p = netBaselineGetAcked(&bl, 3, &got);
	CHECK(p && got == 3);

	// ...one more (67, same slot) and it is recycled, and the ack with it
	CHECK(netBaselineBegin(&bl, seq) == 0);
	seq = netSeqNext(seq);
	CHECK(netBaselineGetAcked(&bl, 3, &got) == NULL && got == 0);
	CHECK(netBaselineAck(&bl, 3) == 0);

	// A jump of more than 64: everything held expires, none of it acked
	CHECK(netBaselineBegin(&bl, seq) == 0);
	CHECK(netBaselineAdd(&bl, 5, rec) == 0);
	CHECK(netBaselineAck(&bl, seq) == 1);
	CHECK(netBaselineGetAcked(&bl, 5, &got) != NULL);
	seq += 1000;
	CHECK(netBaselineBegin(&bl, seq) == 0);
	CHECK(netBaselineGetAcked(&bl, 5, &got) == NULL);

	// Slot capacity
	seq = netSeqNext(seq);
	CHECK(netBaselineBegin(&bl, seq) == 0);
	for (i = 0; i < 50; i++) {
		CHECK(netBaselineAdd(&bl, i, rec) == 0);
	}
	CHECK(netBaselineAdd(&bl, 60, rec) == -1);

	// Reset: everything a keyframe, and any sequence may start again
	CHECK(netBaselineAck(&bl, seq) == 1);
	CHECK(netBaselineGetAcked(&bl, 10, &got) != NULL);
	netBaselineReset(&bl);
	CHECK(netBaselineGetAcked(&bl, 10, &got) == NULL);
	CHECK(netBaselineGet(&bl, seq, 10) == NULL);
	CHECK(netBaselineBegin(&bl, 1) == 0);

	netBaselineFree(&bl);

	CHECK(netBaselineInit(&bl, 0, 1, 1) == -1);
	CHECK(netBaselineInit(&bl, 10, 11, 1) == -1);
	CHECK(netBaselineInit(&bl, 10, 10, NETDELTA_MAXRECORD + 1) == -1);

	TEST_END();
}

/**
 * The whole scheme in one process without sockets: a host keeps a ring per
 * client and encodes each entity against the newest acked record; the
 * client decodes against the record it stored for the sequence named, and
 * must end up with exactly the host's state. Snapshots and acks are lost at
 * random, entities come and go, and the sequence wraps several times.
 */
#define SIM_ENTITIES 40
#define SIM_RECSIZE  24

static s32 testBaselineScheme(void)
{
	TEST_BEGIN("baseline scheme: lossy snapshots across 3 sequence wraps");
	struct netbaseline host;
	struct netbaseline client;
	u8 state[SIM_ENTITIES][SIM_RECSIZE];
	u8 alive[SIM_ENTITIES];
	u8 clientstate[SIM_ENTITIES][SIM_RECSIZE];
	u8 clientalive[SIM_ENTITIES];
	u8 packet[4096];
	u16 seq = 1;
	u16 lastclientseq = 0;
	s32 frame;
	s32 delivered = 0;
	s32 keyframes = 0;
	s32 deltas = 0;
	s64 bytes = 0;

	CHECK(netBaselineInit(&host, SIM_ENTITIES, SIM_ENTITIES, SIM_RECSIZE) == 0);
	CHECK(netBaselineInit(&client, SIM_ENTITIES, SIM_ENTITIES, SIM_RECSIZE) == 0);
	memset(state, 0, sizeof(state));
	memset(alive, 0, sizeof(alive));
	memset(clientalive, 0, sizeof(clientalive));

	for (frame = 0; frame < 200000; frame++) {
		struct netbuf w;
		struct netbuf r;
		s32 e;

		// The world moves on
		for (e = 0; e < SIM_ENTITIES; e++) {
			if (rndRange(500) == 0) {
				// Freed, or freed and reused: the host forgets its baselines
				alive[e] = !alive[e] || rndRange(2);
				memset(state[e], rnd(), SIM_RECSIZE);
				netBaselineForget(&host, e);
			} else if (alive[e] && rndRange(3) == 0) {
				mutate(state[e], SIM_RECSIZE);
			}
		}

		// The host writes snapshot seq: per entity, id, baseline seq, delta
		netBufInitWrite(&w, packet, sizeof(packet));
		netBufWriteU16(&w, seq);
		CHECK(netBaselineBegin(&host, seq) == 0);

		for (e = 0; e < SIM_ENTITIES; e++) {
			const u8 *base;
			u16 baseseq;

			if (!alive[e]) {
				continue;
			}

			base = netBaselineGetAcked(&host, e, &baseseq);

			netBufWriteU16(&w, e);
			netBufWriteU16(&w, baseseq);
			netDeltaWrite(&w, state[e], base, SIM_RECSIZE);
			netBaselineAdd(&host, e, state[e]);

			if (base) {
				deltas++;
			} else {
				keyframes++;
			}
		}

		netBufWriteU16(&w, 0xffff);
		CHECK(netBufOk(&w));
		bytes += netBufLen(&w);

		// 25% of snapshots lost; the client reads the rest
		if (rndRange(4) != 0) {
			u16 rseq;
			u8 newalive[SIM_ENTITIES];
			u8 newstate[SIM_ENTITIES][SIM_RECSIZE];
			s32 ok = 1;

			netBufInitRead(&r, packet, netBufLen(&w));
			rseq = netBufReadU16(&r);
			memset(newalive, 0, sizeof(newalive));

			while (netBufOk(&r)) {
				u16 id = netBufReadU16(&r);
				u16 baseseq;
				const u8 *base = NULL;

				if (id == 0xffff || !netBufOk(&r)) {
					break;
				}

				baseseq = netBufReadU16(&r);

				if (id >= SIM_ENTITIES) {
					ok = 0;
					break;
				}

				if (baseseq != 0) {
					base = netBaselineGet(&client, baseseq, id);

					if (!base) {
						ok = 0; // the host named a baseline we never kept
						break;
					}
				}

				netDeltaRead(&r, base, newstate[id], SIM_RECSIZE);
				newalive[id] = 1;
			}

			CHECK(ok && netBufOk(&r));

			if (ok && netBufOk(&r) && (lastclientseq == 0 || netSeqNewer(rseq, lastclientseq))) {
				CHECK(netBaselineBegin(&client, rseq) == 0);

				for (e = 0; e < SIM_ENTITIES; e++) {
					if (newalive[e]) {
						netBaselineAdd(&client, e, newstate[e]);
						memcpy(clientstate[e], newstate[e], SIM_RECSIZE);
					}
				}

				memcpy(clientalive, newalive, sizeof(clientalive));
				lastclientseq = rseq;
				delivered++;

				// The client's view must now be exactly the host's
				for (e = 0; e < SIM_ENTITIES; e++) {
					CHECK(clientalive[e] == alive[e]);

					if (alive[e]) {
						CHECK(memcmp(clientstate[e], state[e], SIM_RECSIZE) == 0);
					}
				}

				// 25% of acks lost too
				if (rndRange(4) != 0) {
					netBaselineAck(&host, rseq);
				}
			}
		}

		// Every so often the link stalls: more than 64 snapshots without an ack
		if (frame % 20000 == 19999) {
			s32 k;

			for (k = 0; k < 70; k++) {
				seq = netSeqNext(seq);
				netBaselineBegin(&host, seq);
			}
		}

		seq = netSeqNext(seq);
	}

	printf("  %d snapshots delivered, %d deltas, %d keyframes, %.1f bytes/snapshot for %d entities of %d bytes\n",
			delivered, deltas, keyframes, (double)bytes / frame, SIM_ENTITIES, SIM_RECSIZE);

	netBaselineFree(&host);
	netBaselineFree(&client);

	TEST_END();
}

/**
 * Loopback transport
 */

#define LB_CLIENTS   2
#define LB_RELIABLE  300
#define LB_UNRELIABLE 300
#define LB_RAW       20
#define LB_BIGLEN    (60 * 1024)

struct lbstate {
	// Host side, per client index (taken from the first byte of each message)
	s32 hostpeer[LB_CLIENTS];
	s32 nextreliable[LB_CLIENTS];
	s32 unreliablegot[LB_CLIENTS];   // even indices: sequenced
	s32 unsequencedgot[LB_CLIENTS];  // odd indices: unsequenced
	s32 lastsequenced[LB_CLIENTS];
	u8 unreliableseen[LB_CLIENTS][LB_UNRELIABLE];
	s32 bulkgot[LB_CLIENTS];
	s32 rawgot;
	s32 hostdisconnects;
	s32 badmessages;

	// Client side
	s32 clientpeer[LB_CLIENTS];
	s32 connected[LB_CLIENTS];
	s32 clientnext[LB_CLIENTS];
	s32 clientbig[LB_CLIENTS];
	s32 clientraw[LB_CLIENTS];
	s32 clientreceives[LB_CLIENTS];
};

static void lbFillBig(u8 *buf, s32 len, s32 who)
{
	s32 i;

	for (i = 0; i < len; i++) {
		buf[i] = (u8)(i * 31 + who * 7);
	}
}

// The host's first service waits up to waitms, so the loop does not spin
static void lbPumpHost(struct nethost *host, struct lbstate *st, u8 *big, u32 waitms)
{
	struct netevent ev;

	while (netHostService(host, &ev, waitms) > 0) {
		waitms = 0;

		switch (ev.type) {
		case NETEVENT_CONNECT:
			break;
		case NETEVENT_DISCONNECT:
			if (getenv("NETTEST_VERBOSE")) {
				printf("  host: peer %d disconnected (%s) at %u ms\n", ev.peer, ev.timedout ? "timed out" : "closed", netTransportTime());
			}
			st->hostdisconnects++;
			break;
		case NETEVENT_RECEIVE: {
			struct netbuf r;
			s32 who;
			s32 index;

			netBufInitRead(&r, ev.data, ev.len);
			who = netBufReadU8(&r);
			index = netBufReadU32(&r);

			if (!netBufOk(&r) || who >= LB_CLIENTS) {
				st->badmessages++;
				break;
			}

			st->hostpeer[who] = ev.peer;

			if (ev.channel == NET_CHAN_RELIABLE) {
				if (index != st->nextreliable[who]) {
					st->badmessages++;
				}
				st->nextreliable[who] = index + 1;
			} else if (ev.channel == NET_CHAN_UNRELIABLE) {
				// Never a duplicate, and the sequenced ones never go backwards
				if (index >= LB_UNRELIABLE || st->unreliableseen[who][index]) {
					st->badmessages++;
					break;
				}

				st->unreliableseen[who][index] = 1;

				if (index & 1) {
					st->unsequencedgot[who]++;
				} else {
					if (index <= st->lastsequenced[who]) {
						st->badmessages++;
					}
					st->lastsequenced[who] = index;
					st->unreliablegot[who]++;
				}
			} else if (ev.channel == NET_CHAN_BULK) {
				lbFillBig(big, LB_BIGLEN, who);
				if (ev.len != 5 + LB_BIGLEN || memcmp(ev.data + 5, big, LB_BIGLEN) != 0) {
					st->badmessages++;
				}
				st->bulkgot[who]++;
			}
			break;
		}
		case NETEVENT_RAW: {
			char addr[64];
			u8 reply[8];

			// Echo it from the same socket, so the client sees our port
			if (ev.len == 6 && memcmp(ev.data, "punch", 5) == 0) {
				st->rawgot++;
				memcpy(reply, "reply", 5);
				reply[5] = ev.data[5];
				netHostSendRaw(host, &ev.from, reply, 6);
			} else {
				netAddrToString(&ev.from, addr, sizeof(addr));
				printf("  unexpected raw datagram from %s\n", addr);
				st->badmessages++;
			}
			break;
		}
		}
	}
}

static void lbPumpClient(struct nethost *client, s32 who, struct lbstate *st)
{
	struct netevent ev;

	while (netHostService(client, &ev, 0) > 0) {
		switch (ev.type) {
		case NETEVENT_CONNECT:
			st->connected[who] = 1;
			break;
		case NETEVENT_DISCONNECT:
			printf("  client %d disconnected (%s) at %u ms\n", who, ev.timedout ? "timed out" : "closed", netTransportTime());
			st->connected[who] = -1;
			break;
		case NETEVENT_RECEIVE: {
			struct netbuf r;
			u32 index;

			st->clientreceives[who]++;
			netBufInitRead(&r, ev.data, ev.len);
			index = netBufReadU32(&r);

			if (ev.channel != NET_CHAN_RELIABLE || !netBufOk(&r) || index != (u32)st->clientnext[who]) {
				st->badmessages++;
			}

			st->clientnext[who] = index + 1;
			break;
		}
		case NETEVENT_RAW:
			if (ev.len == 6 && memcmp(ev.data, "reply", 5) == 0 && ev.data[5] == who) {
				st->clientraw[who]++;
			} else {
				st->badmessages++;
			}
			break;
		}
	}
}

static s32 testLoopback(void)
{
	char name[128];
	snprintf(name, sizeof(name), "loopback: 1 host, 2 clients, %d%% loss + %d-%d ms delay", g_SimLoss, g_SimDelay, g_SimDelay + g_SimJitter);
	TEST_BEGIN(name);
	struct nethost *host;
	struct nethost *clients[LB_CLIENTS];
	struct lbstate st;
	struct netaddr hostaddr;
	u8 *big = malloc(LB_BIGLEN + 5);
	u8 *expect = malloc(LB_BIGLEN);
	u32 start;
	s32 sentreliable[LB_CLIENTS] = { 0 };
	s32 sentunreliable[LB_CLIENTS] = { 0 };
	s32 sentraw[LB_CLIENTS] = { 0 };
	s32 hostsent[LB_CLIENTS] = { 0 };
	s32 bigsent[LB_CLIENTS] = { 0 };
	s32 done = 0;
	s32 i;
	u16 port;

	memset(&st, 0, sizeof(st));

	for (i = 0; i < LB_CLIENTS; i++) {
		st.hostpeer[i] = -1;
		st.lastsequenced[i] = -1;
	}

	CHECK(netTransportInit() == 0);

	host = netHostCreate("127.0.0.1", 0, 4);
	CHECK(host != NULL);

	if (!host) {
		TEST_END();
	}

	port = netHostPort(host);
	CHECK(port != 0);
	CHECK(netAddrResolve("127.0.0.1", port, &hostaddr) == 0);
	netHostSetSim(host, g_SimLoss, g_SimDelay, g_SimJitter, 1);

	for (i = 0; i < LB_CLIENTS; i++) {
		clients[i] = netHostCreate(NULL, 0, 1);
		CHECK(clients[i] != NULL);
		netHostSetSim(clients[i], g_SimLoss, g_SimDelay, g_SimJitter, 100 + i);
		st.clientpeer[i] = netHostConnect(clients[i], "127.0.0.1", port, 0x1234 + i);
		CHECK(st.clientpeer[i] >= 0);
	}

	// Refused sends: not connected yet, bad channel, oversized unreliable
	CHECK(netHostSend(clients[0], st.clientpeer[0], NET_CHAN_RELIABLE, "x", 1, NET_SEND_RELIABLE) == -1);
	CHECK(netHostSend(clients[0], 99, NET_CHAN_RELIABLE, "x", 1, NET_SEND_RELIABLE) == -1);

	start = netTransportTime();

	while (!done && netTransportTime() - start < 30000) {
		lbPumpHost(host, &st, expect, 1);

		for (i = 0; i < LB_CLIENTS; i++) {
			lbPumpClient(clients[i], i, &st);
		}

		for (i = 0; i < LB_CLIENTS; i++) {
			u8 msg[64];
			struct netbuf w;

			if (st.connected[i] != 1) {
				continue;
			}

			if (sentreliable[i] == 0) {
				u8 huge[NET_MAXUNRELIABLE + 1];

				memset(huge, 0, sizeof(huge));
				CHECK(netHostSend(clients[i], st.clientpeer[i], NET_CHAN_UNRELIABLE, huge, sizeof(huge), 0) == -1);
				CHECK(netHostSend(clients[i], st.clientpeer[i], NET_NUMCHANNELS, "x", 1, NET_SEND_RELIABLE) == -1);
			}

			// A few of each per pass
			if (sentreliable[i] < LB_RELIABLE) {
				s32 k;

				for (k = 0; k < 5 && sentreliable[i] < LB_RELIABLE; k++) {
					netBufInitWrite(&w, msg, sizeof(msg));
					netBufWriteU8(&w, i);
					netBufWriteU32(&w, sentreliable[i]);
					CHECK(netHostSend(clients[i], st.clientpeer[i], NET_CHAN_RELIABLE, msg, netBufLen(&w), NET_SEND_RELIABLE) == 0);
					sentreliable[i]++;
				}
			}

			// Unreliable only once the host has been heard from, as the
			// session layer will: until the host has the ack of its
			// verify-connect its end is not connected and drops unreliable
			// data, and with that ack lost it does not know for a resend
			if (sentunreliable[i] < LB_UNRELIABLE && st.clientnext[i] > 0) {
				netBufInitWrite(&w, msg, sizeof(msg));
				netBufWriteU8(&w, i);
				netBufWriteU32(&w, sentunreliable[i]);
				CHECK(netHostSend(clients[i], st.clientpeer[i], NET_CHAN_UNRELIABLE, msg, netBufLen(&w),
							(sentunreliable[i] & 1) ? NET_SEND_UNSEQUENCED : 0) == 0);
				sentunreliable[i]++;
			}

			if (!bigsent[i]) {
				big[0] = i;
				big[1] = big[2] = big[3] = big[4] = 0;
				lbFillBig(big + 5, LB_BIGLEN, i);
				CHECK(netHostSend(clients[i], st.clientpeer[i], NET_CHAN_BULK, big, LB_BIGLEN + 5, NET_SEND_RELIABLE) == 0);
				bigsent[i] = 1;
			}

			if (sentraw[i] < LB_RAW) {
				u8 punch[6] = { 'p', 'u', 'n', 'c', 'h', (u8)i };

				CHECK(netHostSendRaw(clients[i], &hostaddr, punch, sizeof(punch)) == 0);
				sentraw[i]++;
			}

			// The host answers each client once it knows its peer id
			if (st.hostpeer[i] >= 0 && hostsent[i] < LB_RELIABLE) {
				netBufInitWrite(&w, msg, sizeof(msg));
				netBufWriteU32(&w, hostsent[i]);
				CHECK(netHostSend(host, st.hostpeer[i], NET_CHAN_RELIABLE, msg, netBufLen(&w), NET_SEND_RELIABLE) == 0);
				hostsent[i]++;
			}
		}

		done = 1;

		for (i = 0; i < LB_CLIENTS; i++) {
			if (st.nextreliable[i] < LB_RELIABLE || st.clientnext[i] < LB_RELIABLE
					|| st.bulkgot[i] < 1 || st.clientraw[i] < LB_RAW || sentunreliable[i] < LB_UNRELIABLE) {
				done = 0;
			}
		}
	}

	printf("  %u ms\n", netTransportTime() - start);

	for (i = 0; i < LB_CLIENTS; i++) {
		struct netpeerstats stats;

		CHECK(st.connected[i] == 1);
		CHECK(st.nextreliable[i] == LB_RELIABLE);
		CHECK(st.clientnext[i] == LB_RELIABLE);
		CHECK(st.bulkgot[i] == 1);
		CHECK(st.clientraw[i] == LB_RAW);

		// Unsequenced lose only what the simulator drops; sequenced also lose
		// whatever arrives behind a newer one, which jitter of many times the
		// send interval makes most of them, so only "some" is asked of those
		CHECK(st.unsequencedgot[i] >= (LB_UNRELIABLE / 2) * (100 - g_SimLoss) / 100 * 3 / 4);
		CHECK(st.unreliablegot[i] > 0);

		CHECK(netHostPeerStats(clients[i], st.clientpeer[i], &stats) == 0);
		CHECK(stats.connected && stats.mtu <= NET_MTU);
		printf("  client %d: unreliable %d/%d unsequenced and %d/%d sequenced arrived, rtt %u ms, resent %u of %u\n",
				i, st.unsequencedgot[i], LB_UNRELIABLE / 2, st.unreliablegot[i], LB_UNRELIABLE / 2,
				stats.rtt, stats.resent, stats.packetssent);
	}

	CHECK(st.rawgot == LB_RAW * LB_CLIENTS);
	CHECK(st.badmessages == 0);

	// Peer addresses
	{
		struct netaddr a;
		char str[64];

		CHECK(netHostPeerAddr(host, st.hostpeer[0], &a) == 0);
		netAddrToString(&a, str, sizeof(str));
		CHECK(strncmp(str, "127.0.0.1:", 10) == 0);
		CHECK(a.port == netHostPort(clients[0]));
	}

	// A polite disconnect reaches the host
	netHostSetSim(host, 0, 0, 0, 0);
	netHostSetSim(clients[0], 0, 0, 0, 0);
	netHostDisconnect(clients[0], st.clientpeer[0], 7);
	start = netTransportTime();

	while (st.hostdisconnects == 0 && netTransportTime() - start < 5000) {
		struct netevent ev;

		lbPumpHost(host, &st, expect, 1);
		netHostService(clients[0], &ev, 0);
		if (ev.type == NETEVENT_DISCONNECT) {
			st.connected[0] = -1;
		}
	}

	CHECK(st.hostdisconnects == 1);

	for (i = 0; i < LB_CLIENTS; i++) {
		netHostDestroy(clients[i]);
	}

	netHostDestroy(host);
	netTransportShutdown();
	free(big);
	free(expect);

	TEST_END();
}

/**
 * A peer that vanishes (cable pulled, game crashed) while the host is still
 * sending it reliable messages must time out in seconds: ENET_PD_RESEND_LIMIT
 * unanswered sends past the 5 s timeout minimum, not the 30 s maximum the
 * capped resend back-off once left as the only way out.
 */
static s32 testDeadPeer(void)
{
	TEST_BEGIN("vanished peer times out");
	struct nethost *host = NULL;
	struct nethost *client = NULL;
	struct netevent ev;
	s32 hostpeer = -1;
	s32 timedout = -1;
	u32 dead = 0;
	u32 start;
	u8 msg[16] = { 1 };
	s32 i;

	CHECK(netTransportInit() == 0);
	host = netHostCreate("127.0.0.1", 0, 4);
	client = netHostCreate(NULL, 0, 1);
	CHECK(host != NULL && client != NULL);

	if (host && client) {
		CHECK(netHostConnect(client, "127.0.0.1", netHostPort(host), 0) >= 0);

		for (i = 0; i < 400 && hostpeer < 0; i++) {
			while (netHostService(host, &ev, 5) > 0) {
				if (ev.type == NETEVENT_CONNECT) {
					hostpeer = ev.peer;
				}
			}

			while (netHostService(client, &ev, 5) > 0);
		}

		// Settle the RTT estimate first
		for (i = 0; i < 50; i++) {
			while (netHostService(host, &ev, 5) > 0);
			while (netHostService(client, &ev, 5) > 0);
		}

		CHECK(hostpeer >= 0);

		// The client vanishes: the host hears nothing more from it
		netHostSetSim(host, 100, 0, 0, 1);
		start = netTransportTime();

		while (hostpeer >= 0 && timedout < 0 && netTransportTime() - start < 40000) {
			netHostSend(host, hostpeer, NET_CHAN_RELIABLE, msg, sizeof(msg), NET_SEND_RELIABLE);

			while (netHostService(host, &ev, 50) > 0) {
				if (ev.type == NETEVENT_DISCONNECT && ev.peer == hostpeer) {
					timedout = ev.timedout;
					dead = netTransportTime() - start;
				}
			}
		}

		printf("  vanished peer dropped after %u ms\n", dead);
		CHECK(timedout == 1);
		CHECK(dead >= 4000 && dead <= 15000);
	}

	netHostDestroy(client);
	netHostDestroy(host);
	netTransportShutdown();

	TEST_END();
}

// nettesthostile.c
s32 hostileFragmentFlood(void);
s32 hostileBigDatagrams(void);

// nettestsnap.c
s32 snapTestQuant(void);
s32 snapTestLocalPlayer(void);
s32 snapTestStreamClean(void);
s32 snapTestStreamLossy(void);
s32 snapTestHostile(void);

static void testPlain(const char *name, s32 (*fn)(void))
{
	const s32 fails = fn();

	g_Checks++;
	printf("%s %s\n", fails ? "FAIL" : "PASS", name);

	if (fails) {
		g_Failures++;
	}
}

static void testHostile(const char *name, s32 (*fn)(void))
{
	s32 fails;

	if (netTransportInit() != 0) {
		printf("FAIL %s (netTransportInit)\n", name);
		g_Failures++;
		return;
	}

	fails = fn();
	g_Checks++;
	printf("%s %s\n", fails ? "FAIL" : "PASS", name);

	if (fails) {
		g_Failures++;
	}

	netTransportShutdown();
}

int main(int argc, char **argv)
{
	s32 skipnet = 0;
	s32 netonly = 0;
	s32 i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-net") == 0) {
			skipnet = 1;
		} else if (strcmp(argv[i], "--net-only") == 0) {
			netonly = 1;
		} else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) {
			g_SimLoss = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
			g_SimDelay = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--jitter") == 0 && i + 1 < argc) {
			g_SimJitter = atoi(argv[++i]);
		} else {
			printf("usage: %s [--no-net] [--net-only] [--loss PCT] [--delay MS] [--jitter MS]\n", argv[0]);
			return 2;
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	if (!netonly) {
		testNetBufRoundTrip();
		testNetBufOverflow();
		testDeltaRoundTrip();
		testDeltaEdges();
		testDeltaMalformed();
		testDeltaNetBuf();
		testBaselineRing();
		testBaselineScheme();
		testPlain("snapshot quantizers", snapTestQuant);
		testPlain("local-player block round trip", snapTestLocalPlayer);
		testPlain("snapshot stream, no loss", snapTestStreamClean);
		testPlain("snapshot stream, 10% loss each way", snapTestStreamLossy);
		testPlain("snapshot hostile input", snapTestHostile);
	}

	if (!skipnet) {
		testLoopback();
		testHostile("hostile fragment flood", hostileFragmentFlood);
		testHostile("hostile oversized datagrams", hostileBigDatagrams);
		testDeadPeer();
	}

	printf("%s: %d checks, %d test(s) failed\n", g_Failures ? "FAIL" : "PASS", g_Checks, g_Failures);

	return g_Failures ? 1 : 0;
}
