#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "system.h"
#include "mod.h"
#include "modloader.h"
#include "gexplusrom.h"
#include "geconvert.h"
#include "gebean.h"
#include "game/mplayer/mplayer.h"
#include "xblamesh.h"
#include "net/net.h"
#include "net/nettransport.h"
#include "netint.h"

/**
 * Content follows the host (protocol 13).
 *
 * A host plays with what it has: a mod loaded over the game (GoldenEye X,
 * the Mario characters, All in One), the Stage Loader's maps of other mods,
 * the GoldenEye, Goldfinger 64 and Tomorrow Never Dies 64 conversions made
 * from the ROMs in its added-content/, and the Combat Simulator mode those
 * open. A client is to play the same without being sent a byte of it: every
 * machine owns its copies, and this file makes the client's game use the
 * ones the host named.
 *
 *  - The overlay mod. ACCEPT and RULES carry the host's by its dir name and
 *    a contents hash. The client finds the same name among its installed
 *    mods (modListIndexOf), hashes it (nethash.c's walk: the bytes the
 *    simulation reads, never pictures or text) and, when the bytes are the
 *    host's, switches to it live (modListSwap), keeping its own Mod.ModDir
 *    for pd.ini (H13) and switching back when the session ends. A mod that
 *    holds ROM segments cannot be swapped under a running game (mods.md:
 *    "files swap live; segments cannot"), so for one of those - either the
 *    host's or the one loaded here - the client is told to choose it in
 *    Load Mods, restart and join again. A mod not installed, or not the
 *    host's version, is named. The host checks the one the client then
 *    loaded through the "mod" component of LOADED.
 *
 *  - The Stage Loader's maps and the conversions. STAGE_LOAD's key names the
 *    map's mod dir; one installed here but not mounted (Mod.MapMods left it
 *    out) is mounted on demand (modMapsMountIndex). A conversion missing
 *    here is answered with the file to put in added-content/, since the game
 *    converts it itself at the next start.
 *
 *  - The mode. RULES carry the GoldenEye ROM hack whose Combat Simulator the
 *    host is in (g_GexPlusVariant) as its conversion tag, so the client's
 *    weapon set list holds the same block (gexplus.c) when it reads the
 *    host's set number.
 */

/*
 * The look's data (protocol 15). A look is a picture, each machine's own,
 * but a few choices it makes at a stage's load are the simulation's: on a
 * GoldenEye stage the HD look with the Community Edition loads the
 * converter's _ce copies of a mission's setup, pads and tiles
 * (geRoomCeData()), and on two of Perfect Dark's the release's collision
 * replaces the ROM's while the release is installed (xblaStageLoadTiles()),
 * and on every stage the release's meshes take Penny out of the male guards'
 * heads, which changes the RNG's draws at the load (bodyreset.c).
 * A net match plays the host's choice on every machine: the converter
 * writes the _ce copies into every conversion (and the host serves a missing
 * one), so any client can follow it whatever its own look.
 */
static u32 s_HostLook = 0;
static s32 s_HostLookLatched = 0;

void netContentLookLatch(void)
{
	s_HostLook = (xblaMeshGetEnabled() && gebeanCeIsActive() ? NETLOOK_GECE : 0)
		| (xblaMeshPackageReady(0) ? NETLOOK_XBLATILES : 0)
		| (xblaMeshGetEnabled() ? NETLOOK_MESHES : 0);
	s_HostLookLatched = 1;
	sysLogPrintf(LOG_NOTE, "net: the match plays the host's look data: GoldenEye's Community Edition copies %s, the XBLA release's collision %s, "
			"the guards' heads as the release deals them %s", (s_HostLook & NETLOOK_GECE) ? "on" : "off",
			(s_HostLook & NETLOOK_XBLATILES) ? "on" : "off", (s_HostLook & NETLOOK_MESHES) ? "on" : "off");
}

s32 netLookData(u32 *bits)
{
	if (g_NetMode == NETMODE_SERVER && s_HostLookLatched) {
		*bits = s_HostLook;
		return 1;
	}

	if (g_NetMode == NETMODE_CLIENT && netRulesMatchId()) {
		*bits = netRulesContent()->look;
		return 1;
	}

	return 0;
}

static s32 s_Swapped = 0;        // this machine's mod was switched for the host's
static s32 s_OwnLoaded = -1;     // the mod loaded before (an installed index, -1 none)
static char s_OwnSelected[64];   // Mod.ModDir as it was (pd.ini's value stays the player's)

static const char *contentBasename(const char *path, char *buf, s32 size)
{
	s32 end;
	s32 start;

	if (!path) {
		buf[0] = '\0';
		return buf;
	}

	end = strlen(path);

	while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
		end--;
	}

	start = end;

	while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') {
		start--;
	}

	snprintf(buf, size, "%.*s", end - start, path + start);

	return buf;
}

/** This machine's g_GexPlusVariant as its conversion tag ("gf64"), "" for GoldenEye's own or none */
const char *netContentVariantTag(void)
{
	const char *tag = g_GexPlusVariant ? gexPlusRomDirTag(g_GexPlusVariant) : NULL;

	return tag ? tag : "";
}

/**
 * g_GexPlusVariant from a tag: the hack converted here under that tag, or
 * none for "". Returns 0 when the tag names a hack this machine has not
 * converted (its arenas will not resolve either: STAGE_LOAD says what to
 * add).
 */
s32 netContentVariantApply(const char *tag)
{
	if (!tag || !tag[0]) {
		g_GexPlusVariant = NULL;
		return 1;
	}

	// the hack converted here, or its folder as the host served it
	g_GexPlusVariant = netContentVariantName(tag);

	return g_GexPlusVariant != NULL;
}

/** What this machine plays with, as a host names it */
void netContentHostNeed(struct netcontentneed *n)
{
	const char *overlay = fsGetModDir();

	memset(n, 0, sizeof(*n));

	if (overlay) {
		contentBasename(overlay, n->mod, sizeof(n->mod));
		n->modhash = netHashDirContents(overlay);
	}

	snprintf(n->gevariant, sizeof(n->gevariant), "%s", netContentVariantTag());
	n->look = (u8)s_HostLook;
}

void netContentWrite(struct netbuf *b, const struct netcontentneed *n, s32 withvariant)
{
	netWriteStr(b, n->mod, NET_MAXMAPDIR);
	netWriteU64(b, n->modhash);

	if (withvariant) {
		netWriteStr(b, n->gevariant, NET_MAXCOMPNAME);
		netBufWriteU8(b, n->look); // protocol 15
	}
}

