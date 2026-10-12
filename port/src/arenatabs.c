/**
 * The Combat Simulator's Arena list in tabs (F3 20261012-114949, dab: "organize
 * the stage selector with tabs for each game ... so is quick and easy to
 * distinguish").
 *
 * With the Stage Loader on, every installed mod's maps are arenas after
 * "Random" (modloader.c), and a tester's list ran to 168 rows: Perfect Dark's
 * seventeen, then GoldenEye's, GoldenEye X's, Goldfinger 64's and the rest,
 * told apart only by the mod in brackets after each name. The list is now one
 * tab a game, each listing only its own maps:
 *
 *   - the game's own list, rows the Stage Loader did not register: Perfect
 *     Dark's (Dark, Classic, Random, and the mod-dir arenas past them), or a
 *     loaded mod's own list (mpImportArenas()), named after the mod;
 *   - GoldenEye (GEXPLUSROM_DIR's maps);
 *   - GE-X: GoldenEye X's maps, the patch imported (GE-X_6a_01-19-25) or the
 *     All in One Mod's pack (mod_gex), by name;
 *   - one per GoldenEye ROM hack's conversion (geconvertVariantNameAt():
 *     Goldfinger 64, Tomorrow Never Dies 64), with any pack of the same name
 *     (the All in One Mod's mod_goldfinger_64);
 *   - Mods: every other mod's maps.
 *
 * Only tabs with a map listed are shown, and none at all when there is one
 * (a stock install draws the dialog it always drew). GE Plus's own Combat
 * Simulator lists GoldenEye's alone and keeps no tabs. The game's own tab is
 * listed by mpArenaMenuHandler() as it always was, its rows filtered
 * (arenaTabsRowShown()); every other tab by arenaTabsListHandler() here: a
 * group a mod, under the mod's name, the mod's own folder before the All in
 * One Mod's packs, and each map without the "(mod)" the loader put after it.
 *
 * The tabs only choose what the list shows. What is chosen is still
 * g_MpSetup.stagenum, set from a row of g_MpArenas, so Random (in Perfect
 * Dark's tab, as it always was), a saved setup and an online room's arena -
 * the same dialog is Create Room's Arena page - are what they were, and
 * nothing new goes on the wire.
 */

#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/menu.h"
#include "game/menuitem.h"
#include "game/challenge.h"
#include "game/mplayer/setup.h"
#include "fs.h"
#include "mod.h"
#include "modloader.h"
#include "geconvert.h"
#include "langpack.h"
#include "arenatabs.h"

#define ARENATABS_MAX  12
#define ARENATABS_ROWS ((s32)ARRAYCOUNT(g_MpArenas))
#define ARENATABS_DIRS 128

// a tab's place in the row: the game's own list first, Mods last
#define TABKEY_MAIN       0
#define TABKEY_GOLDENEYE  1
#define TABKEY_GEX        2
#define TABKEY_CONVERSION 3 // + the conversion's number in geconvert's table
#define TABKEY_MODS       100

struct arenatab {
	s32 key;
	char name[32];
	char shortname[16];
};

static struct arenatab s_Tabs[ARENATABS_MAX];
static s32 s_NumTabs;

// the rows as they were worked out: stage, unlocked, mod dir (-1 the game's own) and tab key
static s32 s_NumRows = -1;
static s16 s_RowStage[ARENATABS_ROWS];
static u8 s_RowListed[ARENATABS_ROWS];
static s16 s_RowDir[ARENATABS_ROWS];
static s16 s_RowKey[ARENATABS_ROWS];
static s32 s_GexPlusMode;
static s32 s_NumModDirs;
static const char *s_LoadedName;
static u32 s_Generation;

// a mod dir's name in its group's header, the label the loader put after its
// maps' names, and whether it is one of the All in One Mod's packs
static char s_DirName[ARENATABS_DIRS][40];
static char s_DirLabel[ARENATABS_DIRS][40];
static u8 s_DirPack[ARENATABS_DIRS];

