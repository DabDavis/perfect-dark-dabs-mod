#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "config.h"
#include "system.h"
#include "fs.h"
#include "langpack.h"

/**
 * Language packs. See langpack.h and CLAUDE-notes/languages.md.
 *
 * A pack is the text tools/langpack/build.py writes: "@name", "@script" and
 * "@fallback" header lines, then "key<TAB>value" lines with \\, \n and \t
 * escaped. It is copied, unescaped in place and indexed: Perfect Dark's ids
 * in a table by text id (langGet() asks for every string drawn, every frame),
 * everything else in one hash table. A pack once loaded is never freed, since
 * a string handed out may be held by a menu until its next layout; switching
 * back to it reuses it.
 */

#define LANGPACK_MAX 32
#define LANGPACK_CODELEN 16
#define LANGPACK_NAMELEN 64
#define LANGPACK_PD_IDS (128 * 512)

// hash namespaces
#define NS_PORT 1
#define NS_GE   2   // ge.<bank>.<slot>
#define NS_GEEN 3   // ge:<English>

struct langpackentry {
	u32 hash;
	u8 ns;
	const char *key;    // without its trailing newline
	u32 keylen;
	const char *value;  // without its trailing newline for NS_PORT/NS_GEEN
	char *valuenl;      // value + "\n", made the first time it is asked for
};

struct langpack {
	char code[LANGPACK_CODELEN];
	char name[LANGPACK_NAMELEN];
	s32 script;
	s32 accents;        // draws a character past ASCII
	const char *src;    // the pack's text as found (embedded or file)
	u32 srclen;
	char *filepath;     // a dropped-in pack's file, read on first use
	s32 loaded;
	char *text;         // unescaped copy
	const char **pd;    // [LANGPACK_PD_IDS]
	struct langpackentry *entries;
	u32 numentries;
	u32 tablesize;      // power of two
	u32 *table;         // index + 1 into entries, 0 empty
};

struct langpackembedded {
	const char *code;
	const char *text;
	unsigned int len;
};

extern const struct langpackembedded g_LangPacksEmbedded[];
extern const int g_LangNumPacksEmbedded;

static struct langpack g_Packs[LANGPACK_MAX];
static s32 g_NumPacks = 1; // [0] is the ROM's English
static s32 g_Selected = 0;
static struct langpack *g_Cur = NULL;
static u32 g_Generation = 1;
static s32 g_LogMissing = 0;

static char g_LanguageSetting[LANGPACK_CODELEN] = "";

PD_CONSTRUCTOR static void langpackConfigInit(void)
{
	configRegisterString("Game.Language", g_LanguageSetting, sizeof(g_LanguageSetting));
}

static s32 langpackStrCaseEq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;

		if (ca != cb) {
			return 0;
		}
	}

	return *a == *b;
}

static char *langpackStrDup(const char *s)
{
	const u32 len = strlen(s);
	char *d = malloc(len + 1);

	if (d) {
		memcpy(d, s, len + 1);
	}

	return d;
}

static u32 langpackHash(u8 ns, const char *s, u32 len)
{
	u32 h = 2166136261u ^ ns;

	for (u32 i = 0; i < len; i++) {
		h ^= (u8)s[i];
		h *= 16777619u;
	}

	return h ? h : 1;
}

/**
 * Unescapes s in place up to its end (a tab, newline or NUL), writes a NUL
 * there and answers the length. *next is where the scan stopped.
 */
static u32 langpackUnescape(char *s, char **next, char *term)
{
	char *r = s;
	char *w = s;

	while (*r && *r != '\t' && *r != '\n' && *r != '\r') {
		if (*r == '\\' && r[1]) {
			r++;
			switch (*r) {
			case 'n': *w++ = '\n'; break;
			case 't': *w++ = '\t'; break;
			case '0': *w++ = '\0'; break; // a NUL inside a string ("name\0|subheading")
			default:  *w++ = *r; break;
			}
			r++;
		} else {
			*w++ = *r++;
		}
	}

	*next = r;
	*term = *r;
	*w = '\0';

	return w - s;
}