void netContentRead(struct netbuf *b, struct netcontentneed *n, s32 withvariant)
{
	memset(n, 0, sizeof(*n));
	netBufReadString(b, n->mod, sizeof(n->mod));
	n->modhash = netReadU64(b);

	if (withvariant) {
		netBufReadString(b, n->gevariant, sizeof(n->gevariant));
		n->look = netBufReadU8(b) & (NETLOOK_GECE | NETLOOK_XBLATILES | NETLOOK_MESHES);
	}
}

/**
 * Client: make this game's mod the host's. Returns a NETCONTENT_* result;
 * for anything but OK and SWAPPED, text says what the player can do.
 */
s32 netContentFollow(const struct netcontentneed *n, char *text, s32 textsize)
{
	char loaded[NET_MAXMAPDIR + 1];
	char ownselected[64];
	const char *overlay = fsGetModDir();
	s32 index;
	s32 own;
	s32 selected;
	u64 h;

	contentBasename(overlay, loaded, sizeof(loaded));
	text[0] = '\0';

	if (!n->mod[0]) {
		if (!overlay) {
			return NETCONTENT_OK;
		}

		// the host plays with no mod over the game; this machine has one
		if (modListIsFromArgs()) {
			snprintf(text, textsize, "The host plays with no mod, and this game was started with --moddir %s. Start it without.", loaded);
			return NETCONTENT_RESTART;
		}

		if (!modListSwapIsLive(-1)) {
			snprintf(text, textsize, "The host plays with no mod. %s is loaded here and holds ROM segments, which only a restart takes out: "
					"choose No Mod in Extended Options > Load Mods, Restart Now, and join again.", loaded);
			return NETCONTENT_RESTART;
		}

		index = -1;
	} else {
		if (overlay && strcasecmp(loaded, n->mod) == 0) {
			h = netHashDirContents(overlay);

			if (h == n->modhash) {
				return NETCONTENT_OK;
			}

			snprintf(text, textsize, "The host's %s is not the same as the one loaded here (another version of the mod, or imported by "
					"another version of the game: %016llx there, %016llx here). Install the host's.", n->mod,
					(unsigned long long)n->modhash, (unsigned long long)h);
			return NETCONTENT_DIFFERS;
		}

		index = modListIndexOf(n->mod);

		if (index < 0) {
			snprintf(text, textsize, "The host plays with the mod %s, which is not installed here. Drop it in mods/ (the zip or patch it "
					"came as will do), start the game again, and join again.", n->mod);
			return NETCONTENT_MISSING;
		}

		h = netHashDirContents(modListGetPath(index));

		if (h != n->modhash) {
			snprintf(text, textsize, "The host's %s is not the same as the one installed here (another version of the mod, or imported by "
					"another version of the game: %016llx there, %016llx here). Install the host's.", n->mod,
					(unsigned long long)n->modhash, (unsigned long long)h);
			return NETCONTENT_DIFFERS;
		}

		if (modListIsFromArgs()) {
			snprintf(text, textsize, "The host plays with the mod %s, and this game's mods came from --moddir. Start it with --moddir \"%s\".",
					n->mod, modListGetPath(index));
			return NETCONTENT_RESTART;
		}

		if (!modListSwapIsLive(index)) {
			if (modListHasSegs(index)) {
				snprintf(text, textsize, "The host plays with the mod %s, which holds ROM segments and loads only at a start: choose it in "
						"Extended Options > Load Mods, Restart Now, and join again.", n->mod);
			} else {
				snprintf(text, textsize, "The host plays with the mod %s. %s is loaded here and holds ROM segments, which only a restart "
						"takes out: choose %s in Extended Options > Load Mods, Restart Now, and join again.", n->mod, loaded, n->mod);
			}

			return NETCONTENT_RESTART;
		}
	}

	// a live swap: the player's own choice is kept for pd.ini (H13) and
	// put back when the session ends. Both are taken before the swap, which
	// makes what it loaded the selection (the first run of this wrote the
	// host's mod into the gate client's pd.ini: netsessiontest swap)
	own = overlay ? modListIndexOf(loaded) : -1;
	selected = modListGetSelected();
	snprintf(ownselected, sizeof(ownselected), "%s", modListGetSelectedName());

	if (!modListSwap(index)) {
		snprintf(text, textsize, "The host plays with %s, which this game could not switch to.", n->mod[0] ? n->mod : "no mod");
		return NETCONTENT_RESTART;
	}

	if (!s_Swapped) {
		s_Swapped = 1;
		s_OwnLoaded = own;
		snprintf(s_OwnSelected, sizeof(s_OwnSelected), "%s", ownselected);
	}

	modListSetSelected(selected);
	netSessionHashInvalidate();
	sysLogPrintf(LOG_NOTE, "net: content: switched to %s for the host (this machine's %s comes back after the session)",
			n->mod[0] ? n->mod : "no mod", own >= 0 ? modListGetName(own) : "no mod");

	return NETCONTENT_SWAPPED;
}

/** Client: the session is over; the mod it switched for the host goes, its own comes back */
void netContentRestore(void)
{
	if (!s_Swapped) {
		return;
	}

	s_Swapped = 0;

	if (modListSwapIsLive(s_OwnLoaded) && modListSwap(s_OwnLoaded)) {
		sysLogPrintf(LOG_NOTE, "net: content: back to %s after the session", s_OwnLoaded >= 0 ? modListGetName(s_OwnLoaded) : "no mod");
	} else {
		sysLogPrintf(LOG_WARNING, "net: content: could not switch back to %s after the session; it loads at the next start",
				s_OwnLoaded >= 0 ? modListGetName(s_OwnLoaded) : "no mod");
	}

	// the selection the player made is theirs whatever was swapped
	modListSetSelected(modListIndexOf(s_OwnSelected));
	netSessionHashInvalidate();
}

/**
 * Host migration: a conversion's folder (by its tag) mounted here for its
 * maps, converted from this machine's own ROM or served into memory by a
 * host this session (the user, 2026-10-08: a guest that was served the
 * content can host it after)
 */
static s32 netContentConversionHere(const char *tag)
{
	const char *dir = gexPlusRomDirOfTag(tag);
	char base[NET_MAXMAPDIR + 1];
	s32 i;

	for (i = 0; dir && i < fsGetNumModDirs(); i++) {
		if (strcasecmp(contentBasename(fsGetModDirAt(i), base, sizeof(base)), dir) == 0) {
			return 1;
		}
	}

	return 0;
}