// the tab each player's menu shows, by key
static s32 s_ShownKey[MAX_PLAYERS];

// a game's tab as its list shows it: rows in order, and its groups
static s16 s_Order[ARENATABS_ROWS];
static s32 s_OrderCount;
static s16 s_GroupDir[ARENATABS_DIRS];
static s16 s_GroupStart[ARENATABS_DIRS];
static s32 s_NumGroups;
static s32 s_OrderKey = -1;
static u32 s_OrderGeneration;

// the All in One Mod's map packs (its folder of mod_ dirs, made for Jonaeru's
// port), named as the bundle names them
static const struct {
	const char *dir;
	const char *name;
} s_PackNames[] = {
	{ "mod_gex",           "GE-X (All in One)" },
	{ "mod_goldfinger_64", "Goldfinger 64 (All in One)" },
	{ "mod_kakariko",      "Kakariko (All in One)" },
	{ "mod_dark_noon",     "Dark Noon (All in One)" },
	{ "mod_allinone",      "All in One" },
};

static const char *arenaTabsBase(const char *dir)
{
	const char *base = dir;

	for (; dir && *dir; dir++) {
		if ((*dir == '/' || *dir == '\\') && dir[1]) {
			base = dir + 1;
		}
	}

	return base;
}

// a folder's name as a menu shows it: the font draws '_' as a bar over the line
static void arenaTabsShowName(const char *name, char *out, size_t len)
{
	size_t i = 0;

	for (; name && name[i] && i + 1 < len; i++) {
		out[i] = name[i] == '_' ? ' ' : name[i];
	}

	out[i] = '\0';

	while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '/' || out[i - 1] == '\\')) {
		out[--i] = '\0';
	}
}