static void langpackHeaderLine(struct langpack *pack, const char *key, const char *value)
{
	if (strcmp(key, "@name") == 0) {
		snprintf(pack->name, sizeof(pack->name), "%s", value);
	} else if (strcmp(key, "@script") == 0) {
		pack->script = strcmp(value, "cjk") == 0 ? LANGPACK_SCRIPT_CJK : LANGPACK_SCRIPT_LATIN;
	}
}

/**
 * Reads the header lines only, for the dropdown: a pack is loaded whole the
 * first time it is chosen.
 */
static void langpackReadHeader(struct langpack *pack, const char *src, u32 len)
{
	const char *p = src;
	const char *end = src + len;

	while (p < end && *p == '@') {
		char key[32];
		char value[LANGPACK_NAMELEN * 2];
		const char *tab = memchr(p, '\t', end - p);
		const char *nl = memchr(p, '\n', end - p);
		char *dummy;
		char dummyterm;

		if (!nl) {
			nl = end;
		}

		if (!tab || tab > nl || tab - p >= (s32)sizeof(key) || nl - tab - 1 >= (s32)sizeof(value)) {
			break;
		}

		memcpy(key, p, tab - p);
		key[tab - p] = '\0';
		memcpy(value, tab + 1, nl - tab - 1);
		value[nl - tab - 1] = '\0';
		langpackUnescape(value, &dummy, &dummyterm);
		langpackHeaderLine(pack, key, value);

		p = nl + 1;
	}

	if (pack->name[0] == '\0') {
		snprintf(pack->name, sizeof(pack->name), "%s", pack->code);
	}
}

static void langpackTableInsert(struct langpack *pack, u32 index)
{
	struct langpackentry *e = &pack->entries[index];
	u32 mask = pack->tablesize - 1;
	u32 slot = e->hash & mask;

	while (pack->table[slot]) {
		struct langpackentry *o = &pack->entries[pack->table[slot] - 1];

		if (o->hash == e->hash && o->ns == e->ns && o->keylen == e->keylen && memcmp(o->key, e->key, e->keylen) == 0) {
			pack->table[slot] = index + 1; // a later line wins
			return;
		}

		slot = (slot + 1) & mask;
	}

	pack->table[slot] = index + 1;
}

static s32 langpackLoad(struct langpack *pack)
{
	char *p;
	u32 numlines = 0;
	u32 i;

	if (pack->loaded) {
		return pack->loaded > 0;
	}

	pack->loaded = -1;

	if (pack->filepath) {
		u32 size = 0;
		char *data = fsFileLoadPadded(pack->filepath, &size, 1);

		if (!data) {
			sysLogPrintf(LOG_WARNING, "lang: could not read %s", pack->filepath);
			return 0;
		}

		pack->src = data;
		pack->srclen = size;
	}

	pack->text = malloc(pack->srclen + 1);
	pack->pd = calloc(LANGPACK_PD_IDS, sizeof(*pack->pd));

	if (!pack->text || !pack->pd) {
		return 0;
	}

	memcpy(pack->text, pack->src, pack->srclen);
	pack->text[pack->srclen] = '\0';

	for (i = 0; i < pack->srclen; i++) {
		if (pack->text[i] == '\n') {
			numlines++;
		}
	}

	pack->entries = calloc(numlines + 1, sizeof(*pack->entries));
	pack->tablesize = 64;

	while (pack->tablesize < (numlines + 1) * 2) {
		pack->tablesize <<= 1;
	}

	pack->table = calloc(pack->tablesize, sizeof(*pack->table));

	if (!pack->entries || !pack->table) {
		return 0;
	}

	p = pack->text;

	while (*p) {
		char *key = p;
		char *value;
		char *next;
		char term;
		u32 keylen;
		u32 valuelen;

		if (*p == '\n' || *p == '\r') {
			p++;
			continue;
		}

		keylen = langpackUnescape(key, &next, &term);

		if (term != '\t') {
			// a line with no value: skip it
			p = term ? next + 1 : next;
			continue;
		}

		value = next + 1;
		valuelen = langpackUnescape(value, &next, &term);
		p = next;

		// past the rest of the line (a stray tab and whatever follows it)
		if (term == '\t' || term == '\r') {
			p++;

			while (*p && *p != '\n') p++;
		}

		if (*p || term == '\n') {
			p++;
		}

		if (key[0] == '@') {
			langpackHeaderLine(pack, key, value);
			continue;
		}

		for (i = 0; i < valuelen; i++) {
			if ((u8)value[i] >= 0x80) {
				pack->accents = 1;
				break;
			}
		}

		if (strncmp(key, "pd.", 3) == 0) {
			u32 id = strtoul(key + 3, NULL, 16);

			if (id < LANGPACK_PD_IDS) {
				pack->pd[id] = value;
			}
		} else {
			struct langpackentry *e = &pack->entries[pack->numentries];
			u8 ns;

			if (strncmp(key, "port:", 5) == 0) {
				ns = NS_PORT;
				key += 5;
				keylen -= 5;
			} else if (strncmp(key, "ge:", 3) == 0) {
				ns = NS_GEEN;
				key += 3;
				keylen -= 3;
			} else if (strncmp(key, "ge.", 3) == 0) {
				ns = NS_GE;
			} else {
				continue;
			}

			if (ns != NS_GE) {
				if (keylen && key[keylen - 1] == '\n') {
					key[--keylen] = '\0';
				}

				if (valuelen && value[valuelen - 1] == '\n') {
					value[--valuelen] = '\0';
				}
			}

			e->ns = ns;
			e->key = key;
			e->keylen = keylen;
			e->value = value;
			e->hash = langpackHash(ns, key, keylen);

			langpackTableInsert(pack, pack->numentries++);
		}
	}

	pack->loaded = 1;

	sysLogPrintf(LOG_NOTE, "lang: loaded %s (%s): %u keyed strings", pack->code, pack->name, pack->numentries);

	return 1;
}