/**
 * Host migration: whether this machine could take over a room that plays
 * with the overlay mod `mod` ("" none), in the conversion or ROM hack mode
 * tagged `ge` ("" none), on the stage `stagekey` names (the room's rules:
 * "map:NAME", "stage:NN", "mission:...", "campaign:..." or ""). The mod must
 * be one this game can switch to live (as a client's following it is); the
 * rest must be mounted here, the game's own or a host's served copy.
 */
s32 netContentCanHost(const char *mod, const char *ge, const char *stagekey)
{
	char loaded[NET_MAXMAPDIR + 1];
	const char *overlay = fsGetModDir();
	s32 index;
	s32 id;

	contentBasename(overlay, loaded, sizeof(loaded));

	if (mod && mod[0]) {
		if (!overlay || strcasecmp(loaded, mod) != 0) {
			index = modListIndexOf(mod);

			if (index < 0 || modListIsFromArgs() || !modListSwapIsLive(index)) {
				return 0;
			}
		}
	} else if (overlay && (modListIsFromArgs() || !modListSwapIsLive(-1))) {
		return 0;
	}

	if (ge && ge[0] && !netContentConversionHere(ge)) {
		return 0;
	}

	if (stagekey && strncmp(stagekey, "map:", 4) == 0) {
		for (id = 1; id <= 0xff; id++) {
			const char *name = modloaderGetStageModDir(id) ? modloaderGetStageMapName(id) : NULL;

			if (name && strcmp(name, stagekey + 4) == 0 && !modloaderStageIsMission(id)) {
				return 1;
			}
		}

		return 0;
	}

	return 1;
}

/**
 * Client: STAGE_LOAD named a map of a mod dir that is not mounted here.
 * Mounted now when it is installed (Mod.MapMods left it out); 1 if so.
 */
s32 netContentMountMaps(const char *dirbase)
{
	const s32 index = modListIndexOf(dirbase);

	if (index < 0) {
		return 0;
	}

	if (modMapsMountIndex(index) < 0) {
		return 0;
	}

	sysLogPrintf(LOG_NOTE, "net: content: %s mounted for its maps for the host's choice", modListGetName(index));
	return 1;
}

// the Combat Simulator's lists hold GoldenEye's characters already (the pool
// built over a conversion, the release's or GoldenEye X's borrowed ones)
static s32 netContentGePoolListed(void)
{
	s32 i;

	for (i = 0; i < (s32)mpGetNumBodies(); i++) {
		if (g_MpBodies[i].bodynum >= GEBEAN_POOL_BASE) {
			return 1;
		}
	}

	return 0;
}

/**
 * Client, at STAGE_LOAD: the host's sims and players wear GoldenEye's
 * characters - rows of gebean.c's pool, which is built from GoldenEye's own
 * conversion wherever it is mounted (gexPlusRomMpBegin()), in GoldenEye's
 * mode and a ROM hack's alike - and this machine's lists lack them: it has
 * no conversion, or one mounted or served since its pool was built. Without
 * them the host's list places are other characters here, or past the list
 * (Dark Combat; Dr. Caroll, with no hand to hold a gun: netcontenttest
 * gffetch, 2026-10-08). The pool is built again over this machine's own
 * copy, mounted now if Mod.MapMods left it out, else over the host's,
 * fetched first (only the host needs the ROM). A pool listed already lacks
 * only what the XBLA release alone has (eleven of its heads the ROM has no
 * head for): their stand-ins are this machine's look, as the release is.
 * 1 while a fetch runs (the STAGE_LOAD is kept); 0 to go on, with the
 * characters or, refused, others in their places.
 */
s32 netContentGeCharsFollow(void)
{
	char rows[256];
	s32 bodies;
	const s32 missing = netRulesGeCharsMissing(rows, sizeof(rows), &bodies);
	const char *getag = geconvertGoldenEyeTag();

	if (missing == 0) {
		return 0;
	}

	if (netContentGePoolListed()) {
		sysLogPrintf(bodies ? LOG_WARNING : LOG_NOTE, "net: content: %d of the host's GoldenEye characters not listed here, %d of them bodies (the XBLA release's alone): %s",
				missing, bodies, rows);
		return 0;
	}

	if (!netContentConversionHere(getag)) {
		netContentMountMaps(GEXPLUSROM_DIR);
	}

	if (netContentConversionHere(getag)) {
		s32 still;

		gebeanPoolRefresh();
		still = netRulesGeCharsMissing(rows, sizeof(rows), &bodies);
		sysLogPrintf(bodies ? LOG_WARNING : LOG_NOTE, "net: content: %d of the host's characters are GoldenEye's; the pool built again over " GEXPLUSROM_DIR
				": %d still missing, %d of them bodies%s%s", missing, still, bodies, still ? ": " : "", rows);
		return 0;
	}

	if (netContentFetchStart(GEXPLUSROM_DIR)) {
		sysLogPrintf(LOG_NOTE, "net: content: %d of the host's characters are GoldenEye's; fetching " GEXPLUSROM_DIR " for them", missing);
		return 1;
	}

	sysLogPrintf(LOG_WARNING, "net: content: %d of the host's characters are GoldenEye's, not listed here; others stand in: %s", missing, rows);
	return 0;
}

/**
 * The text of a NOSTAGE: what the host chose and what this machine needs
 * for it (the conversion's source file in added-content/, or the mod in
 * mods/)
 */
void netContentNoStageText(s32 kind, const char *dir, const char *map, s32 id, char *text, s32 size)
{
	if (kind == 3) {
		// a conversion's mission (co-op, protocol 14)
		const char *tag = gexPlusRomDirTag(dir);
		const char *what = "GoldenEye 007";
		const char *file = "the GoldenEye 007 ROM";
		s32 i;

		for (i = 0; tag && geconvertVariantTagAt(i); i++) {
			if (strcasecmp(tag, geconvertVariantTagAt(i)) == 0) {
				what = geconvertVariantNameAt(i);
				file = "its patch (or the zip it came in) beside the GoldenEye 007 ROM";
			}
		}

		if (tag) {
			snprintf(text, size, "The host chose mission %d of %s, which is not converted here. Put %s in added-content/ and start "
					"the game again: it converts it itself.", id + 1, what, file);
		} else {
			snprintf(text, size, "The host chose mission %d of the mod %s, which is not installed here. Drop the mod in mods/, "
					"start the game again, and join again.", id + 1, dir);
		}

		return;
	}

	if (kind == 1 && gexPlusRomIsConversionDir(dir)) {
		const char *tag = gexPlusRomDirTag(dir);
		const char *what = "GoldenEye 007";
		const char *file = "the GoldenEye 007 ROM";
		s32 i;

		for (i = 0; tag && geconvertVariantTagAt(i); i++) {
			if (strcasecmp(tag, geconvertVariantTagAt(i)) == 0) {
				what = geconvertVariantNameAt(i);
				file = "its patch (or the zip it came in) beside the GoldenEye 007 ROM";
			}
		}

		snprintf(text, size, "The host chose %s, a %s arena, which is not converted here. Put %s in added-content/ and start the game "
				"again: it converts it itself.", map, what, file);
	} else if (kind == 1) {
		snprintf(text, size, "The host chose map %s from the mod %s, which is not installed here. Drop the mod in mods/, start the game "
				"again, and join again.", map, dir);
	} else if (kind == 2) {
		snprintf(text, size, "The host chose stage 0x%02x of the mod %s, which is not loaded here.", id, dir);
	} else {
		snprintf(text, size, "The host chose stage 0x%02x, which cannot be loaded here.", id);
	}
}

