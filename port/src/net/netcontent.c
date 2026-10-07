#include <stdio.h>
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
#include "net/net.h"
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
	const char *name;
	s32 i;

	if (!tag || !tag[0]) {
		g_GexPlusVariant = NULL;
		return 1;
	}

	name = gexPlusRomDirOfTag(tag);

	for (i = 0; name && gexPlusRomGetVariant(i); i++) {
		if (strcasecmp(gexPlusRomGetVariant(i), name) == 0) {
			g_GexPlusVariant = gexPlusRomGetVariant(i);
			return 1;
		}
	}

	g_GexPlusVariant = NULL;
	return 0;
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
}

void netContentWrite(struct netbuf *b, const struct netcontentneed *n, s32 withvariant)
{
	netWriteStr(b, n->mod, NET_MAXMAPDIR);
	netWriteU64(b, n->modhash);

	if (withvariant) {
		netWriteStr(b, n->gevariant, NET_MAXCOMPNAME);
	}
}

void netContentRead(struct netbuf *b, struct netcontentneed *n, s32 withvariant)
{
	memset(n, 0, sizeof(*n));
	netBufReadString(b, n->mod, sizeof(n->mod));
	n->modhash = netReadU64(b);

	if (withvariant) {
		netBufReadString(b, n->gevariant, sizeof(n->gevariant));
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

/**
 * The text of a NOSTAGE: what the host chose and what this machine needs
 * for it (the conversion's source file in added-content/, or the mod in
 * mods/)
 */
void netContentNoStageText(s32 kind, const char *dir, const char *map, s32 id, char *text, s32 size)
{
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