static struct langpackentry *langpackFind(struct langpack *pack, u8 ns, const char *key, u32 keylen)
{
	u32 hash;
	u32 slot;
	u32 mask;

	if (!pack || !pack->table) {
		return NULL;
	}

	hash = langpackHash(ns, key, keylen);
	mask = pack->tablesize - 1;
	slot = hash & mask;

	while (pack->table[slot]) {
		struct langpackentry *e = &pack->entries[pack->table[slot] - 1];

		if (e->hash == hash && e->ns == ns && e->keylen == keylen && memcmp(e->key, key, keylen) == 0) {
			return e;
		}

		slot = (slot + 1) & mask;
	}

	return NULL;
}

/**
 * The value, with a trailing newline if the English asked with one.
 */
static const char *langpackValue(struct langpackentry *e, s32 newline)
{
	if (!newline) {
		return e->value;
	}

	if (!e->valuenl) {
		u32 len = strlen(e->value);

		e->valuenl = malloc(len + 2);

		if (!e->valuenl) {
			return e->value;
		}

		memcpy(e->valuenl, e->value, len);
		e->valuenl[len] = '\n';
		e->valuenl[len + 1] = '\0';
	}

	return e->valuenl;
}

static s32 langpackIndexOfCode(const char *code)
{
	for (s32 i = 1; i < g_NumPacks; i++) {
		if (langpackStrCaseEq(g_Packs[i].code, code)) {
			return i;
		}
	}

	return -1;
}

static struct langpack *langpackAdd(const char *code)
{
	s32 index = langpackIndexOfCode(code);
	struct langpack *pack;

	if (index > 0) {
		// A dropped-in pack replaces the built-in one of its code, so a
		// translator can try a change without rebuilding the game.
		pack = &g_Packs[index];
		memset(pack, 0, sizeof(*pack));
	} else if (g_NumPacks < LANGPACK_MAX) {
		pack = &g_Packs[g_NumPacks++];
	} else {
		return NULL;
	}

	snprintf(pack->code, sizeof(pack->code), "%s", code);

	return pack;
}