/*
 * Content served by the host (protocol 14).
 *
 * A guest that has not got the conversion or map mod a stage key names
 * (GoldenEye Arenas, Goldfinger 64, a Stage Loader mod) asks the host for
 * the directory rather than leaving: only the host needs the ROM and its
 * conversion. The files come in parts on the BULK channel and are held in
 * one of fs.c's memory directories ("$N/<name>"), which is mounted for its
 * maps and read by the mod loader, the textures, the GoldenEye tables and
 * everything else through the same file calls as a folder on disk. Nothing
 * is written to the guest's disk, nothing is offered to anyone outside the
 * room's sessions, and the memory goes with the process. The host serves only a
 * directory it has mounted for its maps (never its overlay mod, which a
 * guest could not take live anyway), leaving out what nothing in play
 * reads: text, caches, the converter's and importer's own notes. A guest
 * that takes a room over (host migration) serves what it was served in turn.
 */

#define NETCONTENT_PART      (48 * 1024)        // a CONTENT_FILE's bytes
#define NETCONTENT_MAXBYTES  (256u * 1024 * 1024)
#define NETCONTENT_MAXFILES  20000
#define NETCONTENT_PERTICK   2                  // parts a host sends a client each tick (about 5.6 MB/s)
#define NETCONTENT_MAXPATH   255
#define NETCONTENT_MAXPEERS  (MAX_PLAYERS + 2)  // netsession.c's NET_MAXPEERS
#define NETCONTENT_MAXQUEUED (2u * 1024 * 1024) // a peer's bytes ENet has queued or in flight before the next part waits
#define NETCONTENT_MAXASKS   2                  // serves of one folder to one connection (a retry after a failure)
#define NETCONTENT_MAXSESSIONDIRS 4             // the folders this session's stages named

struct netservefile {
	char *rel;
	u32 size;
};

struct netserve {
	s32 active;
	s32 peer;
	char dir[NET_MAXMAPDIR + 1];
	char full[FS_MAXPATH + 1];
	struct netservefile *files;
	s32 nfiles;
	s32 cap;
	u32 bytes;
	s32 cur;          // the file being sent
	u32 curoff;       // how far into it
	u8 *curdata;      // its bytes, loaded whole
	u32 cursize;
	u32 sentbytes;
	u64 started;
};

static struct netserve s_Serve[NETCONTENT_MAXPEERS];

// host: what each connection has been served this session, folder by folder
// (a peer asking for the same folder over and over is refused)
static struct {
	char dir[NETCONTENT_MAXSESSIONDIRS][NET_MAXMAPDIR + 1];
	u8 count[NETCONTENT_MAXSESSIONDIRS];
} s_ServeAsks[NETCONTENT_MAXPEERS];

// host: the map folders this session's STAGE_LOADs named (the only folders,
// with GoldenEye's own conversion for its characters, a guest may ask for)
static char s_SessionDirs[NETCONTENT_MAXSESSIONDIRS][NET_MAXMAPDIR + 1];
static s32 s_SessionDirsNext;
static u8 s_ContentBuf[NETCONTENT_PART + 1024];

static char *netContentStrDup(const char *s)
{
	const size_t len = strlen(s) + 1;
	char *d = malloc(len);

	if (d) {
		memcpy(d, s, len);
	}

	return d;
}

// client: the fetch under way, and what it is for
static struct {
	s32 active;
	char dir[NET_MAXMAPDIR + 1];
	s32 memdir;
	u32 nfiles;
	u32 bytes;
	u32 gotfiles;
	u32 gotbytes;
	char curpath[NETCONTENT_MAXPATH + 1];
	u8 *curdata;
	u32 cursize;
	u32 curgot;
	u64 started;
	u64 lastlog;
} s_Fetch;

static char s_FetchFailed[4][NET_MAXMAPDIR + 1]; // dirs the host would not serve: asked once
static s32 s_FetchFailedCount;

/*
 * Host: serving
 */

static s32 netContentSkipName(const char *name, s32 isdir)
{
	static const char *dirs[] = { "cache", "screenshots", "traces", "crashreports", "texture-dumps", "model-dumps", "recordings" };
	static const char *exts[] = { ".txt", ".md", ".log", ".ini", ".pdf", ".html", ".zip", ".7z", ".rar", ".xdelta", ".bps", ".ips", ".n64", ".z64", ".v64" };
	const char *dot;
	u32 i;

	if (name[0] == '.') {
		return 1;
	}

	// the one text file that matters: the mod's own config (its maps,
	// missions and models blocks); the converter's and importer's notes,
	// readmes and changelogs are text too and stay home
	if (!isdir && strcasecmp(name, "modconfig.txt") == 0) {
		return 0;
	}

	if (isdir) {
		for (i = 0; i < ARRAYCOUNT(dirs); i++) {
			if (strcasecmp(name, dirs[i]) == 0) {
				return 1;
			}
		}

		return 0;
	}

	dot = strrchr(name, '.');

	if (dot) {
		for (i = 0; i < ARRAYCOUNT(exts); i++) {
			if (strcasecmp(dot, exts[i]) == 0) {
				return 1;
			}
		}
	}

	return 0;
}

struct netcontentnames {
	char **names;
	s32 count;
	s32 max;
};

