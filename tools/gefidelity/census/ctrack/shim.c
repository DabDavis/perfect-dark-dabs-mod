/* The census build of port/src/geconvert.c: every byte it reads from the ROM,
 * tracked, with no change to geconvert.c and nothing of this in the game.
 *
 * build.sh compiles geconvert.c alone (no GECONVERT_MAIN: this file has the
 * main) with GCC's kernel-address instrumentation and the callback threshold
 * at 0, so every load the converter makes calls __asan_load*_noabort() below
 * - raw indexing, be32(), struct copies, all of it. Library calls that read
 * memory are wrapped at the link (-Wl,--wrap): memcpy/memmove/memcmp, zlib's
 * inflate and deflate, fwrite; and malloc/calloc/realloc/free, so a buffer's
 * provenance follows it when it moves.
 *
 * Provenance is kept as tags on address ranges: the ROM is one tag; each
 * inflate() call tags the bytes it wrote with its stream (the compressed
 * input's ROM offset) and their offset in it; a copy of 4096 bytes or more
 * carries its source's tags to the destination (a bg file copied out of the
 * ROM, a setup copied to build the later cartridges' revision) and is not a
 * read; a shorter copy is a read of the source, as a copy (a record copied
 * into a Perfect Dark one). A load of a tagged byte is a read, as a parse.
 * Bytes that reach zlib's deflate or fwrite from a tagged range are carried.
 *
 * Per stream: the inflated bytes, and a map (bit 0 parsed, bit 1 copied).
 * GECENSUS_NULL="ROMOFF:LO-HI,LO-HI..." gives the stream at ROMOFF a second
 * map in which reads of those offsets are not marked (census.py's null).
 *
 *   gecensus ROM OUTDIR DUMPDIR
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "geconvert.h"

#define ROMSTREAM 0
#define BULK 4096

typedef struct { uintptr_t lo, hi; int stream; size_t off; } tag;
typedef struct {
	long key;           /* the ROM offset of the compressed input, or -1 for the ROM itself */
	int srcstream;      /* the stream the input was tagged with (the ROM, mostly) */
	size_t len, cap;
	uint8_t *data, *map, *shadow, *mask;
} stream;

static tag *g_Tags;
static size_t g_NumTags, g_CapTags;
static stream *g_Streams;
static int g_NumStreams, g_CapStreams;
static int g_Tracking;
static uintptr_t g_TagLo = UINTPTR_MAX, g_TagHi;
static long g_NullKey = -2;
static char *g_NullRanges;

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__real_memcpy(void *, const void *, size_t);
void *__real_memmove(void *, const void *, size_t);
int __real_memcmp(const void *, const void *, size_t);
int __real_inflate(z_streamp, int);
int __real_deflate(z_streamp, int);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);

/* ------------------------------------------------------------------ sizes */
typedef struct { uintptr_t p; size_t n; } szent;
static szent *g_Sz;
static size_t g_SzCap, g_SzN;

static size_t szSlot(uintptr_t p)
{
	size_t i = (p >> 4) * 0x9e3779b97f4a7c15ull >> 20;
	for (;; i++) {
		szent *e = &g_Sz[i & (g_SzCap - 1)];
		if (e->p == p || e->p == 0) {
			return i & (g_SzCap - 1);
		}
	}
}

static void szGrow(void)
{
	szent *old = g_Sz;
	size_t oldcap = g_SzCap;
	g_SzCap = g_SzCap ? g_SzCap * 2 : 1 << 16;
	g_Sz = __real_calloc(g_SzCap, sizeof(*g_Sz));
	g_SzN = 0;
	for (size_t i = 0; i < oldcap; i++) {
		if (old[i].p && old[i].n != (size_t)-1) {
			size_t s = szSlot(old[i].p);
			g_Sz[s] = old[i];
			g_SzN++;
		}
	}
	__real_free(old);
}

static void szSet(void *p, size_t n)
{
	if (!p) return;
	if ((g_SzN + 1) * 2 > g_SzCap) szGrow();
	size_t s = szSlot((uintptr_t)p);
	if (g_Sz[s].p == 0) g_SzN++;
	g_Sz[s].p = (uintptr_t)p;
	g_Sz[s].n = n;
}