// GoldenEye X by its folder's name: GE-X_6a_01-19-25, GEX_5a_09-22-12_Cloudless,
// the bundle's mod_gex, "GoldenEye X 5e"
static s32 arenaTabsNameIsGex(const char *name)
{
	char flat[16];
	size_t n = 0;

	for (; *name && n + 1 < sizeof(flat); name++) {
		if ((*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9')) {
			flat[n++] = *name;
		} else if (*name >= 'A' && *name <= 'Z') {
			flat[n++] = *name - 'A' + 'a';
		}
	}

	flat[n] = '\0';

	return strncmp(flat, "gex", 3) == 0 || strncmp(flat, "goldeneyex", 10) == 0;
}

static s32 arenaTabsDirKey(s32 dirindex)
{
	const char *dir = fsGetModDirAt(dirindex);
	const char *base = arenaTabsBase(dir);
	char name[64];

	if (!dir) {
		return TABKEY_MODS;
	}

	if (modloaderDirIndexIsGexPlus(dirindex)) {
		return TABKEY_GOLDENEYE;
	}

	// a pack's "mod_goldfinger_64" is the conversion's "Goldfinger 64"
	arenaTabsShowName(strncasecmp(base, "mod_", 4) == 0 ? base + 4 : base, name, sizeof(name));

	for (s32 v = 0; geconvertVariantNameAt(v) && TABKEY_CONVERSION + v < TABKEY_MODS; v++) {
		if (strcasecmp(name, geconvertVariantNameAt(v)) == 0) {
			return TABKEY_CONVERSION + v;
		}
	}

	if (arenaTabsNameIsGex(name)) {
		return TABKEY_GEX;
	}

	return TABKEY_MODS;
}

static void arenaTabsDirNames(s32 dirindex)
{
	const char *base = arenaTabsBase(fsGetModDirAt(dirindex));

	// as modloaderAddDir() names the mod after a map: "mod_" left off
	snprintf(s_DirLabel[dirindex], sizeof(s_DirLabel[0]), "%s", strncasecmp(base, "mod_", 4) == 0 ? base + 4 : base);
	s_DirPack[dirindex] = strncasecmp(base, "mod_", 4) == 0;

	if (modloaderDirIndexIsGexPlus(dirindex)) {
		snprintf(s_DirName[dirindex], sizeof(s_DirName[0]), "%s", LANG_N("GoldenEye"));
		return;
	}

	for (s32 i = 0; i < (s32)ARRAYCOUNT(s_PackNames); i++) {
		if (strcasecmp(base, s_PackNames[i].dir) == 0) {
			snprintf(s_DirName[dirindex], sizeof(s_DirName[0]), "%s", s_PackNames[i].name);
			return;
		}
	}

	arenaTabsShowName(base, s_DirName[dirindex], sizeof(s_DirName[0]));
}

static void arenaTabsNameTab(struct arenatab *tab)
{
	const char *full = NULL;
	const char *shortname = NULL;

	switch (tab->key) {
	case TABKEY_MAIN:
		if (s_LoadedName) {
			// the loaded mod's own list, its own arenas (mpImportArenas()):
			// the mod's name, and for short its first word of three letters
			// or more ("PD_Kakariko" is Kakariko)
			const char *word = tab->name;
			s32 len;

			arenaTabsShowName(s_LoadedName, tab->name, sizeof(tab->name));

			while (*word) {
				len = strcspn(word, " ");

				if (len >= 3 || word[len] == '\0') {
					break;
				}

				word += len + 1;
			}

			len = strcspn(word, " ");

			if (len <= 0) {
				word = tab->name;
				len = strlen(word);
			}

			snprintf(tab->shortname, sizeof(tab->shortname), "%.*s", len < 10 ? len : 10, word);
			return;
		}

		full = LANG_N("Perfect Dark");
		shortname = LANG_N("PD");
		break;
	case TABKEY_GOLDENEYE:
		full = LANG_N("GoldenEye");
		shortname = LANG_N("GE");
		break;
	case TABKEY_GEX:
		full = LANG_N("GE-X");
		shortname = LANG_N("GE-X");
		break;
	case TABKEY_MODS:
		full = LANG_N("Mods");
		shortname = LANG_N("Mods");
		break;
	default: {
		// a conversion: its name, or its tag where the name is long
		const char *variant = geconvertVariantNameAt(tab->key - TABKEY_CONVERSION);
		const char *tag = geconvertVariantTagAt(tab->key - TABKEY_CONVERSION);
		s32 i;

		for (i = 0; tag && tag[i] && i + 1 < (s32)sizeof(tab->shortname); i++) {
			tab->shortname[i] = tag[i] >= 'a' && tag[i] <= 'z' ? tag[i] - 'a' + 'A' : tag[i];
		}

		tab->shortname[i] = '\0';
		snprintf(tab->name, sizeof(tab->name), "%s", variant && strlen(variant) <= 14 ? variant : tab->shortname);
		return;
	}
	}

	snprintf(tab->name, sizeof(tab->name), "%s", full);
	snprintf(tab->shortname, sizeof(tab->shortname), "%s", shortname);
}

/**
 * The loaded mod whose own arena list (mpImportArenas()) is the game's own
 * tab, which is then named after it; NULL when the list is Perfect Dark's,
 * a mod that kept it included.
 */
static const char *arenaTabsLoadedName(void)
{
	return fsGetNumOverlayModDirs() > 0 && g_MpArenasImported ? modListGetLoadedName() : NULL;
}

static s32 arenaTabsIndexOfKey(s32 key)
{
	for (s32 i = 0; i < s_NumTabs; i++) {
		if (s_Tabs[i].key == key) {
			return i;
		}
	}

	return -1;
}

static s32 arenaTabsPlayer(void)
{
	return g_MpPlayerNum >= 0 && g_MpPlayerNum < MAX_PLAYERS ? g_MpPlayerNum : 0;
}

// the tab this player's menu shows, as an index into s_Tabs
static s32 arenaTabsShown(void)
{
	const s32 index = arenaTabsIndexOfKey(s_ShownKey[arenaTabsPlayer()]);

	return index >= 0 ? index : 0;
}

static void arenaTabsRebuild(s32 numrows)
{
	u8 dirdone[ARENATABS_DIRS];
	u8 present[TABKEY_MODS + 1];
	s32 i;

	memset(dirdone, 0, sizeof(dirdone));
	memset(present, 0, sizeof(present));

	s_Generation++;
	s_NumRows = numrows;
	s_GexPlusMode = g_GexPlusMode;
	s_NumModDirs = fsGetNumModDirs();
	s_LoadedName = arenaTabsLoadedName();
	s_NumTabs = 0;

	for (i = 0; i < numrows; i++) {
		const s32 stagenum = g_MpArenas[i].stagenum;
		s32 dir = stagenum > 0 ? modloaderGetStageModDirIndex(stagenum) : -1;
		s32 key = TABKEY_MAIN;

		if (dir >= ARENATABS_DIRS) {
			dir = -1;
		}

		if (dir >= 0) {
			if (!dirdone[dir]) {
				arenaTabsDirNames(dir);
				dirdone[dir] = 1;
			}

			key = arenaTabsDirKey(dir);
		}

		s_RowStage[i] = stagenum;
		s_RowListed[i] = challengeIsFeatureUnlocked(g_MpArenas[i].requirefeature) ? 1 : 0;
		s_RowDir[i] = dir;
		s_RowKey[i] = key;

		if (s_RowListed[i] && key >= 0 && key <= TABKEY_MODS) {
			present[key] = 1;
		}
	}

	// GE Plus's own Combat Simulator lists GoldenEye's arenas alone
	if (g_GexPlusMode) {
		return;
	}

	for (i = 0; i <= TABKEY_MODS && s_NumTabs < ARENATABS_MAX; i++) {
		if (present[i]) {
			s_Tabs[s_NumTabs].key = i;
			arenaTabsNameTab(&s_Tabs[s_NumTabs]);
			s_NumTabs++;
		}
	}

	// a loaded mod's tab short-named as another (GoldenEye X entered beside the
	// All in One Mod's GE-X pack: both "GE-X") is its name's first two words
	for (i = 1; s_NumTabs > 0 && s_Tabs[0].key == TABKEY_MAIN && s_LoadedName && i < s_NumTabs; i++) {
		if (strcasecmp(s_Tabs[0].shortname, s_Tabs[i].shortname) == 0) {
			const char *name = s_Tabs[0].name;
			s32 len = strcspn(name, " ");

			if (name[len] == ' ') {
				len += 1 + strcspn(name + len + 1, " ");
			}

			snprintf(s_Tabs[0].shortname, sizeof(s_Tabs[0].shortname), "%.*s", len < 12 ? len : 12, name);
			break;
		}
	}
}

void arenaTabsSync(void)
{
	s32 numrows = mpGetNumStages();
	s32 same;
	s32 i;

	if (numrows > ARENATABS_ROWS) {
		numrows = ARENATABS_ROWS;
	}

	same = numrows == s_NumRows && s_GexPlusMode == g_GexPlusMode && s_NumModDirs == fsGetNumModDirs()
		&& s_LoadedName == arenaTabsLoadedName();

	for (i = 0; same && i < numrows; i++) {
		same = s_RowStage[i] == g_MpArenas[i].stagenum
			&& s_RowListed[i] == (challengeIsFeatureUnlocked(g_MpArenas[i].requirefeature) ? 1 : 0);
	}

	if (!same) {
		arenaTabsRebuild(numrows);
	}
}

s32 arenaTabsActive(void)
{
	return s_NumTabs >= 2;
}

s32 arenaTabsRowShown(s32 i)
{
	if (!arenaTabsActive()) {
		return true;
	}

	if (i < 0 || i >= s_NumRows) {
		return false;
	}

	return s_RowKey[i] == s_Tabs[arenaTabsShown()].key;
}

s32 arenaTabsOwnList(void)
{
	return arenaTabsActive() && s_Tabs[arenaTabsShown()].key != TABKEY_MAIN;
}

/**
 * The shown game's tab in its list's order: a group a mod dir, the mod's own
 * folder (a conversion, an imported patch) before the All in One Mod's packs
 * of the same game, each mod's maps as registered.
 */
static void arenaTabsOrder(void)
{
	const s32 key = s_Tabs[arenaTabsShown()].key;
	s16 dirs[ARENATABS_DIRS];
	s32 numdirs = 0;
	s32 i;

	if (s_OrderKey == key && s_OrderGeneration == s_Generation) {
		return;
	}

	s_OrderKey = key;
	s_OrderGeneration = s_Generation;
	s_OrderCount = 0;
	s_NumGroups = 0;

	// the dirs with maps in the tab, by their first map
	for (i = 0; i < s_NumRows; i++) {
		s32 j;

		if (!s_RowListed[i] || s_RowKey[i] != key || s_RowDir[i] < 0) {
			continue;
		}

		for (j = 0; j < numdirs && dirs[j] != s_RowDir[i]; j++);

		if (j == numdirs && numdirs < ARENATABS_DIRS) {
			dirs[numdirs++] = s_RowDir[i];
		}
	}

	for (s32 pack = 0; pack < 2; pack++) {
		for (s32 d = 0; d < numdirs; d++) {
			if (s_DirPack[dirs[d]] != pack) {
				continue;
			}

			s_GroupDir[s_NumGroups] = dirs[d];
			s_GroupStart[s_NumGroups] = s_OrderCount;
			s_NumGroups++;

			for (i = 0; i < s_NumRows; i++) {
				if (s_RowListed[i] && s_RowKey[i] == key && s_RowDir[i] == dirs[d]) {
					s_Order[s_OrderCount++] = i;
				}
			}
		}
	}
}

/**
 * Row i's name as a game's tab lists it: without the mod modloaderAddStage()
 * put after it ("%s (%.*s)", the label cut to what the row's thirty characters
 * left), which the tab and the group already say.
 */
static char *arenaTabsRowName(s32 i, char *name)
{
	static char bufs[4][48];
	static s32 next;
	const char *label;
	const char *open = NULL;
	s32 len;
	s32 j;

	if (!name || i < 0 || i >= s_NumRows || s_RowDir[i] < 0) {
		return name;
	}

	label = s_DirLabel[s_RowDir[i]];
	len = strlen(name);

	while (len > 0 && name[len - 1] == '\n') {
		len--;
	}

	if (len < 4 || name[len - 1] != ')') {
		return name;
	}

	for (j = len - 2; j > 0; j--) {
		if (name[j] == '(' && name[j - 1] == ' ') {
			open = &name[j - 1];
			break;
		}
	}

	if (!open) {
		return name;
	}

	j = (s32)(&name[len - 1] - (open + 2));

	if (j <= 0 || strncmp(open + 2, label, j) != 0) {
		return name;
	}

	next = (next + 1) % 4;
	snprintf(bufs[next], sizeof(bufs[0]), "%.*s\n", (s32)(open - name), name);

	return bufs[next];
}

MenuItemHandlerResult arenaTabsListHandler(s32 operation, struct menuitem *item, union handlerdata *data)
{
	arenaTabsOrder();

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = s_OrderCount;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->list.value < (uintptr_t)s_OrderCount) {
			const s32 row = s_Order[data->list.value];

			return (uintptr_t)arenaTabsRowName(row, mpGetArenaName(row));
		}

		return (uintptr_t)"";
	case MENUOP_SET:
		// a row past the list's end (it got shorter under the menu) is none
		if (data->list.value < (uintptr_t)s_OrderCount && g_MpArenas[s_Order[data->list.value]].stagenum > 0) {
			g_MpSetup.stagenum = g_MpArenas[s_Order[data->list.value]].stagenum;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		// the chosen arena's row, and none (past any row) when it is another tab's
		data->list.value = 0x10000;

		for (s32 i = 0; i < s_OrderCount; i++) {
			if (g_MpArenas[s_Order[i]].stagenum == g_MpSetup.stagenum) {
				data->list.value = i;
				break;
			}
		}
		break;
	case MENUOP_GETOPTGROUPCOUNT:
		data->list.value = s_NumGroups;
		break;
	case MENUOP_GETOPTGROUPTEXT:
		if (data->list.value < (uintptr_t)s_NumGroups) {
			return (uintptr_t)s_DirName[s_GroupDir[data->list.value]];
		}

		return (uintptr_t)"";
	case MENUOP_GETGROUPSTARTINDEX:
		data->list.groupstartindex = data->list.value < (uintptr_t)s_NumGroups ? s_GroupStart[data->list.value] : 9999;
		break;
	}

	return 0;
}