static void langpackScanFile(const char *name, void *arg)
{
	const char *dir = arg;
	u32 len = strlen(name);
	char code[LANGPACK_CODELEN];
	char path[FS_MAXPATH];
	struct langpack *pack;
	u32 size = 0;
	char *data;

	if (len <= 5 || !langpackStrCaseEq(name + len - 5, ".lang") || len - 5 >= sizeof(code)) {
		return;
	}

	memcpy(code, name, len - 5);
	code[len - 5] = '\0';
	snprintf(path, sizeof(path), "%s/%s", dir, name);

	data = fsFileLoadPadded(path, &size, 1);

	if (!data) {
		return;
	}

	pack = langpackAdd(code);

	if (pack) {
		pack->filepath = langpackStrDup(path);
		langpackReadHeader(pack, data, size);
		sysLogPrintf(LOG_NOTE, "lang: found %s (%s) in %s", pack->code, pack->name, dir);
	}

	free(data);
}

void langpackInit(void)
{
	const char *arg;
	const char *want = g_LanguageSetting;
	s32 index;

	snprintf(g_Packs[0].code, sizeof(g_Packs[0].code), "%s", "");
	snprintf(g_Packs[0].name, sizeof(g_Packs[0].name), "%s", "English (US)");

	for (s32 i = 0; i < g_LangNumPacksEmbedded; i++) {
		struct langpack *pack = langpackAdd(g_LangPacksEmbedded[i].code);

		if (pack) {
			pack->src = g_LangPacksEmbedded[i].text;
			pack->srclen = g_LangPacksEmbedded[i].len;
			langpackReadHeader(pack, pack->src, pack->srclen);
		}
	}

	fsScanDir("$E/lang", langpackScanFile, "$E/lang");

	g_LogMissing = sysArgCheck("--lang-log-missing");

	arg = sysArgGetString("--lang");

	if (arg) {
		want = arg;
	}

	index = want[0] ? langpackIndexOfCode(want) : 0;

	if (index < 0) {
		sysLogPrintf(LOG_WARNING, "lang: no language \"%s\"; English is used", want);
		index = 0;
	}

	langpackSelect(index, 0);
}

s32 langpackGetCount(void)
{
	return g_NumPacks;
}

const char *langpackGetName(s32 index)
{
	return index >= 0 && index < g_NumPacks ? g_Packs[index].name : "";
}

const char *langpackGetCode(s32 index)
{
	return index >= 0 && index < g_NumPacks ? g_Packs[index].code : "";
}

s32 langpackGetSelected(void)
{
	return g_Selected;
}

u32 langpackGeneration(void)
{
	return g_Generation;
}

s32 langpackActive(void)
{
	return g_Cur != NULL;
}

s32 langpackScript(void)
{
	return g_Cur ? g_Cur->script : LANGPACK_SCRIPT_LATIN;
}

s32 langpackNeedsAccentSpacing(void)
{
	return g_Cur && g_Cur->script == LANGPACK_SCRIPT_LATIN && g_Cur->accents;
}

extern void langOnLanguageChanging(void);
extern void langOnLanguageChanged(void);

void langpackSelect(s32 index, s32 save)
{
	struct langpack *prev = g_Cur;

	if (index < 0 || index >= g_NumPacks) {
		index = 0;
	}

	if (index > 0 && !langpackLoad(&g_Packs[index])) {
		sysLogPrintf(LOG_WARNING, "lang: %s could not be loaded; English is used", g_Packs[index].code);
		index = 0;
	}

	if ((index > 0 ? &g_Packs[index] : NULL) != prev) {
		langOnLanguageChanging();
	}

	g_Selected = index;
	g_Cur = index > 0 ? &g_Packs[index] : NULL;

	if (save) {
		snprintf(g_LanguageSetting, sizeof(g_LanguageSetting), "%s", g_Packs[index].code);
	}

	if (g_Cur != prev) {
		g_Generation++;
		sysLogPrintf(LOG_NOTE, "lang: language is now %s", g_Packs[index].name);
		langOnLanguageChanged();
	}
}

const char *langpackPd(s32 textid)
{
	if (!g_Cur || textid < 0 || textid >= LANGPACK_PD_IDS) {
		return NULL;
	}

	return g_Cur->pd[textid];
}