static size_t szGet(void *p)
{
	if (!p || !g_SzCap) return 0;
	size_t s = szSlot((uintptr_t)p);
	return g_Sz[s].p == (uintptr_t)p && g_Sz[s].n != (size_t)-1 ? g_Sz[s].n : 0;
}

static void szDel(void *p)
{
	if (!p || !g_SzCap) return;
	size_t s = szSlot((uintptr_t)p);
	if (g_Sz[s].p == (uintptr_t)p) g_Sz[s].n = (size_t)-1;   /* a tombstone keeps the probe chain */
}

/* ------------------------------------------------------------------ tags */
static int tagCmp(const void *a, const void *b)
{
	const tag *x = a, *y = b;
	return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void tagsSort(void)
{
	qsort(g_Tags, g_NumTags, sizeof(*g_Tags), tagCmp);
	g_TagLo = UINTPTR_MAX;
	g_TagHi = 0;
	for (size_t i = 0; i < g_NumTags; i++) {
		if (g_Tags[i].lo < g_TagLo) g_TagLo = g_Tags[i].lo;
		if (g_Tags[i].hi > g_TagHi) g_TagHi = g_Tags[i].hi;
	}
}

static void tagsDrop(uintptr_t lo, uintptr_t hi, int sort)
{
	/* cut [lo, hi) out of every tag, splitting one that straddles it */
	size_t n = g_NumTags;
	for (size_t i = 0; i < n; i++) {
		tag t = g_Tags[i];
		if (t.hi <= lo || t.lo >= hi) continue;
		if (t.lo < lo && t.hi > hi) {
			if (g_NumTags == g_CapTags) {
				g_CapTags = g_CapTags ? g_CapTags * 2 : 1024;
				g_Tags = __real_realloc(g_Tags, g_CapTags * sizeof(*g_Tags));
			}
			g_Tags[g_NumTags++] = (tag){hi, t.hi, t.stream, t.off + (hi - t.lo)};
			g_Tags[i].hi = lo;
		} else if (t.lo < lo) {
			g_Tags[i].hi = lo;
		} else if (t.hi > hi) {
			g_Tags[i].off += hi - t.lo;
			g_Tags[i].lo = hi;
		} else {
			g_Tags[i].hi = g_Tags[i].lo;     /* emptied; swept below */
		}
	}
	size_t k = 0;
	for (size_t i = 0; i < g_NumTags; i++) {
		if (g_Tags[i].hi > g_Tags[i].lo) g_Tags[k++] = g_Tags[i];
	}
	g_NumTags = k;
	if (sort) tagsSort();
}

static void tagAdd(uintptr_t lo, uintptr_t hi, int stream, size_t off)
{
	if (hi <= lo) return;
	tagsDrop(lo, hi, 0);
	if (g_NumTags == g_CapTags) {
		g_CapTags = g_CapTags ? g_CapTags * 2 : 1024;
		g_Tags = __real_realloc(g_Tags, g_CapTags * sizeof(*g_Tags));
	}
	g_Tags[g_NumTags++] = (tag){lo, hi, stream, off};
	tagsSort();
}

static const tag *g_LastTag;

static const tag *tagFind(uintptr_t a)
{
	if (a < g_TagLo || a >= g_TagHi) return NULL;
	if (g_LastTag && a >= g_LastTag->lo && a < g_LastTag->hi) return g_LastTag;
	size_t lo = 0, hi = g_NumTags;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (g_Tags[mid].lo <= a) lo = mid + 1; else hi = mid;
	}
	if (lo == 0) return NULL;
	const tag *t = &g_Tags[lo - 1];
	if (a < t->hi) {
		g_LastTag = t;
		return t;
	}
	return NULL;
}

/* ------------------------------------------------------------------ streams */
static void streamGrow(stream *s, size_t n)
{
	if (n <= s->cap) return;
	size_t cap = s->cap ? s->cap : 65536;
	while (cap < n) cap *= 2;
	s->data = __real_realloc(s->data, cap);
	s->map = __real_realloc(s->map, cap);
	memset(s->map + s->cap, 0, cap - s->cap);
	if (s->shadow) {
		s->shadow = __real_realloc(s->shadow, cap);
		memset(s->shadow + s->cap, 0, cap - s->cap);
		s->mask = __real_realloc(s->mask, cap);
		memset(s->mask + s->cap, 0, cap - s->cap);
	}
	s->cap = cap;
}