/**
 * Find the Arena dialog's list (the first after the tabs) and its data in the
 * menu in front, to start it at a row of the tab now shown: the chosen
 * arena's, or the tab's first.
 */
static void arenaTabsPlaceList(struct menuitem *tabsitem)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];
	struct menudialog *dialog = menu->curdialog;
	struct menuitem *list = NULL;
	union handlerdata data;
	s32 col;

	if (!dialog || !dialog->definition || !dialog->definition->items) {
		return;
	}

	for (struct menuitem *item = dialog->definition->items; item->type != MENUITEMTYPE_END; item++) {
		if (item->type == MENUITEMTYPE_LIST && (!tabsitem || item > tabsitem)) {
			list = item;
			break;
		}
	}

	if (!list || !list->handler) {
		return;
	}

	for (col = 0; col < dialog->numcols; col++) {
		const s32 colindex = dialog->colstart + col;

		for (s32 row = 0; row < menu->cols[colindex].numrows; row++) {
			const s32 rowindex = menu->cols[colindex].rowstart + row;
			struct menuitemdata_list *listdata;
			s32 index;

			if (&dialog->definition->items[menu->rows[rowindex].itemindex] != list || menu->rows[rowindex].blockindex == -1) {
				continue;
			}

			listdata = (struct menuitemdata_list *)&menu->blocks[menu->rows[rowindex].blockindex];

			data.list.value = 0x10000;
			list->handler(MENUOP_GETSELECTEDINDEX, list, &data);
			index = data.list.value < 0x10000 ? (s32)data.list.value : 0;

			listdata->index = index;
			listdata->targetoffsety = menuitemListGetOffsetY(index, list);
			listdata->curoffsety = listdata->targetoffsety;
			return;
		}
	}
}