static void netContentNamesAdd(const char *name, void *arg)
{
	struct netcontentnames *n = arg;

	if (n->count == n->max) {
		s32 max = n->max ? n->max * 2 : 64;
		char **names = realloc(n->names, sizeof(char *) * max);

		if (!names) {
			return;
		}

		n->names = names;
		n->max = max;
	}

	n->names[n->count] = netContentStrDup(name);

	if (n->names[n->count]) {
		n->count++;
	}
}

static int netContentNameCmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

// the files under full/rel, in name order, into sv; 0 when a cap is hit
static s32 netContentList(struct netserve *sv, const char *full, const char *rel, s32 depth)
{
	struct netcontentnames n = { NULL, 0, 0 };
	char childfull[FS_MAXPATH + 1];
	char childrel[NETCONTENT_MAXPATH + 1];
	s32 ok = 1;
	s32 i;

	if (depth > 8 || fsScanDir(full, netContentNamesAdd, &n) < 0) {
		return 1;
	}

	qsort(n.names, n.count, sizeof(char *), netContentNameCmp);

	for (i = 0; i < n.count && ok; i++) {
		s32 isdir;
		s32 size;

		snprintf(childfull, sizeof(childfull), "%s/%s", full, n.names[i]);
		snprintf(childrel, sizeof(childrel), "%s%s%s", rel, rel[0] ? "/" : "", n.names[i]);

		if (strlen(rel) + strlen(n.names[i]) + 1 > NETCONTENT_MAXPATH) {
			continue;
		}

		isdir = fsScanDir(childfull, NULL, NULL) >= 0;

		if (netContentSkipName(n.names[i], isdir)) {
			continue;
		}

		if (isdir) {
			ok = netContentList(sv, childfull, childrel, depth + 1);
			continue;
		}

		size = fsFileSize(childfull);

		if (size < 0) {
			continue;
		}

		if (sv->nfiles >= NETCONTENT_MAXFILES || sv->bytes + (u32)size > NETCONTENT_MAXBYTES) {
			ok = 0;
			break;
		}

		if (sv->nfiles == sv->cap) {
			const s32 cap = sv->cap ? sv->cap * 2 : 512;
			struct netservefile *files = realloc(sv->files, sizeof(struct netservefile) * cap);

			if (!files) {
				ok = 0;
				break;
			}

			sv->files = files;
			sv->cap = cap;
		}

		sv->files[sv->nfiles].rel = netContentStrDup(childrel);
		sv->files[sv->nfiles].size = (u32)size;
		sv->nfiles++;
		sv->bytes += (u32)size;
	}

	for (i = 0; i < n.count; i++) {
		free(n.names[i]);
	}

	free(n.names);
	return ok;
}

static void netContentServeFree(struct netserve *sv)
{
	s32 i;

	for (i = 0; i < sv->nfiles; i++) {
		free(sv->files[i].rel);
	}

	free(sv->files);

	if (sv->curdata) {
		sysMemFree(sv->curdata);
	}

	memset(sv, 0, sizeof(*sv));
	sv->peer = -1;
}

void netContentServeStop(s32 peer)
{
	if (peer >= 0 && peer < NETCONTENT_MAXPEERS && s_Serve[peer].active) {
		sysLogPrintf(LOG_NOTE, "net: content: peer %d gone %u of %u bytes into %s", peer, s_Serve[peer].sentbytes,
				s_Serve[peer].bytes, s_Serve[peer].dir);
		netContentServeFree(&s_Serve[peer]);
	}
}

/**
 * Host: a STAGE_LOAD names the map folder dirbase (kinds 1 and 3): a folder
 * this session's guests may be served
 */
void netContentHostStageDir(const char *dirbase)
{
	s32 i;

	if (!dirbase || !dirbase[0]) {
		return;
	}

	for (i = 0; i < NETCONTENT_MAXSESSIONDIRS; i++) {
		if (strcasecmp(s_SessionDirs[i], dirbase) == 0) {
			return;
		}
	}

	snprintf(s_SessionDirs[s_SessionDirsNext], sizeof(s_SessionDirs[0]), "%s", dirbase);
	s_SessionDirsNext = (s_SessionDirsNext + 1) % NETCONTENT_MAXSESSIONDIRS;
}

// a folder this session's content needs: a stage's (the map's conversion or
// map mod, a migrated host's "$N/" copy by its name), or GoldenEye's own
// conversion while this machine's lists hold its characters (a guest without
// them fetches it: netContentGeCharsFollow)
static s32 netContentSessionNeeds(const char *dir)
{
	s32 i;

	for (i = 0; i < NETCONTENT_MAXSESSIONDIRS; i++) {
		if (s_SessionDirs[i][0] && strcasecmp(s_SessionDirs[i], dir) == 0) {
			return 1;
		}
	}

	return strcasecmp(dir, GEXPLUSROM_DIR) == 0 && netContentGePoolListed();
}

// one more serve of dir to peer's connection; 0 when it has had its share
static s32 netContentServeAsk(s32 peer, const char *dir)
{
	s32 i;
	s32 freeslot = -1;

	for (i = 0; i < NETCONTENT_MAXSESSIONDIRS; i++) {
		if (s_ServeAsks[peer].dir[i][0] && strcasecmp(s_ServeAsks[peer].dir[i], dir) == 0) {
			if (s_ServeAsks[peer].count[i] >= NETCONTENT_MAXASKS) {
				return 0;
			}

			s_ServeAsks[peer].count[i]++;
			return 1;
		}

		if (!s_ServeAsks[peer].dir[i][0] && freeslot < 0) {
			freeslot = i;
		}
	}

	if (freeslot < 0) {
		return 0;
	}

	snprintf(s_ServeAsks[peer].dir[freeslot], sizeof(s_ServeAsks[peer].dir[0]), "%s", dir);
	s_ServeAsks[peer].count[freeslot] = 1;
	return 1;
}

/**
 * Host: a new connection on peer: what an earlier one on the index asked
 * for is forgotten (and anything still being sent stopped)
 */
void netContentPeerReset(s32 peer)
{
	if (peer >= 0 && peer < NETCONTENT_MAXPEERS) {
		netContentServeStop(peer);
		memset(&s_ServeAsks[peer], 0, sizeof(s_ServeAsks[peer]));
	}
}

/**
 * Host: a CONTENT_REQ. The dir must be one mounted here for its maps (not
 * the overlay); the reply is CONTENT_BEGIN and the parts follow a tick at a
 * time, or CONTENT_NO with why.
 */