static void armNull(stream *s)
{
	/* "ROMOFF:LO-HI,LO-HI,...": the offsets the null hides from this stream's second map */
	s->shadow = __real_calloc(s->cap ? s->cap : 1, 1);
	s->mask = __real_calloc(s->cap ? s->cap : 1, 1);
	const char *p = strchr(g_NullRanges, ':');
	while (p && *++p) {
		char *e;
		unsigned long lo = strtoul(p, &e, 0), hi;
		if (*e != '-') break;
		hi = strtoul(e + 1, &e, 0);
		streamGrow(s, hi);
		for (unsigned long i = lo; i < hi; i++) s->mask[i] = 1;
		p = e;
		if (*p != ',') break;
	}
}

static int streamFor(long key, int src)
{
	for (int i = 0; i < g_NumStreams; i++) {
		if (g_Streams[i].key == key && g_Streams[i].srcstream == src) return i;
	}
	if (g_NumStreams == g_CapStreams) {
		g_CapStreams = g_CapStreams ? g_CapStreams * 2 : 256;
		g_Streams = __real_realloc(g_Streams, g_CapStreams * sizeof(*g_Streams));
	}
	stream *s = &g_Streams[g_NumStreams];
	memset(s, 0, sizeof(*s));
	s->key = key;
	s->srcstream = src;
	if (key == g_NullKey && src == ROMSTREAM) armNull(s);
	return g_NumStreams++;
}

static void markRange(uintptr_t a, size_t n, uint8_t bit)
{
	if (!g_Tracking || !n) return;
	uintptr_t end = a + n;
	while (a < end) {
		const tag *t = tagFind(a);
		if (!t) {
			/* jump to the next tag's start, or the end */
			size_t lo = 0, hi = g_NumTags;
			while (lo < hi) {
				size_t mid = (lo + hi) / 2;
				if (g_Tags[mid].lo <= a) lo = mid + 1; else hi = mid;
			}
			if (lo == g_NumTags || g_Tags[lo].lo >= end) break;
			a = g_Tags[lo].lo;
			continue;
		}
		uintptr_t stop = end < t->hi ? end : t->hi;
		stream *s = &g_Streams[t->stream];
		size_t o = t->off + (a - t->lo);
		size_t k = stop - a;
		if (o + k <= s->len) {
			for (size_t i = 0; i < k; i++) s->map[o + i] |= bit;
			if (s->shadow) {
				for (size_t i = 0; i < k; i++) {
					if (!s->mask[o + i]) s->shadow[o + i] |= bit;
				}
			}
		}
		a = stop;
	}
}

/* ------------------------------------------------------------------ instrumentation */
#define LOAD(n) void __asan_load##n##_noabort(uintptr_t a) { markRange(a, n, 1); }
LOAD(1) LOAD(2) LOAD(4) LOAD(8) LOAD(16)
void __asan_loadN_noabort(uintptr_t a, size_t n) { markRange(a, n, 1); }
#define STORE(n) void __asan_store##n##_noabort(uintptr_t a) { (void)a; }
STORE(1) STORE(2) STORE(4) STORE(8) STORE(16)
void __asan_storeN_noabort(uintptr_t a, size_t n) { (void)a; (void)n; }
void __asan_handle_no_return(void) {}

/* ------------------------------------------------------------------ wrapped calls */
void *__wrap_malloc(size_t n)
{
	void *p = __real_malloc(n);
	szSet(p, n);
	if (p && g_Tracking) tagsDrop((uintptr_t)p, (uintptr_t)p + n, 1);
	return p;
}

void *__wrap_calloc(size_t a, size_t b)
{
	void *p = __real_calloc(a, b);
	szSet(p, a * b);
	if (p && g_Tracking) tagsDrop((uintptr_t)p, (uintptr_t)p + a * b, 1);
	return p;
}