void arenaTabsOpen(void)
{
	const s32 player = arenaTabsPlayer();
	s32 key = -1;

	s_NumRows = -1;
	arenaTabsSync();

	// the tab of the arena chosen (Random is Perfect Dark's)
	for (s32 i = 0; i < s_NumRows; i++) {
		if (s_RowListed[i] && s_RowStage[i] == g_MpSetup.stagenum) {
			key = s_RowKey[i];
			break;
		}
	}

	if (key < 0 || arenaTabsIndexOfKey(key) < 0) {
		key = s_NumTabs > 0 ? s_Tabs[0].key : TABKEY_MAIN;
	}

	s_ShownKey[player] = key;

	if (arenaTabsActive()) {
		arenaTabsPlaceList(NULL);
	}
}

MenuItemHandlerResult arenaTabsMenuHandler(s32 operation, struct menuitem *item, union handlerdata *data)
{
	arenaTabsSync();

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !arenaTabsActive();
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = arenaTabsActive() ? s_NumTabs : 0;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->list.value < (uintptr_t)s_NumTabs) {
			return (uintptr_t)(data->list.unk04 ? s_Tabs[data->list.value].shortname : s_Tabs[data->list.value].name);
		}

		return (uintptr_t)"";
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = arenaTabsShown();
		break;
	case MENUOP_SET:
		if (data->list.value < (uintptr_t)s_NumTabs) {
			s_ShownKey[arenaTabsPlayer()] = s_Tabs[data->list.value].key;
			arenaTabsPlaceList(item);
		}
		break;
	}

	return 0;
}