void netContentServeRequest(s32 peer, struct netbuf *b)
{
	struct netserve *sv;
	struct netbuf out;
	char dir[NET_MAXMAPDIR + 1];
	char base[NET_MAXMAPDIR + 1];
	const char *found = NULL;
	const char *why = NULL;
	s32 i;

	netBufReadString(b, dir, sizeof(dir));

	if (!netBufOk(b) || peer < 0 || peer >= NETCONTENT_MAXPEERS || !dir[0]) {
		return;
	}

	sv = &s_Serve[peer];

	if (sv->active) {
		why = "one transfer at a time";
	} else if (!netContentSessionNeeds(dir)) {
		// checked before the folder is walked: only what this session's
		// stages need (a joined peer could otherwise have any mounted
		// folder walked and sent, again and again)
		why = "the host's match does not use that folder";
	} else if (!netContentServeAsk(peer, dir)) {
		why = "that folder was sent to you already";
	}

	// a folder a host served this machine (fs.c's "$N/<name>") is served on
	// too: a guest that took a room over (host migration) hosts its
	// conversion from that copy, and its own guests need it as it did
	for (i = fsGetNumOverlayModDirs(); !why && i < fsGetNumModDirs(); i++) {
		const char *d = fsGetModDirAt(i);

		if (d && strcasecmp(contentBasename(d, base, sizeof(base)), dir) == 0) {
			found = d;
		}
	}

	if (!why && !found) {
		why = "the host has no such folder mounted for its maps";
	}

	if (!why) {
		netContentServeFree(sv);
		sv->peer = peer;
		snprintf(sv->dir, sizeof(sv->dir), "%s", dir);
		snprintf(sv->full, sizeof(sv->full), "%s", found);

		if (!netContentList(sv, found, "", 0)) {
			why = "the folder is too large to send";
			netContentServeFree(sv);
		} else if (sv->nfiles == 0) {
			why = "the folder has no files to send";
			netContentServeFree(sv);
		}
	}

	netBufInitWrite(&out, s_ContentBuf, sizeof(s_ContentBuf));

	if (why) {
		netBufWriteU8(&out, NETMSG_CONTENT_NO);
		netWriteStr(&out, dir, NET_MAXMAPDIR);
		netWriteStr(&out, why, NET_MAXTEXT);
		netSessionSendPeer(peer, NET_CHAN_BULK, out.data, netBufLen(&out));
		sysLogPrintf(LOG_NOTE, "net: content: peer %d asked for %s: %s", peer, dir, why);
		return;
	}

	sv->active = 1;
	sv->started = sysGetMicroseconds();
	netBufWriteU8(&out, NETMSG_CONTENT_BEGIN);
	netWriteStr(&out, dir, NET_MAXMAPDIR);
	netBufWriteU32(&out, (u32)sv->nfiles);
	netBufWriteU32(&out, sv->bytes);
	netSessionSendPeer(peer, NET_CHAN_BULK, out.data, netBufLen(&out));
	sysLogPrintf(LOG_NOTE, "net: content: serving %s to peer %d: %d files, %u bytes", dir, peer, sv->nfiles, sv->bytes);
}

s32 netContentServingPeer(s32 peer)
{
	return peer >= 0 && peer < NETCONTENT_MAXPEERS && s_Serve[peer].active;
}

s32 netContentServing(void)
{
	s32 i;

	for (i = 0; i < NETCONTENT_MAXPEERS; i++) {
		if (s_Serve[i].active) {
			return 1;
		}
	}

	return 0;
}

// one part of the current file to the peer; 1 when the dir is done
static s32 netContentServePart(struct netserve *sv)
{
	struct netbuf out;
	u32 len;

	if (sv->cur >= sv->nfiles) {
		netBufInitWrite(&out, s_ContentBuf, sizeof(s_ContentBuf));
		netBufWriteU8(&out, NETMSG_CONTENT_END);
		netWriteStr(&out, sv->dir, NET_MAXMAPDIR);
		netBufWriteU32(&out, (u32)sv->nfiles);
		netBufWriteU32(&out, sv->bytes);
		netSessionSendPeer(sv->peer, NET_CHAN_BULK, out.data, netBufLen(&out));
		sysLogPrintf(LOG_NOTE, "net: content: %s served to peer %d: %d files, %u bytes in %u ms", sv->dir, sv->peer, sv->nfiles,
				sv->sentbytes, (u32)((sysGetMicroseconds() - sv->started) / 1000));
		return 1;
	}

	if (!sv->curdata) {
		char path[FS_MAXPATH + 1];
		u32 size = 0;

		snprintf(path, sizeof(path), "%s/%s", sv->full, sv->files[sv->cur].rel);
		sv->curdata = fsFileLoad(path, &size);
		sv->cursize = sv->curdata ? size : 0;
		sv->curoff = 0;

		if (!sv->curdata && sv->files[sv->cur].size > 0) {
			// gone since it was listed: an empty file in its place keeps
			// the count right; the stage hash tells if it mattered
			sysLogPrintf(LOG_WARNING, "net: content: %s could not be read; sent empty", path);
		}
	}

	len = sv->cursize - sv->curoff;

	if (len > NETCONTENT_PART) {
		len = NETCONTENT_PART;
	}

	netBufInitWrite(&out, s_ContentBuf, sizeof(s_ContentBuf));
	netBufWriteU8(&out, NETMSG_CONTENT_FILE);
	netWriteStr(&out, sv->files[sv->cur].rel, NETCONTENT_MAXPATH);
	netBufWriteU32(&out, sv->cursize);
	netBufWriteU32(&out, sv->curoff);
	netBufWriteU16(&out, (u16)len);

	if (len) {
		netBufWriteBytes(&out, sv->curdata + sv->curoff, len);
	}

	netSessionSendPeer(sv->peer, NET_CHAN_BULK, out.data, netBufLen(&out));
	sv->curoff += len;
	sv->sentbytes += len;

	if (sv->curoff >= sv->cursize) {
		if (sv->curdata) {
			sysMemFree(sv->curdata);
		}

		sv->curdata = NULL;
		sv->cursize = 0;
		sv->cur++;
	}

	return 0;
}