void *__wrap_realloc(void *p, size_t n)
{
	size_t old = szGet(p);
	void *q = __real_realloc(p, n);
	if (!q) return q;
	if (p && q != p) szDel(p);
	szSet(q, n);
	if (g_Tracking && p) {
		uintptr_t lo = (uintptr_t)p, hi = lo + old;
		if (q != p) {
			/* the block moved: its tags move with it, as far as it still reaches */
			intptr_t d = (intptr_t)((uintptr_t)q - lo);
			size_t nt = g_NumTags;
			for (size_t i = 0; i < nt; i++) {
				tag *t = &g_Tags[i];
				if (t->lo >= lo && t->hi <= hi) {
					t->lo += d;
					t->hi += d;
					if (t->hi > (uintptr_t)q + n) t->hi = (uintptr_t)q + n;
					if (t->lo >= t->hi) t->hi = t->lo;
				}
			}
			tagsDrop(0, 0, 0);
			tagsSort();
		} else if (n < old) {
			tagsDrop(lo + n, hi, 1);
		}
	} else if (g_Tracking && !p) {
		tagsDrop((uintptr_t)q, (uintptr_t)q + n, 1);
	}
	return q;
}

void __wrap_free(void *p)
{
	if (p && g_Tracking) {
		size_t n = szGet(p);
		if (n) tagsDrop((uintptr_t)p, (uintptr_t)p + n, 1);
	}
	szDel(p);
	__real_free(p);
}

static void copyTags(uintptr_t dst, uintptr_t src, size_t n)
{
	/* the destination takes the source's provenance, range for range */
	tagsDrop(dst, dst + n, 0);
	size_t nt = g_NumTags;
	for (size_t i = 0; i < nt; i++) {
		tag t = g_Tags[i];
		uintptr_t lo = t.lo > src ? t.lo : src, hi = t.hi < src + n ? t.hi : src + n;
		if (lo >= hi) continue;
		if (g_NumTags == g_CapTags) {
			g_CapTags = g_CapTags ? g_CapTags * 2 : 1024;
			g_Tags = __real_realloc(g_Tags, g_CapTags * sizeof(*g_Tags));
		}
		g_Tags[g_NumTags++] = (tag){dst + (lo - src), dst + (hi - src), t.stream, t.off + (lo - t.lo)};
	}
	tagsSort();
}

static int anyTagged(uintptr_t a, size_t n)
{
	if (a + n <= g_TagLo || a >= g_TagHi) return 0;
	for (size_t i = 0; i < g_NumTags; i++) {
		if (g_Tags[i].lo < a + n && g_Tags[i].hi > a) return 1;
	}
	return 0;
}

static void *copyLike(void *d, const void *s, size_t n, int move)
{
	if (g_Tracking && n) {
		if (n >= BULK) {
			if (anyTagged((uintptr_t)s, n)) copyTags((uintptr_t)d, (uintptr_t)s, n);
			else tagsDrop((uintptr_t)d, (uintptr_t)d + n, 1);
		} else {
			markRange((uintptr_t)s, n, 2);
		}
	}
	return move ? __real_memmove(d, s, n) : __real_memcpy(d, s, n);
}

void *__wrap_memcpy(void *d, const void *s, size_t n) { return copyLike(d, s, n, 0); }
void *__wrap_memmove(void *d, const void *s, size_t n) { return copyLike(d, s, n, 1); }

int __wrap_memcmp(const void *a, const void *b, size_t n)
{
	markRange((uintptr_t)a, n, 1);
	markRange((uintptr_t)b, n, 1);
	return __real_memcmp(a, b, n);
}

/* the inflate a z_stream belongs to, from its first call */
typedef struct { z_streamp zs; int stream; } zent;
static zent g_Z[64];