const char *langpackGe(const char *bank, s32 slot)
{
	char key[48];
	struct langpackentry *e;
	s32 len;

	if (!g_Cur || !bank) {
		return NULL;
	}

	len = snprintf(key, sizeof(key), "ge.%s.%d", bank, slot);

	if (len <= 0 || len >= (s32)sizeof(key)) {
		return NULL;
	}
	e = langpackFind(g_Cur, NS_GE, key, len);

	return e ? e->value : NULL;
}

static const char *langpackFindText(u8 ns, const char *en)
{
	struct langpackentry *e;
	u32 len;
	s32 newline;

	if (!g_Cur || !en || !en[0]) {
		return NULL;
	}

	len = strlen(en);
	newline = en[len - 1] == '\n';

	if (newline) {
		len--;
	}

	e = langpackFind(g_Cur, ns, en, len);

	return e ? langpackValue(e, newline) : NULL;
}

const char *langpackGeText(const char *en)
{
	return langpackFindText(NS_GEEN, en);
}

/**
 * Logs a drawn port string the pack has no translation for, once each, under
 * --lang-log-missing: the list a translator works through.
 */
static void langpackLogMissing(const char *en)
{
	static u32 seen[4096];
	static u32 numseen;
	u32 h;

	if (!g_LogMissing || !en || !en[0]) {
		return;
	}

	h = langpackHash(NS_PORT, en, strlen(en));

	for (u32 i = 0; i < numseen; i++) {
		if (seen[i] == h) {
			return;
		}
	}

	if (numseen < sizeof(seen) / sizeof(seen[0])) {
		seen[numseen++] = h;
	}

	{
		// one line, newlines shown as \n, so the log can be pasted as keys
		char buf[512];
		u32 w = 0;

		for (const char *p = en; *p && w < sizeof(buf) - 3; p++) {
			if (*p == '\n') {
				buf[w++] = '\\';
				buf[w++] = 'n';
			} else {
				buf[w++] = *p;
			}
		}

		buf[w] = '\0';
		sysLogPrintf(LOG_NOTE, "lang: missing [%s] \"%s\"", g_Cur ? g_Cur->code : "", buf);
	}
}

const char *langTrFind(const char *en)
{
	return langpackFindText(NS_PORT, en);
}

const char *langTr(const char *en)
{
	const char *s;

	if (!g_Cur || !en) {
		return en;
	}

	s = langpackFindText(NS_PORT, en);

	if (s) {
		return s;
	}

	langpackLogMissing(en);

	return en;
}

const char *langTrCtx(const char *ctx, const char *en)
{
	char key[512];
	const char *s;

	if (!g_Cur || !en) {
		return en;
	}

	if (ctx && snprintf(key, sizeof(key), "%s|%s", ctx, en) < (s32)sizeof(key)) {
		s = langpackFindText(NS_PORT, key);

		if (s) {
			return s;
		}
	}

	return langTr(en);
}

/**
 * For --lang-audit: the selected pack's i-th keyed string, NULL past the end.
 * *ns is 1 for a port string and 3 for GoldenEye's by English (both keyed by
 * their English, *key), 2 for GoldenEye's by id.
 */
const char *langpackEntry(u32 i, const char **key, s32 *ns)
{
	if (!g_Cur || i >= g_Cur->numentries) {
		return NULL;
	}

	*key = g_Cur->entries[i].key;
	*ns = g_Cur->entries[i].ns;

	return g_Cur->entries[i].value;
}

const char *langpackGeFile(const char *file, s32 slot)
{
	char bank[32];
	u32 len;

	if (!g_Cur || !file) {
		return NULL;
	}

	len = strlen(file);

	// "LdamE" is ge.dam.<slot>
	if (len < 3 || file[0] != 'L' || file[len - 1] != 'E' || len - 2 >= sizeof(bank)) {
		return NULL;
	}

	for (u32 i = 0; i < len - 2; i++) {
		const char c = file[i + 1];

		bank[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
	}

	bank[len - 2] = '\0';

	return langpackGe(bank, slot);
}

const char *langpackSelectedCode(void)
{
	return g_Cur ? g_Cur->code : "";
}