/** Host, each tick: a few parts to every client being served */
void netContentServeTick(void)
{
	// the gates (netmigratetest midfetch, netlobbytest fetchend): a part
	// every N ticks (--net-test-serve-pace N), so a fetch is still running
	// when a match ends; the host gone as a player quits once it has sent B
	// bytes of a folder (--net-test-serve-quit B), mid-transfer
	static s32 pace = -1;
	static s32 quitat = -1;
	static u32 tick;
	s32 i;
	s32 k;

	if (pace < 0) {
		pace = sysArgGetInt("--net-test-serve-pace", 0);
		quitat = sysArgGetInt("--net-test-serve-quit", 0);
	}

	if (pace > 1 && (++tick % (u32)pace) != 0) {
		return;
	}

	for (i = 0; i < NETCONTENT_MAXPEERS; i++) {
		struct netserve *sv = &s_Serve[i];

		if (!sv->active) {
			continue;
		}

		for (k = 0; k < (pace > 1 ? 1 : NETCONTENT_PERTICK); k++) {
			if (quitat > 0 && sv->sentbytes >= (u32)quitat) {
				sysLogPrintf(LOG_NOTE, "net: content: --net-test-serve-quit: %u of %u bytes of %s sent to peer %d; quitting the game",
						sv->sentbytes, sv->bytes, sv->dir, sv->peer);
				fflush(stdout);
				exit(0);
			}

			// paced by what ENet still holds for the peer: a slow link
			// gets the parts as it takes them, never a queue of the folder
			if (netHostPeerQueuedBytes(g_NetHostSocket, sv->peer) > NETCONTENT_MAXQUEUED) {
				break;
			}

			if (netContentServePart(sv)) {
				netContentServeFree(sv);
				break;
			}
		}
	}
}

/*
 * Client: fetching
 */

s32 netContentFetching(void)
{
	return s_Fetch.active;
}

const char *netContentFetchStatus(char *out, s32 size)
{
	if (!s_Fetch.active) {
		out[0] = '\0';
	} else if (!s_Fetch.bytes) {
		snprintf(out, size, "Getting %s from the host...", s_Fetch.dir);
	} else {
		snprintf(out, size, "Getting %s from the host: %u%% (%u of %u files)", s_Fetch.dir,
				(u32)((u64)s_Fetch.gotbytes * 100 / (s_Fetch.bytes ? s_Fetch.bytes : 1)), s_Fetch.gotfiles, s_Fetch.nfiles);
	}

	return out;
}

/**
 * Client: a stage key named dir, which is not here: ask the host for it.
 * 1 when asked (the STAGE_LOAD waits for CONTENT_END); 0 when it was
 * refused before, a fetch is under way, or the dir is already here.
 */
s32 netContentFetchStart(const char *dir)
{
	struct netbuf out;
	s32 i;

	if (!dir || !dir[0] || strchr(dir, '/') || strchr(dir, '\\') || strcmp(dir, ".") == 0 || strcmp(dir, "..") == 0) {
		return 0;
	}

	// asked already (a STAGE_LOAD of the next match while the last one's
	// fetch runs): that one is waited for
	if (s_Fetch.active) {
		return strcasecmp(s_Fetch.dir, dir) == 0;
	}

	if (fsMemDirFind(dir) >= 0) {
		return 0;
	}

	for (i = 0; i < s_FetchFailedCount; i++) {
		if (strcasecmp(s_FetchFailed[i], dir) == 0) {
			return 0;
		}
	}

	memset(&s_Fetch, 0, sizeof(s_Fetch));
	s_Fetch.active = 1;
	s_Fetch.memdir = -1;
	s_Fetch.started = sysGetMicroseconds();
	snprintf(s_Fetch.dir, sizeof(s_Fetch.dir), "%s", dir);

	netBufInitWrite(&out, s_ContentBuf, sizeof(s_ContentBuf));
	netBufWriteU8(&out, NETMSG_CONTENT_REQ);
	netWriteStr(&out, dir, NET_MAXMAPDIR);
	netSessionSendServer(NET_CHAN_RELIABLE, out.data, netBufLen(&out), NET_SEND_RELIABLE);
	sysLogPrintf(LOG_NOTE, "net: content: asking the host for %s", dir);

	return 1;
}

// the fetch's memory freed (a half-filled folder never stays) and forgotten
static void netContentFetchClear(void)
{
	if (s_Fetch.curdata) {
		free(s_Fetch.curdata);
	}

	if (s_Fetch.memdir >= 0) {
		fsMemDirDestroy(s_Fetch.memdir);
	}

	memset(&s_Fetch, 0, sizeof(s_Fetch));
	s_Fetch.memdir = -1;
}

static void netContentFetchFail(const char *why)
{
	if (s_FetchFailedCount < (s32)ARRAYCOUNT(s_FetchFailed)) {
		snprintf(s_FetchFailed[s_FetchFailedCount++], sizeof(s_FetchFailed[0]), "%s", s_Fetch.dir);
	}

	sysLogPrintf(LOG_WARNING, "net: content: %s not fetched: %s", s_Fetch.dir, why);
	netContentFetchClear();
}

/**
 * Client: the session ended (the host quit or went, a migration, a leave)
 * with a fetch under way: dropped, so the next session's host can serve it
 * whole (the user's case: a guest being served GoldenEye's conversion when
 * its host went, and the next host serving it again)
 */
void netContentFetchAbort(void)
{
	if (s_Fetch.active) {
		sysLogPrintf(LOG_NOTE, "net: content: the session ended %u of %u bytes into %s; dropped", s_Fetch.gotbytes, s_Fetch.bytes, s_Fetch.dir);
	}

	netContentFetchClear();
}

/**
 * The session is over, either side: a fetch dropped, every transfer
 * stopped, and what this session refused or allowed forgotten
 */
void netContentSessionEnd(void)
{
	s32 i;

	netContentFetchAbort();
	s_FetchFailedCount = 0;
	memset(s_FetchFailed, 0, sizeof(s_FetchFailed));

	for (i = 0; i < NETCONTENT_MAXPEERS; i++) {
		netContentPeerReset(i);
	}

	memset(s_SessionDirs, 0, sizeof(s_SessionDirs));
	s_SessionDirsNext = 0;
}

// CONTENT_BEGIN
void netContentFetchBegin(struct netbuf *b)
{
	char dir[NET_MAXMAPDIR + 1];
	u32 nfiles;
	u32 bytes;

	netBufReadString(b, dir, sizeof(dir));
	nfiles = netBufReadU32(b);
	bytes = netBufReadU32(b);

	if (!netBufOk(b) || !s_Fetch.active || strcasecmp(dir, s_Fetch.dir) != 0) {
		return;
	}

	if (s_Fetch.memdir >= 0) {
		netContentFetchFail("the host began the folder twice");
		return;
	}

	if (nfiles == 0 || nfiles > NETCONTENT_MAXFILES || bytes > NETCONTENT_MAXBYTES) {
		netContentFetchFail("the host's folder is too large");
		return;
	}

	s_Fetch.memdir = fsMemDirCreate(dir);

	if (s_Fetch.memdir < 0) {
		netContentFetchFail("no room for another folder in memory");
		return;
	}

	s_Fetch.nfiles = nfiles;
	s_Fetch.bytes = bytes;
	sysLogPrintf(LOG_NOTE, "net: content: the host serves %s: %u files, %u bytes", dir, nfiles, bytes);
}