int __wrap_inflate(z_streamp zs, int flush)
{
	if (!g_Tracking) return __real_inflate(zs, flush);
	const uint8_t *in0 = zs->next_in;
	uint8_t *out0 = zs->next_out;
	uLong tout = zs->total_out;
	int slot = -1;
	for (int i = 0; i < 64; i++) {
		if (g_Z[i].zs == zs) { slot = i; break; }
	}
	if (slot < 0 || zs->total_in == 0) {
		const tag *t = tagFind((uintptr_t)in0);
		long key = t ? (long)(t->off + ((uintptr_t)in0 - t->lo)) : -3;
		int src = t ? t->stream : -1;
		if (slot < 0) {
			for (slot = 0; slot < 63 && g_Z[slot].zs; slot++) {}
		}
		g_Z[slot].zs = zs;
		g_Z[slot].stream = streamFor(key, src);
	}
	int sid = g_Z[slot].stream;
	int ret = __real_inflate(zs, flush);
	markRange((uintptr_t)in0, (size_t)(zs->next_in - in0), 2);
	size_t made = (size_t)(zs->next_out - out0);
	stream *s = &g_Streams[sid];
	if (made) {
		streamGrow(s, tout + made);
		if (tout + made > s->len) {
			__real_memcpy(s->data + tout, out0, made);
			s->len = tout + made;
		}
		tagAdd((uintptr_t)out0, (uintptr_t)out0 + made, sid, tout);
	}
	if (ret == Z_STREAM_END) g_Z[slot].zs = NULL;
	return ret;
}

int __wrap_deflate(z_streamp zs, int flush)
{
	const uint8_t *in0 = zs->next_in;
	int ret = __real_deflate(zs, flush);
	markRange((uintptr_t)in0, (size_t)(zs->next_in - in0), 2);
	return ret;
}

size_t __wrap_fwrite(const void *p, size_t a, size_t b, FILE *f)
{
	markRange((uintptr_t)p, a * b, 2);
	return __real_fwrite(p, a, b, f);
}

/* ------------------------------------------------------------------ main */
static void toZ64(uint8_t *r, size_t n)
{
	if (r[0] == 0x37) {
		for (size_t i = 0; i + 1 < n; i += 2) { uint8_t t = r[i]; r[i] = r[i + 1]; r[i + 1] = t; }
	} else if (r[0] == 0x40) {
		for (size_t i = 0; i + 3 < n; i += 4) {
			uint8_t t = r[i]; r[i] = r[i + 3]; r[i + 3] = t;
			t = r[i + 1]; r[i + 1] = r[i + 2]; r[i + 2] = t;
		}
	}
}

static void dumpFile(const char *dir, const char *name, const void *p, size_t n)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f || __real_fwrite(p, 1, n, f) != n) {
		fprintf(stderr, "gecensus: cannot write %s\n", path);
		exit(1);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s ROM OUTDIR DUMPDIR\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *rom = __real_malloc(len);
	if (fread(rom, 1, len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
	fclose(f);
	toZ64(rom, len);

	g_NullRanges = getenv("GECENSUS_NULL");
	if (g_NullRanges) g_NullKey = strtol(g_NullRanges, NULL, 0);

	int rs = streamFor(-1, -1);
	g_Streams[rs].data = rom;
	g_Streams[rs].len = g_Streams[rs].cap = len;
	g_Streams[rs].map = __real_calloc(len, 1);
	tagAdd((uintptr_t)rom, (uintptr_t)rom + len, rs, 0);
	g_Tracking = 1;

	char err[256];
	int ok = geconvertRun(rom, (size_t)len, argv[2], err, sizeof(err));
	g_Tracking = 0;
	if (!ok) {
		fprintf(stderr, "geconvert: %s\n", err);
		return 1;
	}

	char path[2048], name[64];
	snprintf(path, sizeof(path), "%s/streams.json", argv[3]);
	FILE *js = fopen(path, "w");
	if (!js) { perror(path); return 1; }
	fprintf(js, "[\n");
	for (int i = 0; i < g_NumStreams; i++) {
		stream *s = &g_Streams[i];
		snprintf(name, sizeof(name), "s%04d.data", i); dumpFile(argv[3], name, s->data, s->len);
		snprintf(name, sizeof(name), "s%04d.map", i); dumpFile(argv[3], name, s->map, s->len);
		if (s->shadow) { snprintf(name, sizeof(name), "s%04d.shadow", i); dumpFile(argv[3], name, s->shadow, s->len); }
		fprintf(js, "%s{\"id\": %d, \"key\": %ld, \"src\": %d, \"len\": %zu, \"null\": %d}\n",
				i ? "," : "", i, s->key, s->srcstream, s->len, s->shadow != NULL);
	}
	fprintf(js, "]\n");
	fclose(js);
	fprintf(stderr, "gecensus: %d streams, %zu tags at the end\n", g_NumStreams, g_NumTags);
	return 0;
}