// CONTENT_FILE
void netContentFetchFile(struct netbuf *b)
{
	char path[NETCONTENT_MAXPATH + 1];
	u32 size;
	u32 offset;
	u32 len;

	netBufReadString(b, path, sizeof(path));
	size = netBufReadU32(b);
	offset = netBufReadU32(b);
	len = netBufReadU16(b);

	if (!netBufOk(b) || netBufRemaining(b) != (s32)len || !s_Fetch.active || s_Fetch.memdir < 0) {
		return;
	}

	if (offset == 0) {
		// a new file
		if (s_Fetch.curdata) {
			free(s_Fetch.curdata);
			s_Fetch.curdata = NULL;
		}

		// never more than CONTENT_BEGIN announced: a host streaming files
		// for ever is cut off at once, not at CONTENT_END
		if (s_Fetch.gotfiles >= s_Fetch.nfiles) {
			netContentFetchFail("more files came than the host announced");
			return;
		}

		if (size > s_Fetch.bytes - s_Fetch.gotbytes) {
			netContentFetchFail("more bytes came than the host announced");
			return;
		}

		snprintf(s_Fetch.curpath, sizeof(s_Fetch.curpath), "%s", path);
		s_Fetch.cursize = size;
		s_Fetch.curgot = 0;
		s_Fetch.curdata = size ? malloc(size) : NULL;

		if (size && !s_Fetch.curdata) {
			netContentFetchFail("out of memory");
			return;
		}
	} else if (strcmp(path, s_Fetch.curpath) != 0 || offset != s_Fetch.curgot || size != s_Fetch.cursize) {
		netContentFetchFail("a part came out of order");
		return;
	}

	if (offset + len > size) {
		netContentFetchFail("a part ran past its file");
		return;
	}

	if (len) {
		netBufReadBytes(b, s_Fetch.curdata + offset, len);
	}

	s_Fetch.curgot += len;
	s_Fetch.gotbytes += len;

	if (s_Fetch.curgot >= s_Fetch.cursize) {
		if (fsMemDirAddFile(s_Fetch.memdir, s_Fetch.curpath, s_Fetch.curdata, s_Fetch.cursize) != 0) {
			netContentFetchFail("a file could not be kept");
			return;
		}

		free(s_Fetch.curdata);
		s_Fetch.curdata = NULL;
		s_Fetch.gotfiles++;
	}

	if (sysGetMicroseconds() - s_Fetch.lastlog > 2000000) {
		s_Fetch.lastlog = sysGetMicroseconds();
		sysLogPrintf(LOG_NOTE, "net: content: %s: %u of %u files, %u of %u bytes", s_Fetch.dir, s_Fetch.gotfiles, s_Fetch.nfiles,
				s_Fetch.gotbytes, s_Fetch.bytes);
	}
}

// CONTENT_NO
void netContentFetchNo(struct netbuf *b)
{
	char dir[NET_MAXMAPDIR + 1];
	char text[NET_MAXTEXT + 1];

	netBufReadString(b, dir, sizeof(dir));
	netBufReadString(b, text, sizeof(text));

	if (!netBufOk(b) || !s_Fetch.active || strcasecmp(dir, s_Fetch.dir) != 0) {
		return;
	}

	netContentFetchFail(text);
}

/**
 * CONTENT_END: the folder is whole; sealed, mounted for its maps and its
 * maps registered, so the stage key that asked for it resolves now. 1 when
 * the caller should try the STAGE_LOAD it kept; 0 when the fetch failed.
 */
s32 netContentFetchEnd(struct netbuf *b)
{
	char dir[NET_MAXMAPDIR + 1];
	u32 nfiles;
	u32 bytes;
	s32 mount;

	netBufReadString(b, dir, sizeof(dir));
	nfiles = netBufReadU32(b);
	bytes = netBufReadU32(b);

	if (!netBufOk(b) || !s_Fetch.active || strcasecmp(dir, s_Fetch.dir) != 0 || s_Fetch.memdir < 0) {
		return 0;
	}

	if (s_Fetch.gotfiles != nfiles || s_Fetch.gotbytes != bytes) {
		netContentFetchFail("not every file arrived");
		return 0;
	}

	fsMemDirSeal(s_Fetch.memdir);
	mount = fsAddMapsDir(fsMemDirPath(s_Fetch.memdir));

	if (mount < 0) {
		netContentFetchFail("could not mount the folder");
		return 0;
	}

	modloaderAddDir(mount);
	netSessionHashInvalidate();
	sysLogPrintf(LOG_NOTE, "net: content: %s fetched from the host and mounted for its maps: %u files, %u bytes in %u ms", dir, nfiles, bytes,
			(u32)((sysGetMicroseconds() - s_Fetch.started) / 1000));

	memset(&s_Fetch, 0, sizeof(s_Fetch));
	s_Fetch.memdir = -1;

	return 1;
}

/**
 * The name a conversion's mode is chosen by (g_GexPlusVariant) for a tag:
 * the hack converted here, else a folder of the tag's name mounted here
 * (one the host served), else NULL
 */
const char *netContentVariantName(const char *tag)
{
	static char mounted[4][NET_MAXMAPDIR + 1];
	static s32 nmounted;
	const char *dir;
	char base[NET_MAXMAPDIR + 1];
	s32 i;

	if (!tag || !tag[0]) {
		return NULL;
	}

	dir = gexPlusRomDirOfTag(tag);

	for (i = 0; dir && gexPlusRomGetVariant(i); i++) {
		if (strcasecmp(gexPlusRomGetVariant(i), dir) == 0) {
			return gexPlusRomGetVariant(i);
		}
	}

	for (i = 0; dir && i < fsGetNumModDirs(); i++) {
		if (strcasecmp(contentBasename(fsGetModDirAt(i), base, sizeof(base)), dir) == 0) {
			s32 k;

			for (k = 0; k < nmounted; k++) {
				if (strcasecmp(mounted[k], dir) == 0) {
					return mounted[k];
				}
			}

			if (nmounted < (s32)ARRAYCOUNT(mounted)) {
				snprintf(mounted[nmounted], sizeof(mounted[0]), "%s", dir);
				return mounted[nmounted++];
			}
		}
	}

	return NULL;
}
