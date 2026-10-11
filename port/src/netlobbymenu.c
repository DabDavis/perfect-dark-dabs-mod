#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "game/menu.h"
#include "game/menugfx.h"
#include "game/game_006900.h"
#include "game/challenge.h"
#include "game/mplayer/setup.h"
#include "game/game_1531a0.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "lib/vi.h"
#include "game/lang.h"
#include "modloader.h"
#include "mod.h"
#include "modmode.h"
#include "fs.h"
#include "video.h"
#include "gexplusrom.h"
#include "net/net.h"
#include "net/netlobby.h"
#include "net/netproto.h"

/**
 * Online Game: the lobby's rooms on PD's own menu dialogs (the flow is
 * SOCOM's briefing room, ~/claude-socom/socom-client/frontend/localgame.go):
 *
 *   Combat Simulator > Online Game        the account, Browse Rooms, Create Room
 *     Briefing Room                       the room list: IN/MAX, ARENA, MODE,
 *                                         PING, PASS; a mark on a room this
 *                                         game cannot play, why on select;
 *                                         REFRESH, JOIN, CREATE
 *     Create Room                         name, password, max players, then
 *                                         the Combat Simulator's own Scenario,
 *                                         Options, Arena, Weapons, Limits,
 *                                         Handicaps, Simulants, Teams, Load/
 *                                         Save Settings; Room Rules
 *     Game Lobby                          the roster in team columns with
 *                                         ready marks, chat, the settings;
 *                                         CHANGE TEAM / READY / LEAVE; the
 *                                         host's LAUNCH (5 s, cancellable),
 *                                         KICK and SETTINGS
 *
 * Everything the network says comes from netlobby.c, ticked here while a
 * page is up and from the main loop besides; nothing on these pages waits.
 */

#define ROWH 11
#define LABEL_WIDTH 262  // a small-font label's line inside the dialog, in its own font's units (labelWidth; ~2.2 px a unit at 640x480, the line from x 32 to the edge at ~612); it does not widen the dialog

static char s_RoomPassword[NETLOBBY_MAXPASSWORD + 1];
static char s_JoinPassword[NETLOBBY_MAXPASSWORD + 1];
static char s_ChatLine[NETLOBBY_MAXCHATTEXT + 1];
static struct netlobbycreate s_Create = { "", "", 4 };
static s32 s_SelectedRoom = -1;
static char s_JoinId[9];
static s32 s_WaitingForRoom = 0; // a join or create is in flight from these pages
static char s_Status[320];
static char s_BriefingNote[160]; // why the focused room cannot be joined
static s32 s_HostTeamTouched = 0;  // the room's Teams page was opened (hostTeamSend)
static void hostTeamSend(void);

extern struct menudialogdef g_NetBriefingMenuDialog;
extern struct menudialogdef g_NetCreateMenuDialog;
extern struct menudialogdef g_NetRoomMenuDialog;
extern struct menudialogdef g_NetJoinPasswordMenuDialog;
extern struct menudialogdef g_NetKickMenuDialog;
extern struct menudialogdef g_NetRoomSettingsMenuDialog;
extern struct menudialogdef g_MpArenaMenuDialog;
extern struct menudialogdef g_MpLimitsMenuDialog;
extern struct menudialogdef g_MpSimulantsMenuDialog;

char *mpMenuTextArenaName(struct menuitem *item);
char *mpMenuTextScenarioShortName(struct menuitem *item);

static s32 isCurrent(struct menudialogdef *def)
{
	return g_Menus[g_MpPlayerNum].curdialog && g_Menus[g_MpPlayerNum].curdialog->definition == def;
}

// A cell, drawn at x, y (menu units from the row's top left), cut to maxchars
static Gfx *drawCellAt(Gfx *gdl, struct menuitemrenderdata *rd, s32 x, s32 y, const char *text, s32 maxchars, u32 colour)
{
	char buf[48];
	s32 tx = rd->x + x;
	s32 ty = rd->y + y;
	s32 n = strlen(text);

	if (n > maxchars) {
		n = maxchars;
	}

	if (n > (s32)sizeof(buf) - 1) {
		n = sizeof(buf) - 1;
	}

	memcpy(buf, text, n);
	buf[n] = '\0';

	return textRenderProjected(gdl, &tx, &ty, buf, g_CharsHandelGothicSm, g_FontHandelGothicSm, colour, viGetWidth(), viGetHeight(), 0, 0);
}

static s32 fontWidth(const char *text, s32 label)
{
	s32 w = 0;
	s32 h = 0;

	// a MENUITEMFLAG_SMALLFONT label draws in HandelGothicXs (menuitem.c),
	// the cells drawCellAt draws in HandelGothicSm
	if (label) {
		textMeasure(&h, &w, (char *)text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);
	} else {
		textMeasure(&h, &w, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
	}

	return w;
}

static s32 cellWidth(const char *text)
{
	return fontWidth(text, 0);
}

static s32 labelWidth(const char *text)
{
	return fontWidth(text, 1);
}

// text cut (in place) to width, ".." where it was cut
static void fitDots(char *text, s32 size, s32 width, s32 label)
{
	s32 n = strlen(text);

	if (fontWidth(text, label) <= width) {
		return;
	}

	while (n > 0) {
		text[--n] = '\0';

		while (n > 0 && text[n - 1] == ' ') {
			text[--n] = '\0';
		}

		if (n + 3 <= size) {
			memcpy(text + n, "..", 3);

			if (fontWidth(text, label) <= width) {
				return;
			}

			text[n] = '\0';
		}
	}
}

static void cellFitDots(char *text, s32 size, s32 width)
{
	fitDots(text, size, width, 0);
}

// a small-font label's text cut to width, in the font it draws in
static void labelFitDots(char *text, s32 size, s32 width)
{
	fitDots(text, size, width, 1);
}

static u32 headerColour(u32 colour)
{
	return 0xffc04000 | (colour & 0xff);
}

static u32 dimColour(u32 colour)
{
	return 0x80808000 | (colour & 0xff);
}

/*
 * Online Game
 */

extern struct menudialogdef g_GhostAccountMenuDialog;

static char *textAccount(struct menuitem *item)
{
	static char text[96];

	if (!netLobbyAvailable()) {
		// Sign In makes the account when the name is new: there is no
		// Create Account to find first (players could not tell they needed
		// one, 2026-10-08)
		snprintf(text, sizeof(text), "%s", "Sign In below - a new name makes your account.\n");
	} else if (netLobbySignedIn()) {
		snprintf(text, sizeof(text), "Signed in as %s\n", netLobbyAccount());
	} else if (netLobbyBusy()) {
		snprintf(text, sizeof(text), "Signing in as %s...\n", netLobbyAccount());
	} else {
		snprintf(text, sizeof(text), "Not signed in as %s\n", netLobbyAccount());
	}

	return text;
}

static char *textMessage(struct menuitem *item)
{
	if (netLobbyBusy()) {
		snprintf(s_Status, sizeof(s_Status), "%s", "Asking the lobby...\n");
	} else if (netLobbyMessage()[0]) {
		snprintf(s_Status, sizeof(s_Status), "%s\n", netLobbyMessage());
	} else {
		s_Status[0] = '\n';
		s_Status[1] = '\0';
	}

	return s_Status;
}

/**
 * Sign In: the account page (ghostmenu.c, shared with Ghost Trials), where a
 * name and PIN are typed and one Sign In signs in or, for a new name, makes
 * the account; it closes itself once that has worked. It used to open
 * Ghost Trials' list of remembered accounts ("No account - ghosts stay on
 * this machine", "< Empty >", and a list the cursor could not leave upwards)
 * with the page behind New Account on it. Signed in, the row is Change
 * Account and opens the same page. The lobby signs in with what the page
 * leaves in Mod.GhostUser/GhostPin as this page comes back (dialogOnline).
 */
static char *textSignIn(struct menuitem *item)
{
	return netLobbySignedIn() ? "Change Account...\n" : "Sign In...\n";
}

static MenuItemHandlerResult handlerSignIn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netLobbyClearMessage();
		menuPushDialog(&g_GhostAccountMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult handlerBrowse(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyAvailable();
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetBriefingMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult handlerCreateOpen(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyAvailable();
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetCreateMenuDialog);
	}

	return 0;
}

static MenuDialogHandlerResult dialogOnline(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		netLobbyClearMessage();
		netLobbyWarm();
		netLobbySignInAgain();
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		// Back from it as the Combat Simulator's root goes to the Perfect
		// Menu (menutick.c), as the Combat Simulator's own tick has it: on
		// the way here from the Perfect Menu, menutick.c zeroed it while the
		// menus were shut, and Back went into Perfect Dark's Combat
		// Simulator (F3 2026-10-09)
		g_Vars.mpsetupmenu = MPSETUPMENU_GENERAL;

		// as it opens, and as the account page closes over a new account
		netLobbySignIn();
		netLobbyTick();
	}

	return 0;
}

static struct menuitem s_OnlineItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textAccount, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textMessage, 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT, (uintptr_t)"Browse Rooms\n", 0, handlerBrowse },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT, (uintptr_t)"Create Room\n", 0, handlerCreateOpen },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textSignIn, 0, handlerSignIn },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetOnlineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Online Game",
	s_OnlineItems,
	dialogOnline,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The Combat Simulator's own row for it (setup.c), in the GoldenEye mode's
 * and a ROM hack's Combat Simulator only (protocol 13): a room made there
 * carries the mode in its RULES, and a joiner plays the arena from its own
 * conversion. In Perfect Dark's it is hidden: the main menu's row below is
 * one step nearer, and a second one here was a duplicate (the user,
 * 2026-10-07). Open wherever the build has the lobby: the page's Sign In
 * row is where an account is made or entered (it used to be greyed "sign
 * in first", which sent the player to Ghost Trials with no word of why).
 * Built or not is netLobbyBuilt()'s answer, not PD_GHOST_NET's: this file
 * does not include ghostnet.h, so the macro is never defined here and the
 * rows were greyed in every build (the user's report, 2026-10-07).
 */
char *netLobbyMenuTextOnline(struct menuitem *item)
{
	return "Online Game\n";
}

MenuItemHandlerResult netLobbyMenuHandlerOnline(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !g_GexPlusMode;
	}

	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyBuilt();
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetOnlineMenuDialog);
	}

	return 0;
}

void netLobbyMenuPushRoom(void);

/**
 * Online Game on the Perfect Menu (mainmenu.c): the lobby's pages as the
 * root of the Combat Simulator's menus, so a room is one step from the main
 * menu and Back comes back to it rather than into the Combat Simulator (the
 * user, 2026-10-07: nested there it "traps you inside combat simulator").
 * The match state is Perfect Dark's Combat Simulator's, set as its own row
 * sets it (menuhandlerMainMenuCombatSimulator); the GoldenEye mode's and a
 * ROM hack's Combat Simulator keep their own Online Game row for rooms in
 * their mode.
 */
MenuItemHandlerResult netLobbyMenuHandlerOnlineMain(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyBuilt();
	}

	if (operation == MENUOP_SET) {
		// already in a room (a campaign's host between missions): its Game
		// Lobby, over the Perfect Menu and with the mode as it is
		if (netLobbyInRoom()) {
			netLobbyMenuPushRoom();
			return 0;
		}

		g_GexPlusVariant = NULL;
		mpSetGexPlusMode(false);
		g_Vars.bondplayernum = 0;
		g_Vars.coopplayernum = -1;
		g_Vars.antiplayernum = -1;
		challengeDetermineUnlockedFeatures();
		g_Vars.mpsetupmenu = MPSETUPMENU_GENERAL;
		func0f0f820c(&g_NetOnlineMenuDialog, MENUROOT_MPSETUP);
		func0f0f8300();
	}

	return 0;
}

/*
 * Briefing Room
 */

static const char *roomMode(const struct netlobbyroomsum *r)
{
	return r->scenario[0] ? r->scenario : "-";
}

static void roomPing(const struct netlobbyroomsum *r, char *out, s32 size)
{
	s32 est = 0;
	const s32 ping = netLobbyRoomPingEx(r, &est);

	if (ping >= 0) {
		// "~": this machine's leg estimated from the list's HTTP round trip
		snprintf(out, size, "%s%d", est ? "~" : "", ping > 999 ? 999 : ping);
	} else {
		snprintf(out, size, "--");
	}
}

/**
 * A room is two lines: its name across the whole row with its host at the
 * right, then IN, ARENA, MODE, PING and PASS under it, each column as wide
 * as its widest cell (to a cap) and laid out from the left. One line had
 * room for 11 characters of a name that may have 32 ("Stock Gam"): the
 * dialog is about 270 units wide at any window size, and the five columns
 * need 130 of them. A cell still too wide is cut with "..".
 */
#define ROOMROWH        22
#define ROOMLIST_WIDTH  266 // what shows of the list's 304 inside the dialog
#define ROOMCOL_IN      0
#define ROOMCOL_ARENA   1
#define ROOMCOL_MODE    2
#define ROOMCOL_PING    3
#define ROOMCOL_PASS    4
#define ROOMCOL_COUNT   5
#define ROOMCOL_GAP     10

struct roomcol {
	s32 x;
	s32 w;
};

static const char *s_RoomColHeads[ROOMCOL_COUNT] = { "IN", "ARENA", "MODE", "PING", "PASS" };
static const s32 s_RoomColCaps[ROOMCOL_COUNT] = { 28, 76, 64, 24, 24 };
static struct roomcol s_RoomCols[ROOMCOL_COUNT];

static void roomCell(const struct netlobbyroomsum *r, s32 col, char *out, s32 size)
{
	switch (col) {
	case ROOMCOL_IN:
		snprintf(out, size, "%d/%d", r->humans, r->maxhumans);
		break;
	case ROOMCOL_ARENA:
		snprintf(out, size, "%s", r->stage[0] ? r->stage : "-");
		break;
	case ROOMCOL_MODE:
		snprintf(out, size, "%s", roomMode(r));
		break;
	case ROOMCOL_PING:
		roomPing(r, out, size);
		break;
	default:
		snprintf(out, size, "%s", r->locked ? "YES" : "-");
		break;
	}
}

static void roomColumns(struct roomcol *cols)
{
	char buf[48];
	s32 x = 12;
	s32 c;
	s32 i;

	for (c = 0; c < ROOMCOL_COUNT; c++) {
		s32 w = cellWidth(s_RoomColHeads[c]);

		for (i = 0; i < netLobbyNumRooms(); i++) {
			s32 cw;

			roomCell(netLobbyRoomAt(i), c, buf, sizeof(buf));
			cw = cellWidth(buf);

			if (cw > w) {
				w = cw;
			}
		}

		if (w > s_RoomColCaps[c]) {
			w = s_RoomColCaps[c];
		}

		if (x + w > ROOMLIST_WIDTH) {
			w = ROOMLIST_WIDTH - x;
		}

		cols[c].x = x;
		cols[c].w = w;
		x += w + ROOMCOL_GAP;
	}
}


/**
 * What a room plays with that this game must have its own copy of
 * (netcontent.c): the host's mod, or the GoldenEye ROM hack mode, and
 * whether it is here. One status line: about 42 characters.
 */
static void roomContentNote(const struct netlobbyroomsum *r, char *out, s32 size)
{
	char name[48];

	if (r->mod[0]) {
		const char *loaded = modListGetLoadedName();
		const s32 index = modListIndexOf(r->mod);
		const char *state;

		// protocol 25: entered on joining, from this machine's copy or
		// the host's, served into memory (netcontent.c)
		if (loaded && strcasecmp(loaded, r->mod) == 0) {
			state = "playing here";
		} else if (modListIsFromArgs()) {
			state = "start without --moddir";
		} else if (index >= 0 || fsMemDirFind(r->mod) >= 0) {
			state = "entered on join";
		} else {
			state = "served on join";
		}

		snprintf(name, sizeof(name), "%s", modModeDisplayNameOf(r->mod));
		labelFitDots(name, sizeof(name), 110);
		snprintf(out, size, "Mod %s: %s", name, state);
		return;
	}

	{
		// a conversion's folder is listed among the mods (maps-only)
		const char *dir = gexPlusRomDirOfTag(r->ge);
		const s32 have = dir && modListIndexOf(dir) >= 0;

		snprintf(out, size, "%s: %s", dir ? dir : r->ge, have ? "converted here" : "NOT CONVERTED HERE (added-content/)");
	}
}
static MenuItemHandlerResult handlerRoomList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menuitemrenderdata *rd;
	const struct netlobbyroomsum *r;
	Gfx *gdl;
	char buf[48];
	u32 colour;
	s32 index;
	s32 c;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = netLobbyNumRooms() + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)"";
	case MENUOP_GETOPTIONHEIGHT:
		data->list.value = ROOMROWH;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	case MENUOP_GETOPTGROUPCOUNT:
		data->list.value = 0;
		break;
	case MENUOP_GETOPTGROUPTEXT:
		return 0;
	case MENUOP_GETGROUPSTARTINDEX:
		data->list.groupstartindex = 0;
		break;
	case MENUOP_LISTITEMFOCUS:
		s_SelectedRoom = (s32)data->list.value - 1;
		r = netLobbyRoomAt(s_SelectedRoom);
		netLobbyClearMessage();
		if (r && r->compat != NETLOBBY_COMPAT_OK) {
			snprintf(s_BriefingNote, sizeof(s_BriefingNote), "%s", netLobbyCompatText(r->compat));
		} else if (r && (r->mod[0] || r->ge[0])) {
			roomContentNote(r, s_BriefingNote, sizeof(s_BriefingNote));
		} else if (r) {
			// the whole name, which the GAME column may have cut
			// (a label does not widen the dialog: name and host cut to fit)
			char name[64];
			char host[48];

			snprintf(host, sizeof(host), "%s", r->host);
			labelFitDots(host, sizeof(host), 96);
			snprintf(name, sizeof(name), "%s", r->name);
			labelFitDots(name, sizeof(name), LABEL_WIDTH - labelWidth(" ()") - labelWidth(host));
			snprintf(s_BriefingNote, sizeof(s_BriefingNote), "%s (%s)", name, host);
		} else {
			s_BriefingNote[0] = '\0';
		}
		break;
	case MENUOP_SET:
		s_SelectedRoom = (s32)data->list.value - 1;
		r = netLobbyRoomAt(s_SelectedRoom);

		if (r == NULL) {
			netLobbyRefresh();
		} else if (r->compat != NETLOBBY_COMPAT_OK) {
			// the mark's reason, on the status line
			snprintf(s_BriefingNote, sizeof(s_BriefingNote), "%s", netLobbyCompatText(r->compat));
			menuPlaySound(MENUSOUND_ERROR);
		} else if (r->locked) {
			snprintf(s_JoinId, sizeof(s_JoinId), "%s", r->id);
			menuPushDialog(&g_NetJoinPasswordMenuDialog);
		} else {
			snprintf(s_JoinId, sizeof(s_JoinId), "%s", r->id);
			s_WaitingForRoom = 1;
			netLobbyJoin(r->id, "");
		}
		break;
	case MENUOP_RENDER:
		gdl = data->type19.gdl;
		rd = data->type19.renderdata2;
		index = (s32)data->type19.unk04;
		colour = rd->colour;
		gdl = text0f153628(gdl);

		// laid out on every row drawn: the header row may be scrolled off
		roomColumns(s_RoomCols);

		if (index == 0) {
			colour = headerColour(colour);
			gdl = drawCellAt(gdl, rd, 4, 1, "GAME", 20, colour);
			gdl = drawCellAt(gdl, rd, ROOMLIST_WIDTH - cellWidth("HOST"), 1, "HOST", 20, colour);

			for (c = 0; c < ROOMCOL_COUNT; c++) {
				gdl = drawCellAt(gdl, rd, s_RoomCols[c].x, 12, s_RoomColHeads[c], 20, colour);
			}
		} else if ((r = netLobbyRoomAt(index - 1)) != NULL) {
			char host[NETLOBBY_MAXUSER + 4];
			s32 hostw;

			if (r->compat != NETLOBBY_COMPAT_OK) {
				colour = dimColour(colour);
			}

			snprintf(host, sizeof(host), "%s", r->host);
			cellFitDots(host, sizeof(host), 70);
			hostw = cellWidth(host);
			snprintf(buf, sizeof(buf), "%s%s", r->compat != NETLOBBY_COMPAT_OK ? "! " : "", r->name);
			cellFitDots(buf, sizeof(buf), ROOMLIST_WIDTH - 4 - hostw - 8);
			gdl = drawCellAt(gdl, rd, 4, 1, buf, sizeof(buf) - 1, colour);
			gdl = drawCellAt(gdl, rd, ROOMLIST_WIDTH - hostw, 1, host, sizeof(host) - 1, dimColour(colour));

			for (c = 0; c < ROOMCOL_COUNT; c++) {
				roomCell(r, c, buf, sizeof(buf));
				cellFitDots(buf, sizeof(buf), s_RoomCols[c].w);
				gdl = drawCellAt(gdl, rd, s_RoomCols[c].x, 12, buf, sizeof(buf) - 1, colour);
			}
		}

		gdl = text0f153780(gdl);
		return (uintptr_t)gdl;
	}

	return 0;
}

static char *textBriefingStatus(struct menuitem *item)
{
	if (netLobbyBusy()) {
		snprintf(s_Status, sizeof(s_Status), "%s", "Asking the lobby...\n");
	} else if (netLobbyMessage()[0]) {
		snprintf(s_Status, sizeof(s_Status), "%s\n", netLobbyMessage());
	} else if (s_BriefingNote[0]) {
		snprintf(s_Status, sizeof(s_Status), "%s\n", s_BriefingNote);
	} else {
		snprintf(s_Status, sizeof(s_Status), "Rooms: %d. Pick one to join. ! = can't join.\n", netLobbyNumRooms());
	}

	return s_Status;
}

static MenuItemHandlerResult handlerRefresh(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return netLobbyBusy();
	}

	if (operation == MENUOP_SET) {
		s_BriefingNote[0] = '\0';
		netLobbyClearMessage();
		netLobbyRefresh();
	}

	return 0;
}

static const struct netlobbyroomsum *joinTarget(void)
{
	const struct netlobbyroomsum *r = netLobbyRoomAt(s_SelectedRoom);
	s32 i;

	// no row picked yet: the first room this game can join
	for (i = 0; r == NULL && i < netLobbyNumRooms(); i++) {
		if (netLobbyRoomAt(i)->compat == NETLOBBY_COMPAT_OK) {
			r = netLobbyRoomAt(i);
		}
	}

	return r;
}

static MenuItemHandlerResult handlerJoin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct netlobbyroomsum *r = joinTarget();

	if (operation == MENUOP_CHECKDISABLED) {
		return netLobbyBusy() || r == NULL || r->compat != NETLOBBY_COMPAT_OK;
	}

	if (operation == MENUOP_SET && r) {
		snprintf(s_JoinId, sizeof(s_JoinId), "%s", r->id);

		if (r->locked) {
			menuPushDialog(&g_NetJoinPasswordMenuDialog);
		} else {
			s_WaitingForRoom = 1;
			netLobbyJoin(r->id, "");
		}
	}

	return 0;
}

/**
 * A plain list scrolls its focused row to the middle of its view, so with
 * the first row focused half the view is empty above it. Held like a
 * dropdown's: the first row at the top, the last at the bottom.
 */
static void listClamp(struct menudialog *dialog, struct menuitem *item, s32 rowh)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];
	union handlerdata hd;
	s32 col;
	s32 j;

	for (col = dialog->colstart; col < dialog->colstart + dialog->numcols; col++) {
		for (j = 0; j < menu->cols[col].numrows; j++) {
			const s32 row = menu->cols[col].rowstart + j;

			if (&dialog->definition->items[menu->rows[row].itemindex] == item && menu->rows[row].blockindex != -1) {
				union menuitemdata *d = (union menuitemdata *)&menu->blocks[menu->rows[row].blockindex];
				const s32 view = d->list.viewheight > 0 ? d->list.viewheight : item->param3;
				const s32 min = (view / 2) / rowh * rowh;
				s32 max;

				item->handler(MENUOP_GETOPTIONCOUNT, item, &hd);
				max = (s32)hd.list.value * rowh - view + min;

				if (max < min) {
					max = min;
				}

				if (d->list.targetoffsety < min) {
					d->list.targetoffsety = min;
				} else if (d->list.targetoffsety > max) {
					d->list.targetoffsety = max;
				}

				return;
			}
		}
	}
}

static MenuDialogHandlerResult dialogBriefing(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		s_BriefingNote[0] = '\0';
		s_SelectedRoom = -1;
		netLobbyClearMessage();
		netLobbyRefresh();
	}

	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		listClamp(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[5], ROOMROWH);
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		if (s_WaitingForRoom && netLobbyInRoom()) {
			s_WaitingForRoom = 0;
			menuPushDialog(&g_NetRoomMenuDialog);
		} else if (s_WaitingForRoom && !netLobbyBusy()) {
			s_WaitingForRoom = 0;
		}
	}

	// closed with a create or join still in flight: called off, so no room
	// is held (and no game socket left open) behind the menus
	if (operation == MENUOP_CLOSE && s_WaitingForRoom) {
		s_WaitingForRoom = 0;
		netLobbyCancelPending();
	}

	// closed with no room made (Back): this machine's own setup and
	// settings again
	if (operation == MENUOP_CLOSE && !netLobbyInRoom()) {
		netLobbyRoomRulesCancel();
	}

	return 0;
}

static struct menuitem s_BriefingItems[] = {
	// the buttons above the list: a list keeps the cursor once it has it
	// (its rows wrap), so below it they could not be reached by keys; a
	// room's row joins it, and Back is the menus' own back
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textBriefingStatus, 0, NULL },
	// what the PING column measures (netLobbyRoomPing): no packet goes to a
	// host before joining, which keeps its address out of the public list
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"PING: ms, you to lobby + lobby to host (~ by HTTP)\n", 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Refresh\n", 0, handlerRefresh },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Join\n", 0, handlerJoin },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Create\n", 0, handlerCreateOpen },
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_CUSTOMRENDER | MENUITEMFLAG_LIST_LEAVEATENDS, 304, ROOMROWH * 5, handlerRoomList },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetBriefingMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Briefing Room",
	s_BriefingItems,
	dialogBriefing,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// A locked room's password, then the join
static MenuItemHandlerResult handlerJoinPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", s_JoinPassword);
		break;
	case MENUOP_SETTEXT:
		snprintf(s_JoinPassword, sizeof(s_JoinPassword), "%s", data->keyboard.string);
		break;
	case MENUOP_SET:
		s_WaitingForRoom = 1;
		netLobbyJoin(s_JoinId, s_JoinPassword);
		break;
	}

	return 0;
}

static struct menuitem s_JoinPasswordItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"This room has a password.\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, NETLOBBY_MAXPASSWORD, 0, 0, 0, handlerJoinPassword },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetJoinPasswordMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Room Password",
	s_JoinPasswordItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/*
 * Create Room
 */

static MenuItemHandlerResult handlerRoomName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", s_Create.name);
		break;
	case MENUOP_SETTEXT:
		snprintf(s_Create.name, sizeof(s_Create.name), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerRoomPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		snprintf(data->keyboard.string, MPSETUP_MAXNAME + 1, "%s", s_RoomPassword);
		break;
	case MENUOP_SETTEXT:
		snprintf(s_RoomPassword, sizeof(s_RoomPassword), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static struct menuitem s_RoomNameItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"The name in the Briefing Room's list.\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, MPSETUP_MAXNAME, 0, 0, 0, handlerRoomName },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_RoomNameDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Room Name", s_RoomNameItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static struct menuitem s_RoomPasswordItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"Up to 16 characters.\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, NETLOBBY_MAXPASSWORD, 0, 0, 0, handlerRoomPassword },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_RoomPasswordDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Room Password", s_RoomPasswordItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static char *textCreateName(struct menuitem *item)
{
	static char text[64];

	snprintf(text, sizeof(text), "Name: %s\n", s_Create.name);

	return text;
}

static char *textCreatePassword(struct menuitem *item)
{
	return s_RoomPassword[0] ? "Password: set (select to clear)\n" : "Password: none\n";
}

static MenuItemHandlerResult handlerCreateName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&s_RoomNameDialog);
	}

	return 0;
}

static MenuItemHandlerResult handlerCreatePassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (s_RoomPassword[0]) {
			s_RoomPassword[0] = '\0';
		} else {
			menuPushDialog(&s_RoomPasswordDialog);
		}
	}

	return 0;
}

static MenuItemHandlerResult handlerMaxPlayers(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[8];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = MAX_PLAYERS - 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		snprintf(text, sizeof(text), "%d", (s32)data->dropdown.value + 2);
		return (intptr_t)text;
	case MENUOP_SET:
		s_Create.maxhumans = data->dropdown.value + 2;
		break;
	case MENUOP_GETSELECTEDINDEX:
		if (s_Create.maxhumans > MAX_PLAYERS) {
			s_Create.maxhumans = MAX_PLAYERS;
		}

		data->dropdown.value = s_Create.maxhumans - 2;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerCreateGo(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return netLobbyBusy();
	}

	if (operation == MENUOP_SET) {
		snprintf(s_Create.password, sizeof(s_Create.password), "%s", s_RoomPassword);
		s_WaitingForRoom = 1;
		netLobbyClearMessage();
		hostTeamSend(); // kept until the room exists
		netLobbyCreate(&s_Create);
	}

	return 0;
}

static char *textArena(struct menuitem *item)
{
	return mpMenuTextArenaName(item);
}

static char *textScenario(struct menuitem *item)
{
	return mpMenuTextScenarioShortName(item);
}

static MenuDialogHandlerResult dialogCreate(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		netLobbyClearMessage();

		// the room's own Room Rules and Combat Simulator setup from here,
		// this machine's own kept aside (owner, F3 20261010-053136)
		netLobbyRoomRulesBegin();

		if (!s_Create.name[0]) {
			snprintf(s_Create.name, sizeof(s_Create.name), "%.12s's Room", netLobbyAccount());
		}

		// the room's host plays: player 1 of the setup
		g_MpSetup.chrslots |= 1;

		// a new room: everyone at 100%, the host's team the room's own
		// until its Teams page says otherwise
		netLobbyHandicapsReset();
		s_HostTeamTouched = 0;
	}

	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		listClamp(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[5], ROOMROWH);
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		if (s_WaitingForRoom && netLobbyInRoom()) {
			s_WaitingForRoom = 0;
			func0f0f3704(&g_NetRoomMenuDialog); // the room in place of this page
		} else if (s_WaitingForRoom && !netLobbyBusy()) {
			s_WaitingForRoom = 0;
		}
	}

	// closed with a create or join still in flight: called off, so no room
	// is held (and no game socket left open) behind the menus
	if (operation == MENUOP_CLOSE && s_WaitingForRoom) {
		s_WaitingForRoom = 0;
		netLobbyCancelPending();
	}

	return 0;
}

/*
 * The room's game: a Combat Simulator match (the Simulator's own setup
 * pages, as Advanced Setup lists them) or a co-op mission (netcoop.c:
 * mission, difficulty, radar, friendly fire). Each set hides while the
 * other is chosen; an item that opens a dialog cannot hide itself (menu.c
 * asks only a plain item's handler), so the setup pages open from handlers.
 */
/**
 * The room's game: a Combat Simulator match, one co-op mission, or a
 * campaign of a set's missions (Perfect Dark's, GoldenEye's, a ROM hack's,
 * as converted here), which the host plays from its own menus. The
 * GoldenEye mode's rooms are matches on its arenas.
 */
static s32 gameOptions(void)
{
	char tag[16];
	s32 n;

	if (g_GexPlusMode) {
		return 1;
	}

	for (n = 0; netCoopGameTag(n, tag, sizeof(tag)); n++) {
	}

	return 2 + n;
}

static MenuItemHandlerResult handlerGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[48];
	char tag[16];
	s32 n;

	if (g_GexPlusMode) {
		g_NetCoopSetup.on = 0;
		g_NetCoopSetup.campaign = 0;
	}

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = gameOptions();
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value == 0) {
			return (intptr_t)"Combat Simulator";
		}

		if (data->dropdown.value == 1) {
			return (intptr_t)"Co-op Mission";
		}

		if (!netCoopGameTag(data->dropdown.value - 2, tag, sizeof(tag))) {
			tag[0] = '\0';
		}

		snprintf(text, sizeof(text), "%s Campaign", netCoopGameName(tag));
		return (intptr_t)text;
	case MENUOP_SET:
		g_NetCoopSetup.on = data->dropdown.value >= 1 && !g_GexPlusMode;
		g_NetCoopSetup.campaign = data->dropdown.value >= 2 && !g_GexPlusMode;

		if (g_NetCoopSetup.campaign) {
			if (!netCoopGameTag(data->dropdown.value - 2, tag, sizeof(tag))) {
				tag[0] = '\0';
			}

			snprintf(g_NetCoopSetup.game, sizeof(g_NetCoopSetup.game), "%s", tag);
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;

		if (g_NetCoopSetup.on && g_NetCoopSetup.campaign) {
			for (n = 0; netCoopGameTag(n, tag, sizeof(tag)); n++) {
				if (strcasecmp(tag, g_NetCoopSetup.game) == 0) {
					data->dropdown.value = 2 + n;
				}
			}
		} else if (g_NetCoopSetup.on) {
			data->dropdown.value = 1;
		}
		break;
	}

	return 0;
}

/**
 * Missions: whose - Perfect Dark's, GoldenEye's, a ROM hack's - among the
 * sets converted here (protocol 14, netcoop.c). A joiner plays the mission
 * from its own conversion, as it does an arena.
 */
static MenuItemHandlerResult handlerMissionSet(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[40];
	char tag[16];
	s32 n;

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on || g_NetCoopSetup.campaign;
	case MENUOP_GETOPTIONCOUNT:
		for (n = 0; netCoopGameTag(n, tag, sizeof(tag)); n++) {
		}

		data->dropdown.value = n;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (!netCoopGameTag(data->dropdown.value, tag, sizeof(tag))) {
			tag[0] = '\0';
		}

		snprintf(text, sizeof(text), "%s", netCoopGameName(tag));
		return (intptr_t)text;
	case MENUOP_SET:
		if (netCoopGameTag(data->dropdown.value, tag, sizeof(tag)) && strcasecmp(tag, g_NetCoopSetup.game) != 0) {
			snprintf(g_NetCoopSetup.game, sizeof(g_NetCoopSetup.game), "%s", tag);
			g_NetCoopSetup.stageindex = 0;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;

		for (n = 0; netCoopGameTag(n, tag, sizeof(tag)); n++) {
			if (strcasecmp(tag, g_NetCoopSetup.game) == 0) {
				data->dropdown.value = n;
			}
		}
		break;
	}

	return 0;
}

// a conversion's mission has no number in its own set that this machine
// lacks: the list is the set's missions here, each by its number
static s32 missionAt(s32 n)
{
	s32 k = 0;
	s32 i;

	if (!g_NetCoopSetup.game[0]) {
		return n;
	}

	for (i = 0; i < MODLOADER_MAX_MISSIONS; i++) {
		if (strcmp(netCoopMissionNameOf(g_NetCoopSetup.game, i), "?") != 0 && k++ == n) {
			return i;
		}
	}

	return 0;
}

static MenuItemHandlerResult handlerMission(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[40];
	s32 i;
	s32 k;

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on || g_NetCoopSetup.campaign;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = netCoopGameMissions(g_NetCoopSetup.game);
		break;
	case MENUOP_GETOPTIONTEXT:
		if (!g_NetCoopSetup.game[0]) {
			return (intptr_t)langGet(g_SoloStages[data->dropdown.value <= SOLOSTAGEINDEX_WAR ? data->dropdown.value : 0].name3);
		}

		snprintf(text, sizeof(text), "%s\n", netCoopMissionNameOf(g_NetCoopSetup.game, missionAt(data->dropdown.value)));
		return (intptr_t)text;
	case MENUOP_SET:
		g_NetCoopSetup.stageindex = missionAt(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;

		for (i = 0, k = 0; i < MODLOADER_MAX_MISSIONS; i++) {
			if (!g_NetCoopSetup.game[0]) {
				data->dropdown.value = g_NetCoopSetup.stageindex;
				break;
			}

			if (strcmp(netCoopMissionNameOf(g_NetCoopSetup.game, i), "?") != 0) {
				if (i == g_NetCoopSetup.stageindex) {
					data->dropdown.value = k;
				}

				k++;
			}
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const u16 names[3] = { L_OPTIONS_251, L_OPTIONS_252, L_OPTIONS_253 }; // "Agent", "Special Agent", "Perfect Agent"

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on || g_NetCoopSetup.campaign;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 3;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)langGet(names[data->dropdown.value < 3 ? data->dropdown.value : 0]);
	case MENUOP_SET:
		g_NetCoopSetup.difficulty = data->dropdown.value < 3 ? data->dropdown.value : DIFF_A;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_NetCoopSetup.difficulty;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerCoopRadar(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on;
	case MENUOP_GET:
		return g_NetCoopSetup.radar;
	case MENUOP_SET:
		g_NetCoopSetup.radar = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerCoopFriendlyFire(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on;
	case MENUOP_GET:
		return g_NetCoopSetup.friendlyfire;
	case MENUOP_SET:
		g_NetCoopSetup.friendlyfire = data->checkbox.value ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerSetupPage(s32 operation, struct menuitem *item, struct menudialogdef *page)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return g_NetCoopSetup.on;
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(page);
	}

	return 0;
}

static MenuItemHandlerResult handlerSetupScenario(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpScenarioMenuDialog);
}

static MenuItemHandlerResult handlerSetupArena(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpArenaMenuDialog);
}

static MenuItemHandlerResult handlerSetupWeapons(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpWeaponsMenuDialog);
}

static MenuItemHandlerResult handlerSetupLimits(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpLimitsMenuDialog);
}

static MenuItemHandlerResult handlerSetupSimulants(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpSimulantsMenuDialog);
}

/*
 * The rest of the Combat Simulator's Game Setup (F3 20261009-071718): the
 * scenario's Options, Handicaps, Teams, Load and Save Settings. Every one
 * writes what RULES already carries (g_MpSetup.options, the scenario's save
 * bits, each slot's team and handicap, the sims' teams), so nothing new goes
 * on the wire. Left out: the Player pages (each player's own, set in their
 * own game) and the Soundtrack and Team Names (the Combat Simulator's Stuff,
 * not its setup).
 */
static MenuItemHandlerResult handlerSetupOptions(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return g_NetCoopSetup.on;
	}

	return menuhandlerMpOpenOptions(operation, item, data);
}

/**
 * The stock Teams page: the simulants' teams, Teams Enabled, Auto Team, and
 * the host's own row, which is its team in the room (the launch puts each
 * member's room team on its slot): the row starts at it, and a change goes
 * to the room when the page's dialog closes. The other members pick theirs
 * with the Game Lobby's Change Team.
 */
static MenuItemHandlerResult handlerSetupTeams(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (netLobbyInRoom() && netLobbyMyTeam() >= 0) {
			g_PlayerConfigsArray[0].base.team = netLobbyMyTeam();
		}

		s_HostTeamTouched = 1;
	}

	return handlerSetupPage(operation, item, &g_MpTeamsMenuDialog);
}

static void hostTeamSend(void)
{
	if (s_HostTeamTouched) {
		netLobbySetHostTeam(g_PlayerConfigsArray[0].base.team);
	}
}

static MenuItemHandlerResult handlerSetupLoad(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &g_MpLoadSettingsMenuDialog);
}

static MenuItemHandlerResult handlerSetupSave(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return g_NetCoopSetup.on;
	}

	return menuhandlerMpSaveSettings(operation, item, data);
}

/**
 * Handicaps over the room's players rather than the four local ones: the
 * host's own (its profile's, as offline), then each other member who plays
 * (by account, netLobbyHandicapOf; the host's slot for it at the match
 * start). Before the room exists (Create Room) only the host's row.
 */
#define HANDICAP_ROWS MAX_PLAYERS

static const struct netlobbymember *handicapMember(s32 row)
{
	const struct netlobbyroom *room;
	s32 i;

	if (row < 1 || !netLobbyInRoom()) {
		return NULL;
	}

	room = netLobbyGetRoom();

	for (i = 0; i < room->nmembers; i++) {
		const struct netlobbymember *m = &room->members[i];

		if (!m->host && !m->spectator && --row == 0) {
			return m;
		}
	}

	return NULL;
}

static char *textHandicapName(struct menuitem *item)
{
	static char name[NETLOBBY_MAXUSER + 2];
	const struct netlobbymember *m = handicapMember(item->param);

	if (item->param == 0) {
		return netLobbyInRoom() && netLobbyAccount()[0] ? (char *)netLobbyAccount() : g_PlayerConfigsArray[0].base.name;
	}

	snprintf(name, sizeof(name), "%s", m ? m->user : "");
	return name;
}

static MenuItemHandlerResult handlerHandicapRow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct netlobbymember *m = handicapMember(item->param);
	s32 value;

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return item->param != 0 && !m;
	case MENUOP_GETSLIDER:
		data->slider.value = item->param == 0 ? g_PlayerConfigsArray[0].handicap : m ? netLobbyHandicapOf(m->user) : 0x80;
		break;
	case MENUOP_SET:
		if (item->param == 0) {
			g_PlayerConfigsArray[0].handicap = (u16)data->slider.value;
		} else if (m) {
			netLobbyHandicapSet(m->user, data->slider.value);
		}
		break;
	case MENUOP_GETSLIDERLABEL:
		value = item->param == 0 ? g_PlayerConfigsArray[0].handicap : m ? netLobbyHandicapOf(m->user) : 0x80;
		sprintf(data->slider.label, "%.00f%%\n", mpHandicapToDamageScale(value) * 100);
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerHandicapDefaults(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_PlayerConfigsArray[0].handicap = 0x80;
		netLobbyHandicapsReset();
	}

	return 0;
}

#define HANDICAP_ROW(n) { MENUITEMTYPE_SLIDER, n, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&textHandicapName, 0xff, handlerHandicapRow }

static struct menuitem s_HandicapItems[] = {
	HANDICAP_ROW(0), HANDICAP_ROW(1), HANDICAP_ROW(2), HANDICAP_ROW(3), HANDICAP_ROW(4), HANDICAP_ROW(5),
	HANDICAP_ROW(6), HANDICAP_ROW(7), HANDICAP_ROW(8), HANDICAP_ROW(9), HANDICAP_ROW(10), HANDICAP_ROW(11),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_110, 0, handlerHandicapDefaults }, // "Restore Defaults"
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, L_MPMENU_111, 0, NULL }, // "Back"
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_NetHandicapsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, L_MPWEAPONS_184, s_HandicapItems, NULL, 0, NULL, // "Player Handicaps"
};

static MenuItemHandlerResult handlerSetupHandicaps(s32 operation, struct menuitem *item, union handlerdata *data)
{
	return handlerSetupPage(operation, item, &s_NetHandicapsMenuDialog);
}


// Room Rules: the Dab's Mod rules the host decides for the match (optionsmenu.c)
extern struct menudialogdef g_NetRoomRulesMenuDialog;

static MenuItemHandlerResult handlerSetupRules(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetRoomRulesMenuDialog);
	}

	return 0;
}

#define NETLOBBY_SETUP_ITEMS \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Game", 0, handlerGame }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_019, (uintptr_t)&textScenario, handlerSetupScenario }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_021, 0, handlerSetupOptions }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_020, (uintptr_t)&textArena, handlerSetupArena }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_023, 0, handlerSetupWeapons }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_024, 0, handlerSetupLimits }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPWEAPONS_184, 0, handlerSetupHandicaps }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_025, 0, handlerSetupSimulants }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_022, 0, handlerSetupTeams }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_018, 0, handlerSetupLoad }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_026, 0, handlerSetupSave }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Missions", 0, handlerMissionSet }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Mission", 0, handlerMission }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, handlerDifficulty }, \
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Radar", 0, handlerCoopRadar }, \
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Friendly Fire", 0, handlerCoopFriendlyFire }, \
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Room Rules\n", 0, handlerSetupRules }

static struct menuitem s_CreateItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textMessage, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textCreateName, 0, handlerCreateName },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textCreatePassword, 0, handlerCreatePassword },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Max Players", 0, handlerMaxPlayers },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	NETLOBBY_SETUP_ITEMS,
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_BIGFONT, (uintptr_t)"Create\n", 0, handlerCreateGo },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetCreateMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Create Room",
	s_CreateItems,
	dialogCreate,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/*
 * Game Lobby
 *
 * Four windows in two columns, after the owner's mock-up (2026-10-11): on
 * the left the room - its name and count, its settings, the roster in team
 * columns - over the mods it plays with; on the right its chat, the status
 * line at the chat's foot, over the buttons. The buttons are the dialog, so
 * the pad, the keys and the mouse work them as on any page; the other three
 * windows are drawn around it in its own colours, after it
 * (netLobbyMenuRenderPanels(), from menuRenderDialog()), and are never
 * focused.
 *
 * The menu is 320x220 units drawn at that shape in the middle of the
 * window. The left column is held to the window's left edge and the right
 * column, the dialog with it, to its right (G_ASPECT_LEFT_EXT and
 * RIGHT_EXT, as Ghost Trials' plaques), each the wider by the screen beside
 * the frame, so on a widescreen display the windows fill its width. The
 * frame is wider than 4:3, so on a 4:3 window that "pillar" is negative
 * and the columns are the narrower (lobbyPillar). Neither column's
 * coordinates leave 0-320 in its own side's space, which is as far as text
 * goes (text0f15568c() drops a glyph past viGetWidth()). WIDE slows the
 * offset past 16:9, as for the plaques: an ultrawide display has the
 * windows in from its edges.
 */

#define LOBBY_MARGIN  8  // from the screen's sides
#define LOBBY_GAP     6  // between the columns
#define LOBBY_VGAP    4  // between a column's windows
#define LOBBY_INSET   4  // a window's text from its sides
#define LOBBY_XSROW   8  // a line of the small font (HandelGothicXs)
#define LOBBY_SMROW   10 // a line of HandelGothicSm
#define LOBBY_MDROW   14 // a line of HandelGothicMd
#define LOBBY_MODROWS 4
#define LOBBY_LEFTPCT 60 // the left column's share of the width

#define LOBBYFONT_XS 0
#define LOBBYFONT_SM 1
#define LOBBYFONT_MD 2

struct lobbylayout {
	s32 pillar;    // the screen past the menu's 320-unit frame on each side, in menu units (negative at 4:3)
	s32 top;
	s32 bottom;
	s32 lx1;       // the left column, in its (left-held) space
	s32 lx2;
	s32 rx1;       // the right column, in its (right-held) space
	s32 rx2;
	s32 roomy2;    // the room window's foot
	s32 modsy1;    // the mods window's top
	s32 buttonsy;  // the dialog's y: its box starts LINEHEIGHT below (no title bar)
	s32 buttonsh;
};

/**
 * How far G_ASPECT_RIGHT_EXT moves what is drawn under it, in menu units
 * (G_ASPECT_LEFT_EXT as far the other way): the screen either side of the
 * menu's frame, capped at 16:9 as G_ASPECT_WIDE_EXT caps it (ghostmenu.c's
 * plaque offset, the same sum). The frame is the menu's 320x220, which is
 * wider than 4:3: on a 4:3 window this is negative (-13), the frame's edges
 * off the screen, and the alignment moves a side in.
 */
static s32 lobbyPillar(void)
{
	const f32 window = videoGetAspect();
	const f32 native = (f32)videoGetNativeWidth() / (f32)videoGetNativeHeight();
	f32 ofs = window / native - 1.0f;

	if (window > 16.0f / 9.0f) {
		ofs *= (16.0f / 9.0f) / window;
	}

	return (s32)(ofs * (SCREEN_WIDTH_LO / 2));
}

/**
 * Where everything goes: LOBBY_MARGIN in from the screen's sides at any
 * aspect (a 4:3 window has dialogs about as far in: the frame's 20-unit
 * margin less the 13 of it off the screen), the height the menu's own
 * (menuFindAvailableSize). The two columns share the width. buttonsh is the
 * dialog's height: its box ends at the foot of the screen, the chat window
 * over it.
 */
static void lobbyLayout(struct lobbylayout *l, s32 buttonsh)
{
	s32 xmin;
	s32 ymin;
	s32 xmax;
	s32 ymax;
	s32 total;
	s32 lw;

#if VERSION >= VERSION_JPN_FINAL
	menuFindAvailableSize(&xmin, &ymin, &xmax, &ymax, NULL);
#else
	menuFindAvailableSize(&xmin, &ymin, &xmax, &ymax);
#endif

	l->pillar = lobbyPillar();
	l->top = ymin;
	l->bottom = ymax;

	// left-held, x 0 is the screen's left edge; right-held, the frame's
	// right edge (SCREEN_WIDTH_LO) is the screen's right edge
	total = SCREEN_WIDTH_LO + 2 * l->pillar - 2 * LOBBY_MARGIN;

	// past about 3:1 (WIDE's offset still grows) a column would reach past
	// 320 in its own side's space, where no text is drawn: the columns stop
	// growing there and the gap between them takes the rest
	if (total > 480) {
		total = 480;
	}
	lw = (total - LOBBY_GAP) * LOBBY_LEFTPCT / 100;
	l->lx1 = LOBBY_MARGIN;
	l->lx2 = LOBBY_MARGIN + lw;
	l->rx2 = SCREEN_WIDTH_LO - LOBBY_MARGIN;
	l->rx1 = l->rx2 - (total - LOBBY_GAP - lw);

	if (buttonsh > (l->bottom - l->top) * 3 / 5) {
		buttonsh = (l->bottom - l->top) * 3 / 5;
	}

	l->buttonsh = buttonsh;
	l->buttonsy = l->bottom - buttonsh;
	l->modsy1 = l->bottom - (LINEHEIGHT + 3 + LOBBY_MODROWS * LOBBY_SMROW + 2);
	l->roomy2 = l->modsy1 - LOBBY_VGAP;
}

static struct fontchar *lobbyChars(s32 font)
{
	return font == LOBBYFONT_MD ? g_CharsHandelGothicMd : font == LOBBYFONT_SM ? g_CharsHandelGothicSm : g_CharsHandelGothicXs;
}

static struct font *lobbyFont(s32 font)
{
	return font == LOBBYFONT_MD ? g_FontHandelGothicMd : font == LOBBYFONT_SM ? g_FontHandelGothicSm : g_FontHandelGothicXs;
}

static s32 lobbyTextWidth(const char *text, s32 font)
{
	s32 w = 0;
	s32 h = 0;

	textMeasure(&h, &w, (char *)text, lobbyChars(font), lobbyFont(font), 0);

	return w;
}

// text cut (in place) to width in a font, ".." where it was cut
static void lobbyFit(char *text, s32 size, s32 width, s32 font)
{
	s32 n = strlen(text);

	if (lobbyTextWidth(text, font) <= width) {
		return;
	}

	while (n > 0) {
		text[--n] = '\0';

		while (n > 0 && text[n - 1] == ' ') {
			text[--n] = '\0';
		}

		if (n + 3 <= size) {
			memcpy(text + n, "..", 3);

			if (lobbyTextWidth(text, font) <= width) {
				return;
			}

			text[n] = '\0';
		}
	}
}

// One line at x, y (menu units, the window's own space), cut to width
static Gfx *lobbyText(Gfx *gdl, s32 x, s32 y, const char *text, s32 font, u32 colour, s32 width)
{
	char buf[NETLOBBY_MAXCHATTEXT + 64];

	snprintf(buf, sizeof(buf), "%s", text);

	if (width > 0) {
		lobbyFit(buf, sizeof(buf), width, font);
	}

	return textRenderProjected(gdl, &x, &y, buf, lobbyChars(font), lobbyFont(font), colour, viGetWidth(), viGetHeight(), 0, 0);
}

// the same, its right edge at x2
static Gfx *lobbyTextRight(Gfx *gdl, s32 x2, s32 y, const char *text, s32 font, u32 colour)
{
	return lobbyText(gdl, x2 - lobbyTextWidth(text, font), y, text, font, colour, 0);
}

static u32 lobbyAlpha(u32 colour, u32 alpha)
{
	return (colour & 0xffffff00) | alpha;
}

/**
 * A window as the dialog draws itself (dialogRender()): the title bar's
 * gradient and shimmer, the title, the body and its borders, all in the
 * dialog's colours, so a change of its type (a red confirmation) or its
 * opening colours them as it colours itself. Only the frame while the
 * dialog grows open.
 */
static Gfx *lobbyWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2, char *title, s32 withtext)
{
	const u32 border1 = MIXCOLOUR(dialog, dialog_border1);
	const u32 titlebg = MIXCOLOUR(dialog, dialog_titlebg);
	const u32 border2 = MIXCOLOUR(dialog, dialog_border2);
	u32 bodybg = MIXCOLOUR(dialog, dialog_bodybg);
	const u32 bodybg2 = MIXCOLOUR(dialog, unused14);
	const s32 bodytop = y1 + LINEHEIGHT;
	s32 x;
	s32 y;

	if (y2 < bodytop) {
		y2 = bodytop;
	}

	if (dialog->dimmed) {
		bodybg = (colourBlend(bodybg, 0x00000000, 44) & 0xffffff00) | (bodybg & 0xff);
	}

	gdl = menugfxRenderGradient(gdl, x1 - 2, y1, x2 + 2, bodytop, border1, titlebg, border2);
	gdl = menugfxDrawShimmer(gdl, x1 - 2, y1, x2 + 2, y1 + 1, (border1 & 0xff) >> 1, 1, 40, 0);
	gdl = menugfxDrawShimmer(gdl, x1 - 2, y1 + 10, x2 + 2, bodytop, (border1 & 0xff) >> 1, 0, 40, 1);
	gdl = menugfxRenderDialogBackground(gdl, x1 + 1, bodytop, x2 - 1, y2, dialog, bodybg, bodybg2, -1.0f);

	if (withtext) {
		const u32 titlefg = MIXCOLOUR(dialog, dialog_titlefg);

		gdl = text0f153628(gdl);
		textSetWaveColours(g_MenuWave2Colours[dialog->type].dialog_titlefg, g_MenuWave1Colours[dialog->type].dialog_titlefg);

		x = x1 + 3;
		y = y1 + 3;
		gdl = textRenderProjected(gdl, &x, &y, title, g_CharsHandelGothicSm, g_FontHandelGothicSm, titlefg & 0xff, x2 - x1, viGetHeight(), 0, 0);

		x = x1 + 2;
		y = y1 + 2;
		gdl = textRenderProjected(gdl, &x, &y, title, g_CharsHandelGothicSm, g_FontHandelGothicSm, titlefg, x2 - x1, viGetHeight(), 0, 0);

		gdl = text0f153780(gdl);
	}

	return gdl;
}

/**
 * The dialog's text effects on a window of its own: the redraw sweeping
 * from the window's corner as dialogRender() has it sweep from the
 * dialog's (the menu blend, the rest of the time, is the dialog's already)
 */
static void lobbyWindowBlend(struct menudialog *dialog, s32 x, s32 y)
{
	if (!(dialog->redrawtimer < 0.0f) && g_MenuData.root != MENUROOT_MPPAUSE) {
		textSetDiagonalBlend(x, y, dialog->redrawtimer, dialog->state >= MENUDIALOGSTATE_POPULATED ? DIAGMODE_REDRAW : DIAGMODE_FADEIN);
	}
}

/**
 * Hold what follows against one side of a widescreen display, and the
 * scissor the whole view in that side's space: set after the alignment,
 * since fast3d turns a scissor into pixels with the alignment in force
 * (ghostmenu.c's menuGhostAlign)
 */
static Gfx *lobbyAlign(Gfx *gdl, s32 side)
{
	// the side set as well as the other cleared: the left column's turn
	// cleared the right's half before the right column's
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gSPSetExtraGeometryModeEXT(gdl++, side | G_ASPECT_WIDE_EXT);

	g_MenuScissorX1 = viGetViewLeft() / g_ScaleX;
	g_MenuScissorY1 = viGetViewTop();
	g_MenuScissorX2 = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	g_MenuScissorY2 = viGetViewTop() + viGetViewHeight();

	return menuApplyScissor(gdl);
}

static Gfx *lobbyAlignRestore(Gfx *gdl)
{
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_WIDE_EXT);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	g_MenuScissorX1 = viGetViewLeft() / g_ScaleX;
	g_MenuScissorY1 = viGetViewTop();
	g_MenuScissorX2 = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	g_MenuScissorY2 = viGetViewTop() + viGetViewHeight();

	return menuApplyScissor(gdl);
}

static const struct netlobbymember *rosterCell(s32 column, s32 row)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	s32 n = 0;
	s32 i;

	for (i = 0; i < room->nmembers; i++) {
		const struct netlobbymember *m = &room->members[i];
		const s32 col = m->spectator ? 2 : (m->team & 1);

		if (col == column && n++ == row) {
			return m;
		}
	}

	return NULL;
}

static s32 rosterCount(s32 column)
{
	s32 n = 0;

	while (rosterCell(column, n)) {
		n++;
	}

	return n;
}

/**
 * A member's way to the host and its ping: LAN, DIR (the host reachable),
 * NAT (punched through both NATs), RLY (through the lobby's relay), as the
 * member measured it; nothing on the host's own row (the settings above the
 * roster name the host). A narrow column has the ping alone.
 */
static void rosterNet(const struct netlobbymember *m, char *out, s32 size, s32 narrow)
{
	const char *tag = "";

	// the host's row has no path: the settings name it
	if (m->host) {
		out[0] = '\0';
		return;
	}

	if (strcmp(m->path, "lan") == 0) {
		tag = "LAN";
	} else if (strcmp(m->path, "direct") == 0) {
		tag = "DIR";
	} else if (strcmp(m->path, "punch") == 0) {
		tag = "NAT";
	} else if (strcmp(m->path, "relay") == 0) {
		tag = "RLY";
	} else if (strcmp(m->path, "none") == 0) {
		snprintf(out, size, "%s", narrow ? "--" : "NO PATH");
		return;
	} else {
		snprintf(out, size, "...");
		return;
	}

	if (m->ping >= 0 && narrow) {
		snprintf(out, size, "%d", m->ping > 999 ? 999 : m->ping);
	} else if (m->ping >= 0) {
		snprintf(out, size, "%s %d", tag, m->ping > 999 ? 999 : m->ping);
	} else {
		snprintf(out, size, "%s", narrow ? "" : tag);
	}
}

/**
 * A roster cell's tag (rosterNet: the ping alone in a narrow column) and the
 * room it takes from the name. The name comes first: a tag that would cut
 * a name that is whole without it is left out.
 */
// the room a roster name's ready mark takes before it
static s32 rosterMarkWidth(s32 font)
{
	return lobbyTextWidth("*", font) + 3;
}

static s32 rosterCellTag(const struct netlobbymember *m, s32 cw, s32 font, char *tag, s32 size)
{
	const s32 namew = lobbyTextWidth(m->user, font);
	const s32 markw = rosterMarkWidth(font);
	s32 tagw;

	rosterNet(m, tag, size, cw < 110);
	tagw = tag[0] ? lobbyTextWidth(tag, LOBBYFONT_XS) + 2 : 0;

	if (tagw > cw / 3 || (namew > cw - 3 - markw - tagw && namew <= cw - 3 - markw)) {
		tag[0] = '\0';
		tagw = 0;
	}

	return tagw;
}

static const char *ruleValue(const char *key)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	s32 i;

	for (i = 0; i < room->nrules; i++) {
		if (strcmp(room->rules[i].key, key) == 0) {
			return room->rules[i].value;
		}
	}

	return NULL;
}

#define LOBBY_MAXSETTINGS 8

struct lobbysetting {
	const char *label;
	char value[48];
};

/**
 * The room's settings, a label and a value each: a Combat Simulator room's
 * arena, mode, host, limits, simulants and weapons; a co-op room's mission
 * (or campaign), difficulty, host, radar and friendly fire. The mods are in
 * the window under the room.
 */
static s32 lobbySettings(struct lobbysetting *out)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	const char *time = ruleValue("time_limit");
	const char *score = ruleValue("score_limit");
	const char *weapons = ruleValue("weapons");
	const char *key = ruleValue("stage_key");
	s32 n = 0;

#define SETTING(lbl, ...) do { out[n].label = lbl; snprintf(out[n].value, sizeof(out[n].value), __VA_ARGS__); n++; } while (0)

	if (key && (strncmp(key, "mission:", 8) == 0 || strncmp(key, "campaign:", 9) == 0)) {
		// a co-op room (netcoop.c): its mission, difficulty and options,
		// none of the Combat Simulator's limits, weapons or simulants
		const s32 campaign = key[0] == 'c';
		const char *radar = ruleValue("radar");
		const char *ff = ruleValue("friendly_fire");

		SETTING(campaign ? "Campaign" : "Mission", "%s", room->sum.stage[0] ? room->sum.stage : "-");
		SETTING("Mode", "%s", campaign ? "Host picks missions" : room->sum.scenario[0] ? room->sum.scenario : "-");
		SETTING("Host", "%s", room->sum.host[0] ? room->sum.host : "-");
		SETTING("Radar", "%s", radar && strcmp(radar, "0") ? "On" : radar ? "Off" : "-");
		SETTING("Friendly Fire", "%s", ff && strcmp(ff, "0") ? "On" : ff ? "Off" : "-");
	} else {
		// the long values first: in two columns they take the wider
		SETTING("Arena", "%s", room->sum.stage[0] ? room->sum.stage : "-");
		SETTING("Mode", "%s", room->sum.scenario[0] ? room->sum.scenario : "-");
		SETTING("Weapons", "%s", weapons ? weapons : "-");
		SETTING("Host", "%s", room->sum.host[0] ? room->sum.host : "-");
		SETTING("Time", "%s%s", time && strcmp(time, "0") ? time : "None", time && strcmp(time, "0") ? " min" : "");
		SETTING("Score", "%s", score && strcmp(score, "0") ? score : "None");
		SETTING("Simulants", "%d", room->sum.sims);
	}

#undef SETTING

	return n;
}

/**
 * The room's window: its name and count, the settings (two columns where
 * the window is wide enough), the roster in team columns with ready marks
 * and each member's way to the host. Three columns (Team 1, Team 2,
 * Spectators) where they fit; on a narrow window two, and the spectators
 * on a line under them. The names in the bigger font while every row fits
 * in it; what does not fit at all is counted on the last row.
 */
static Gfx *lobbyRoomWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 label = MIXCOLOUR(dialog, listgroup_headerfg);
	const u32 bright = lobbyAlpha(0x60f0f000, alpha);
	const u32 dim = dimColour(alpha);
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	struct lobbysetting settings[LOBBY_MAXSETTINGS];
	char buf[128];
	char count[24];
	s32 nsettings;
	s32 nleft;
	s32 cols;
	s32 colw;
	s32 colw2;
	s32 col2x;
	s32 y = y1 + LINEHEIGHT + 3;
	s32 font;
	s32 i;
	s32 c;

	if (!room->valid) {
		return lobbyText(gdl, tx, y, "Waiting for the room...", LOBBYFONT_SM, normal, inner);
	}

	// the name and the count, in the biggest font they fit
	snprintf(count, sizeof(count), " (%d/%d)", room->sum.humans, room->sum.maxhumans);
	snprintf(buf, sizeof(buf), "%s%s", room->sum.name, count);
	font = lobbyTextWidth(buf, LOBBYFONT_MD) <= inner ? LOBBYFONT_MD : LOBBYFONT_SM;

	if (font == LOBBYFONT_SM && lobbyTextWidth(buf, font) > inner) {
		snprintf(buf, sizeof(buf), "%s", room->sum.name);
		lobbyFit(buf, sizeof(buf), inner - lobbyTextWidth(count, font), font);
		strcat(buf, count);
	}

	gdl = lobbyText(gdl, tx, y, buf, font, normal, inner);
	y += font == LOBBYFONT_MD ? LOBBY_MDROW : LOBBY_SMROW + 2;

	// the settings, two columns (one on a window too narrow for them: at
	// 4:3 a column of seven left a full room's roster no rows). The right
	// column's values are short (the limits, the simulants): it is as wide
	// as they are, the left one has the rest.
	nsettings = lobbySettings(settings);
	cols = inner >= 140 ? 2 : 1;
	nleft = cols == 2 ? (nsettings + 1) / 2 : nsettings;
	colw2 = 0;

	for (i = nleft; i < nsettings; i++) {
		s32 w;

		snprintf(buf, sizeof(buf), "%s: %s", settings[i].label, settings[i].value);
		w = lobbyTextWidth(buf, LOBBYFONT_XS);

		if (w > colw2) {
			colw2 = w;
		}
	}

	if (colw2 > inner / 2) {
		colw2 = inner / 2;
	}

	// the right column at the middle where the left one's lines leave it
	// room, else as far right as it fits
	colw = cols == 2 ? inner - 6 - colw2 : inner;
	col2x = colw + 6;

	if (cols == 2) {
		s32 need = 0;

		for (i = 0; i < nleft; i++) {
			s32 w;

			snprintf(buf, sizeof(buf), "%s: %s", settings[i].label, settings[i].value);
			w = lobbyTextWidth(buf, LOBBYFONT_XS);

			if (w > need) {
				need = w;
			}
		}

		if (need + 12 <= inner / 2 && inner / 2 + colw2 <= inner) {
			col2x = inner / 2;
		} else if (need + 12 < col2x) {
			col2x = need + 12;
		}
	}

	for (i = 0; i < nsettings; i++) {
		const s32 col = i >= nleft;
		const s32 row = col ? i - nleft : i;
		const s32 sx = tx + (col ? col2x : 0);
		const s32 sy = y + row * LOBBY_XSROW;
		const s32 cw = col ? colw2 : colw;
		s32 lw;

		snprintf(buf, sizeof(buf), "%s: ", settings[i].label);
		lw = lobbyTextWidth(buf, LOBBYFONT_XS);
		gdl = lobbyText(gdl, sx, sy, buf, LOBBYFONT_XS, label, cw);
		gdl = lobbyText(gdl, sx + lw, sy, settings[i].value, LOBBYFONT_XS, normal, cw - lw);
	}

	y += nleft * LOBBY_XSROW + 4;

	// the roster: the teams' columns the wider, the spectators' (seldom
	// more than a name or two) the narrower
	{
		static const char *heads[3] = { "Team 1", "Team 2", "Spectators" };
		const s32 ncols = inner >= 200 ? 3 : 2;
		const s32 specs = rosterCount(2);
		s32 colx[3];
		s32 colwidth[3];
		s32 rows = 1;
		s32 rowh;
		s32 avail;
		s32 fit;
		s32 nfont;

		if (ncols == 3) {
			colwidth[0] = colwidth[1] = inner * 39 / 100;
			colwidth[2] = inner - colwidth[0] - colwidth[1];
		} else {
			colwidth[0] = colwidth[1] = inner / 2;
			colwidth[2] = 0;
		}

		colx[0] = tx;
		colx[1] = tx + colwidth[0];
		colx[2] = colx[1] + colwidth[1];

		for (c = 0; c < ncols; c++) {
			const s32 n = rosterCount(c);

			snprintf(buf, sizeof(buf), "%s: %d", heads[c], n);

			if (lobbyTextWidth(buf, LOBBYFONT_SM) > colwidth[c] - 4) {
				snprintf(buf, sizeof(buf), "%s: %d", c == 2 ? "Spec" : c == 1 ? "T2" : "T1", n);
			}

			gdl = lobbyText(gdl, colx[c], y, buf, LOBBYFONT_SM, headerColour(alpha), colwidth[c] - 4);

			if (n > rows) {
				rows = n;
			}
		}

		y += LOBBY_SMROW + 1;

		// a narrow window's spectators on a line of their own at the foot
		avail = y2 - 3 - y - (ncols == 2 && specs > 0 ? LOBBY_XSROW : 0);
		nfont = rows * LOBBY_SMROW <= avail ? LOBBYFONT_SM : LOBBYFONT_XS;

		// and while every name is whole in it: a cut name in the bigger
		// font says less than a whole one in the small
		for (c = 0; c < ncols && nfont == LOBBYFONT_SM; c++) {
			for (i = 0; i < rosterCount(c); i++) {
				const struct netlobbymember *m = rosterCell(c, i);
				char tag[16];
				const s32 tagw = rosterCellTag(m, colwidth[c], LOBBYFONT_SM, tag, sizeof(tag));

				if (lobbyTextWidth(m->user, LOBBYFONT_SM) > colwidth[c] - 3 - rosterMarkWidth(LOBBYFONT_SM) - tagw) {
					nfont = LOBBYFONT_XS;
					break;
				}
			}
		}

		rowh = nfont == LOBBYFONT_SM ? LOBBY_SMROW : LOBBY_XSROW;
		fit = avail / rowh;

		if (fit < 1) {
			fit = 1;
		}

		for (c = 0; c < ncols; c++) {
			const s32 n = rosterCount(c);
			const s32 cw = colwidth[c];
			s32 r;

			for (r = 0; r < n && r < fit; r++) {
				const struct netlobbymember *m = rosterCell(c, r);
				const s32 cx = colx[c];
				const s32 cy = y + r * rowh;
				char tag[16];
				s32 tagw;

				if (r == fit - 1 && n > fit) {
					snprintf(buf, sizeof(buf), "+%d more", n - fit + 1);
					gdl = lobbyText(gdl, cx, cy, buf, nfont, dim, cw - 4);
					break;
				}

				// the ready mark, the name, the path and ping at the
				// column's right; the name cut to what is left
				tagw = rosterCellTag(m, cw, nfont, tag, sizeof(tag));

				if (m->ready || m->host) {
					gdl = lobbyText(gdl, cx, cy, "*", nfont, bright, 0);
				}

				gdl = lobbyText(gdl, cx + rosterMarkWidth(nfont), cy, m->user, nfont, m->ready || m->host ? bright : dim,
						cw - 3 - rosterMarkWidth(nfont) - tagw);

				if (tagw) {
					gdl = lobbyTextRight(gdl, cx + cw - 3, cy + (nfont == LOBBYFONT_SM ? 2 : 0), tag, LOBBYFONT_XS, dim);
				}
			}
		}

		if (ncols == 2 && specs > 0) {
			const s32 sy = y2 - 3 - LOBBY_XSROW;
			s32 lw;

			snprintf(buf, sizeof(buf), "Spectators: %d  ", specs);
			lw = lobbyTextWidth(buf, LOBBYFONT_XS);
			gdl = lobbyText(gdl, tx, sy, buf, LOBBYFONT_XS, headerColour(alpha), inner);

			buf[0] = '\0';

			for (i = 0; i < specs; i++) {
				const struct netlobbymember *m = rosterCell(2, i);

				if (strlen(buf) + strlen(m->user) + 3 < sizeof(buf)) {
					strcat(buf, i ? ", " : "");
					strcat(buf, m->user);
				}
			}

			gdl = lobbyText(gdl, tx + lw, sy, buf, LOBBYFONT_XS, bright, inner - lw);
		}
	}

	return gdl;
}

/**
 * The mods window: what the room plays with (netContentLobbyRows(): the
 * GoldenEye conversion or ROM hack, the host's Perfect Dark mod, a map's
 * mod) and where this machine stands with each - here, coming from the host
 * at the launch, or its download (or, on the host, its send) as it goes.
 * The rows left over say <empty>, as the mock-up has them.
 */
static Gfx *lobbyModsWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 disabled = MIXCOLOUR(dialog, item_disabled);
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	struct netcontentrow rows[8];
	char state[48];
	s32 nrows = 0;
	s32 fit = (y2 - 2 - (y1 + LINEHEIGHT + 3)) / LOBBY_SMROW;
	s32 y = y1 + LINEHEIGHT + 3;
	s32 i;

	if (room->valid) {
		nrows = netContentLobbyRows(room->sum.mod, room->sum.ge, ruleValue("stage_key"), rows, ARRAYCOUNT(rows));
	}

	for (i = 0; i < fit; i++, y += LOBBY_SMROW) {
		u32 colour;
		s32 sw;

		if (i >= nrows) {
			gdl = lobbyText(gdl, tx, y, "<empty>", LOBBYFONT_SM, disabled, inner);
			continue;
		}

		if (i == fit - 1 && nrows > fit) {
			snprintf(state, sizeof(state), "+%d more", nrows - fit + 1);
			gdl = lobbyText(gdl, tx, y, state, LOBBYFONT_SM, dimColour(alpha), inner);
			break;
		}

		colour = rows[i].kind == NETCONTENTROW_BUSY ? headerColour(alpha)
			: rows[i].kind == NETCONTENTROW_MISSING ? lobbyAlpha(0xffff6000, alpha) : dimColour(alpha);
		snprintf(state, sizeof(state), "(%s)", rows[i].state);
		sw = lobbyTextWidth(state, LOBBYFONT_XS);

		gdl = lobbyText(gdl, tx, y, rows[i].name, LOBBYFONT_SM, normal, inner - sw - 6);
		gdl = lobbyTextRight(gdl, tx + inner, y + 2, state, LOBBYFONT_XS, colour);
	}

	return gdl;
}

/**
 * The status line: where the room and this machine stand - the countdown,
 * connecting, waiting for the host, a fetch of its content, the reason a
 * launch ended - or what to do next. No newline: the chat window wraps it.
 */
static void lobbyStatusText(char *out, s32 size)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	char fetch[192];

	switch (netLobbyLaunchState()) {
	case 1:
		snprintf(out, size, "Launching in %d...", (netLobbyCountdownMs() + 999) / 1000);
		break;
	case 2:
		if (netLobbyLaunchEnded()) {
			// refused or left: why, not "connecting"
			snprintf(out, size, "%s", netLobbyLaunchEndedText());
		} else if (netLobbyCampaignState() == 1) {
			snprintf(out, size, "%s", "Campaign: start missions from your menus.");
		} else if (netLobbyCampaignState() == 2) {
			snprintf(out, size, "%s", "Campaign: you join the host's next mission.");
		} else if (netLobbyJoinedWait() == 1) {
			// in with the host, whose match had ended (its end screen up):
			// the room opens again when it leaves that, and the next
			// launch has this player in it
			snprintf(out, size, "%s", "In: the host's match is over; you join its next.");
		} else if (netContentFetchLine(fetch, sizeof(fetch))) {
			// in with the host, its map's mod coming from it (through the
			// lobby's relay a few MB take a minute): not "waiting for the
			// host" (F3 20261010-040905, "relay did not connect properly")
			snprintf(out, size, "%s", fetch);
		} else if (netLobbyJoinedWait() == 2) {
			snprintf(out, size, "%s", "In: waiting for the host to start the match.");
		} else {
			snprintf(out, size, "%s", netLobbyIsHost() ? "Launched: waiting for the players to connect..." : "Launched: connecting to the host...");
		}
		break;
	case 3:
		snprintf(out, size, "%s", "In the match.");
		break;
	case 4:
		// host migration: the room's match goes on under its new host
		if (netLobbyIsHost()) {
			snprintf(out, size, "%s", "You host the room now: the match carries on in a moment.");
		} else {
			snprintf(out, size, "%s hosts the room now: the match carries on in a moment.", room->sum.host);
		}
		break;
	default:
		if (netLobbyMessage()[0]) {
			snprintf(out, size, "%s", netLobbyMessage());
		} else if (netLobbyIsHost()) {
			snprintf(out, size, "%s", "Launch when everyone is ready (* = ready).");
		} else {
			snprintf(out, size, "%s", netLobbyMyReady() ? "Ready: waiting for the host to launch." : "Select Ready when you are.");
		}
		break;
	}
}

/**
 * The status line as the chat window shows it. netcooptest.sh reads it
 * through gdb (the nomodroom and campleave cases: a refusal's reason, not
 * "connecting").
 */
const char *netLobbyMenuStatusText(void)
{
	static char text[480];

	lobbyStatusText(text, sizeof(text));

	return text;
}

#define LOBBY_MAXCHATLINES 64

struct lobbychatline {
	char text[NETLOBBY_MAXCHATTEXT + NETLOBBY_MAXUSER + 8];
	s32 userlen; // the "user: " at its start, drawn in the label colour (0 none)
	u32 colour;
};

/**
 * A line wrapped to the window into lines of its own. The wrap is the
 * font's (textWrapN), a word wider than the window cut with dots.
 */
static s32 lobbyChatWrap(struct lobbychatline *lines, s32 n, const char *text, s32 userlen, u32 colour, s32 width)
{
	char in[512]; // a chat line, or the status line's longest: a refusal's reason naming a mod
	char out[sizeof(in) + 128];
	char *p = out;
	s32 first = 1;

	// the wrap measures a little narrower than textMeasure: wrapped to the
	// width itself, a full line came out a letter too long and lost it
	snprintf(in, sizeof(in), "%s\n", text);
	textWrapN(width - 8, in, out, sizeof(out), g_CharsHandelGothicXs, g_FontHandelGothicXs);

	while (*p && n < LOBBY_MAXCHATLINES) {
		char *end = strchr(p, '\n');
		struct lobbychatline *line = &lines[n++];

		if (end) {
			*end = '\0';
		}

		snprintf(line->text, sizeof(line->text), "%s", p);
		lobbyFit(line->text, sizeof(line->text), width, LOBBYFONT_XS);
		line->userlen = first ? userlen : 0;
		line->colour = colour;
		first = 0;

		if (!end) {
			break;
		}

		p = end + 1;
	}

	return n;
}

/**
 * The chat window: the room's chat, oldest at the top, the status line at
 * its foot and the line being typed under that (Space or the chat key,
 * nethud.c). The newest lines are the ones kept when they are more than
 * the window holds.
 */
static Gfx *lobbyChatWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 label = MIXCOLOUR(dialog, listgroup_headerfg);
	const u32 white = MIXCOLOUR(dialog, item_focused_inner);
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	static struct lobbychatline lines[LOBBY_MAXCHATLINES];
	char text[sizeof(lines[0].text) + 128];
	s32 nlines = 0;
	s32 fit = (y2 - 2 - (y1 + LINEHEIGHT + 3)) / LOBBY_XSROW;
	s32 first;
	s32 y;
	s32 i;

	for (i = 0; i < room->nchat; i++) {
		snprintf(text, sizeof(text), "%s: %s", room->chat[i].user, room->chat[i].text);
		nlines = lobbyChatWrap(lines, nlines, text, strlen(room->chat[i].user) + 2, normal, inner);
	}

	if (room->nchat == 0) {
		nlines = lobbyChatWrap(lines, nlines, "No chat yet: press Space to talk.", 0, dimColour(alpha), inner);
	}

	nlines = lobbyChatWrap(lines, nlines, netLobbyMenuStatusText(), 0, white, inner);

	// the line as it is typed (its tail and a cursor), the newline off
	if (netHudChatLine(text, sizeof(text), NETLOBBY_MAXCHATTEXT)) {
		char *nl = strchr(text, '\n');

		if (nl) {
			*nl = '\0';
		}

		// as much of its tail as the window shows
		while (lobbyTextWidth(text, LOBBYFONT_XS) > inner && strlen(text) > 9) {
			memmove(text + 5, text + 9, strlen(text + 9) + 1);
			memcpy(text + 5, "...", 3);
		}

		if (nlines < LOBBY_MAXCHATLINES) {
			snprintf(lines[nlines].text, sizeof(lines[0].text), "%s", text);
			lines[nlines].userlen = 0;
			lines[nlines].colour = white;
			nlines++;
		}
	}

	first = nlines > fit ? nlines - fit : 0;
	y = y1 + LINEHEIGHT + 3;

	for (i = first; i < nlines; i++, y += LOBBY_XSROW) {
		if (lines[i].userlen > 0 && lines[i].userlen < (s32)strlen(lines[i].text)) {
			char user[NETLOBBY_MAXUSER + 4];
			s32 uw;

			snprintf(user, sizeof(user), "%.*s", lines[i].userlen, lines[i].text);
			uw = lobbyTextWidth(user, LOBBYFONT_XS);
			gdl = lobbyText(gdl, tx, y, user, LOBBYFONT_XS, label, 0);
			gdl = lobbyText(gdl, tx + uw, y, lines[i].text + lines[i].userlen, LOBBYFONT_XS, lines[i].colour, inner - uw);
		} else {
			gdl = lobbyText(gdl, tx, y, lines[i].text, LOBBYFONT_XS, lines[i].colour, inner);
		}
	}

	return gdl;
}

static MenuItemHandlerResult handlerChatKeyboard(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		data->keyboard.string[0] = '\0';
		break;
	case MENUOP_SETTEXT:
		snprintf(s_ChatLine, sizeof(s_ChatLine), "%s", data->keyboard.string);
		break;
	case MENUOP_SET:
		// the room's chat in the Game Lobby, the match's from its pause
		// menu (nethud.c)
		netHudSay(s_ChatLine);
		break;
	}

	return 0;
}

static struct menuitem s_ChatItems[] = {
	{ MENUITEMTYPE_KEYBOARD, MPSETUP_MAXNAME, 0, 0, 0, handlerChatKeyboard },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_ChatDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Chat", s_ChatItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static MenuItemHandlerResult handlerChat(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&s_ChatDialog);
	}

	return 0;
}

/*
 * A match's pause menu: Players (protocol 18, nethud.c). The seats played
 * or kept and the spectators with their pings, and the chat for a player
 * with no keyboard: the on-screen one, or a line picked from a list. Its
 * rows are laid out as it opens (a label's size is taken then), their text
 * live after.
 */

#define PLAYERS_ROWS (MAX_PLAYERS + NET_MAXSPECS)

static const char *const s_QuickLines[] = {
	"Good game!",
	"Nice shot!",
	"Wait for me.",
	"Follow me.",
	"Need help!",
	"On my way.",
	"Thanks!",
	"Sorry!",
	"Ready.",
	"One more?",
};

static char *textPlayersCount(struct menuitem *item)
{
	static char text[64];
	s32 in;
	s32 of;
	s32 specs;

	netHudCounts(&in, &of, &specs);

	if (specs > 0) {
		snprintf(text, sizeof(text), "%d/%d playing, %d watching\n", in, of, specs);
	} else {
		snprintf(text, sizeof(text), "%d/%d playing\n", in, of);
	}

	return text;
}

static char s_PlayerNames[PLAYERS_ROWS][NET_MAXNAME + 24];
static char s_PlayerValues[PLAYERS_ROWS][16];

static char *textPlayerName(struct menuitem *item)
{
	const s32 k = item->param % PLAYERS_ROWS;

	if (!netHudPlayerRow(k, s_PlayerNames[k], sizeof(s_PlayerNames[k]), s_PlayerValues[k], sizeof(s_PlayerValues[k]))) {
		snprintf(s_PlayerNames[k], sizeof(s_PlayerNames[k]), "%s", "-\n");
		snprintf(s_PlayerValues[k], sizeof(s_PlayerValues[k]), "%s", "\n");
	}

	return s_PlayerNames[k];
}

static char *textPlayerValue(struct menuitem *item)
{
	return s_PlayerValues[item->param % PLAYERS_ROWS];
}

static MenuItemHandlerResult handlerPlayerRow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	char name[NET_MAXNAME + 24];
	char value[16];

	if (operation == MENUOP_CHECKHIDDEN) {
		return !netHudPlayerRow(item->param, name, sizeof(name), value, sizeof(value));
	}

	return 0;
}

static MenuItemHandlerResult handlerQuickLine(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netHudSay(s_QuickLines[item->param % ARRAYCOUNT(s_QuickLines)]);
	}

	return 0;
}

#define QUICKROW(i, text) { MENUITEMTYPE_SELECTABLE, i, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)text "\n", 0, handlerQuickLine }

static struct menuitem s_QuickItems[] = {
	QUICKROW(0, "Good game!"), QUICKROW(1, "Nice shot!"), QUICKROW(2, "Wait for me."), QUICKROW(3, "Follow me."),
	QUICKROW(4, "Need help!"), QUICKROW(5, "On my way."), QUICKROW(6, "Thanks!"), QUICKROW(7, "Sorry!"),
	QUICKROW(8, "Ready."), QUICKROW(9, "One more?"),
	{ MENUITEMTYPE_END },
};

static struct menudialogdef s_QuickDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Quick Message", s_QuickItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

static MenuItemHandlerResult handlerQuickOpen(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&s_QuickDialog);
	}

	return 0;
}

#define PLAYERROW(i) { MENUITEMTYPE_LABEL, i, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textPlayerName, (uintptr_t)&textPlayerValue, handlerPlayerRow }

static struct menuitem s_PlayersItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&textPlayersCount, 0, NULL },
	PLAYERROW(0), PLAYERROW(1), PLAYERROW(2), PLAYERROW(3), PLAYERROW(4), PLAYERROW(5), PLAYERROW(6),
	PLAYERROW(7), PLAYERROW(8), PLAYERROW(9), PLAYERROW(10), PLAYERROW(11), PLAYERROW(12), PLAYERROW(13),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Send Message...\n", 0, handlerChat },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Quick Message...\n", 0, handlerQuickOpen },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetPlayersMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Players",
	s_PlayersItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

// The pause menus' row (ingame.c's Control page, mainmenu.c's mission
// pause): "Players (3/8)", only in a net match
char *netLobbyMenuTextPlayers(struct menuitem *item)
{
	static char text[48];
	s32 in;
	s32 of;
	s32 specs;

	netHudCounts(&in, &of, &specs);
	snprintf(text, sizeof(text), "Players (%d/%d)\n", in, of);

	return text;
}

MenuItemHandlerResult netLobbyMenuHandlerPlayers(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return g_NetMode == NETMODE_NONE;
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetPlayersMenuDialog);
	}

	return 0;
}

static s32 roomIsOpen(void)
{
	return netLobbyInRoom() && netLobbyLaunchState() == 0;
}

static MenuItemHandlerResult handlerTeam(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !roomIsOpen();
	}

	if (operation == MENUOP_SET) {
		netLobbyCycleTeam();
	}

	return 0;
}

static char *textReady(struct menuitem *item)
{
	if (netLobbyIsHost()) {
		return netLobbyLaunchState() == 1 ? "Cancel Launch\n" : "Launch\n";
	}

	return netLobbyMyReady() ? "Ready: Yes\n" : "Ready: No\n";
}

static MenuItemHandlerResult handlerReady(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyInRoom() || netLobbyLaunchState() >= 2 || (!netLobbyIsHost() && netLobbyMyTeam() < 0);
	}

	if (operation == MENUOP_SET) {
		netLobbyClearMessage();

		if (!netLobbyIsHost()) {
			netLobbySetReady(!netLobbyMyReady());
		} else if (netLobbyLaunchState() == 1) {
			netLobbyCancelLaunch();
		} else {
			netLobbyLaunch(0);
		}
	}

	return 0;
}

static MenuItemHandlerResult handlerHostOnly(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !netLobbyIsHost();
	}

	if (operation == MENUOP_CHECKDISABLED) {
		return !roomIsOpen();
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(item->param == 1 ? &g_NetKickMenuDialog : &g_NetRoomSettingsMenuDialog);
	}

	return 0;
}

/**
 * A campaign's host (netcoop.c): the set's menus again. The Game Lobby
 * ignores Back, and a campaign host comes to it from the Perfect Menu's
 * Online Game between missions.
 */
static MenuItemHandlerResult handlerCampaign(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !netCoopCampaignOn() || !netLobbyIsHost();
	}

	if (operation == MENUOP_SET) {
		netCoopCampaignMenusOpen();
	}

	return 0;
}

static MenuItemHandlerResult handlerLeave(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netLobbyLeave();
		menuPopDialog();
	}

	return 0;
}

static MenuDialogHandlerResult dialogRoom(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		netLobbyClearMessage();
		s_Status[0] = '\0';
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		// its chat line may open: Space or the chat key (nethud.c)
		netHudLobbyFrame();

		// the seat went (kicked, the room closed): back to where it was
		// opened from, the reason on the status line there
		if (!netLobbyInRoom()) {
			menuPopDialog();
		}
	}

	return 0;
}

// The buttons: the dialog itself, the box at the foot of the right column
// (the windows around it are drawn by netLobbyMenuRenderPanels)
static struct menuitem s_RoomItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textReady, 0, handlerReady },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Change Team\n", 0, handlerTeam },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Chat\n", 0, handlerChat },
	{ MENUITEMTYPE_SELECTABLE, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Kick...\n", 0, handlerHostOnly },
	{ MENUITEMTYPE_SELECTABLE, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Settings...\n", 0, handlerHostOnly },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back to Campaign\n", 0, handlerCampaign },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Leave Lobby\n", 0, handlerLeave },
	{ MENUITEMTYPE_END },
};

// No title bar: the windows around it have theirs, and the box under the
// chat has none in the mock-up either
struct menudialogdef g_NetRoomMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Game Lobby",
	s_RoomItems,
	dialogRoom,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_DISABLETITLEBAR,
	NULL,
};

s32 netLobbyMenuDialogAlign(struct menudialogdef *def)
{
	return def == &g_NetRoomMenuDialog ? G_ASPECT_RIGHT_EXT : 0;
}

s32 netLobbyMenuDialogAlignOffset(struct menudialogdef *def)
{
	return def == &g_NetRoomMenuDialog ? lobbyPillar() : 0;
}

/**
 * The Game Lobby's buttons at the foot of the right column, in the right-
 * held space they are drawn in, as wide as the column and as tall as their
 * rows (dialogCalculateContentSize, every tick: the host's rows come and go)
 */
void netLobbyMenuPlaceDialog(struct menudialog *dialog)
{
	struct lobbylayout l;

	if (dialog->definition != &g_NetRoomMenuDialog) {
		return;
	}

	lobbyLayout(&l, dialog->contentheight);

	dialog->dstx = l.rx1;
	dialog->dsty = l.buttonsy;
	dialog->dstwidth = l.rx2 - l.rx1;
	dialog->dstheight = l.buttonsh;
}

/**
 * The windows around the Game Lobby's buttons, after the dialog
 * (menuRenderDialog): the room and its mods held to the left of a
 * widescreen display, the chat to the right over the buttons, whose box
 * the chat's foot follows as it grows and shrinks. They slide with the
 * dialog, grow open with it and take its text effects; while it grows open
 * they are frames.
 */
void *netLobbyMenuRenderPanels(void *gdlptr, struct menudialog *dialog)
{
	Gfx *gdl = gdlptr;
	struct lobbylayout l;
	s32 withtext = dialog->state != MENUDIALOGSTATE_PREOPEN;
	f32 grow = 1.0f;
	s32 dx;
	s32 chaty2;

	if (dialog->definition != &g_NetRoomMenuDialog) {
		return gdl;
	}

	lobbyLayout(&l, dialog->height);

	// a slide (the dialog swiped) moves the windows with it
	dx = dialog->x - l.rx1;
	chaty2 = dialog->y + LINEHEIGHT - LOBBY_VGAP;

	if (dialog->state == MENUDIALOGSTATE_PREOPEN) {
		// as dialogRender() grows the dialog open
		grow = 1.0f - g_MenuData.unk010;
		grow = 1.0f - grow * grow;
	}

	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	// the left column: the room, the mods
	gdl = lobbyAlign(gdl, G_ASPECT_LEFT_EXT);

	{
		const s32 x1 = l.lx1 + dx;
		const s32 x2 = l.lx2 + dx;
		const s32 roomy2 = l.top + (s32)((l.roomy2 - l.top) * grow);
		const s32 modsy2 = l.modsy1 + (s32)((l.bottom - l.modsy1) * grow);

		gdl = lobbyWindow(gdl, dialog, x1, l.top, x2, roomy2, (char *)"Game Lobby", withtext);
		gdl = lobbyWindow(gdl, dialog, x1, l.modsy1, x2, modsy2, (char *)"Mods", withtext);

		if (withtext) {
			gdl = text0f153628(gdl);
			lobbyWindowBlend(dialog, x1, l.top);
			gdl = lobbyRoomWindow(gdl, dialog, x1, l.top, x2, roomy2);
			lobbyWindowBlend(dialog, x1, l.modsy1);
			gdl = lobbyModsWindow(gdl, dialog, x1, l.modsy1, x2, modsy2);
			gdl = text0f153780(gdl);
		}
	}

	// the right column: the chat, over the buttons
	gdl = lobbyAlign(gdl, G_ASPECT_RIGHT_EXT);

	{
		const s32 x1 = l.rx1 + dx;
		const s32 x2 = l.rx2 + dx;
		const s32 y2 = l.top + (s32)((chaty2 - l.top) * grow);

		gdl = lobbyWindow(gdl, dialog, x1, l.top, x2, y2, (char *)"Lobby Chat", withtext);

		if (withtext) {
			gdl = text0f153628(gdl);
			lobbyWindowBlend(dialog, x1, l.top);
			gdl = lobbyChatWindow(gdl, dialog, x1, l.top, x2, y2);
			gdl = text0f153780(gdl);
		}
	}

	return lobbyAlignRestore(gdl);
}

// The host's KICK: every other member, one to pick
static const struct netlobbymember *kickable(s32 index)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	s32 i;

	for (i = 0; i < room->nmembers; i++) {
		if (!room->members[i].host && index-- == 0) {
			return &room->members[i];
		}
	}

	return NULL;
}

static MenuItemHandlerResult handlerKickList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[32];
	const struct netlobbymember *m;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		for (data->list.value = 0; kickable(data->list.value); data->list.value++) {
		}
		break;
	case MENUOP_GETOPTIONTEXT:
		m = kickable(data->list.value);
		snprintf(text, sizeof(text), "%s", m ? m->user : "");
		return (intptr_t)text;
	case MENUOP_SET:
		m = kickable(data->list.value);

		if (m) {
			netLobbyKick(m->user);
			menuPopDialog();
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0xfffff;
		break;
	}

	return 0;
}

static struct menuitem s_KickItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"Remove a player from the room (they cannot rejoin it).\n", 0, NULL },
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_LEAVEATENDS, 160, 66, handlerKickList },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetKickMenuDialog = {
	MENUDIALOGTYPE_DANGER, (uintptr_t)"Kick", s_KickItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

// The host's SETTINGS: the same setup pages, then the room hears them.
// The pages write the setup itself, so however this page closes (Apply,
// Back, the back button) a changed setup goes to the room: the match is
// built from it, and the room must not show (or ready players accept)
// something else.
static struct mpsetup s_SettingsSetup;
static struct mpbotconfig s_SettingsBots[MAX_BOTS];
static struct netcoopsetup s_SettingsCoop;
static u32 s_SettingsExtra;

/**
 * What the setup pages change outside g_MpSetup and the simulants: the
 * handicaps, the scenario's own option (King of the Hill's hill time, its
 * save bits) and the Room Rules (the host's SYNC settings). The host's own team goes to the room as a Change Team
 * does, which un-readies nobody
 */
static u32 settingsExtra(void)
{
	u32 h = netLobbyHandicapsHash();

	h = (h ^ g_PlayerConfigsArray[0].handicap) * 16777619u;
	h = (h ^ g_Vars.mphilltime) * 16777619u;
	h = (h ^ netRulesSyncHash()) * 16777619u;

	return h;
}

static MenuItemHandlerResult handlerSettingsApply(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPopDialog();
	}

	return 0;
}

static MenuDialogHandlerResult dialogRoomSettings(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		memcpy(&s_SettingsSetup, &g_MpSetup, sizeof(s_SettingsSetup));
		memcpy(s_SettingsBots, g_BotConfigsArray, sizeof(s_SettingsBots));
		s_SettingsCoop = g_NetCoopSetup;
		s_SettingsExtra = settingsExtra();
		s_HostTeamTouched = 0;
	}

	if (operation == MENUOP_CLOSE) {
		if (memcmp(&s_SettingsSetup, &g_MpSetup, sizeof(s_SettingsSetup)) != 0
				|| memcmp(s_SettingsBots, g_BotConfigsArray, sizeof(s_SettingsBots)) != 0
				|| memcmp(&s_SettingsCoop, &g_NetCoopSetup, sizeof(s_SettingsCoop)) != 0
				|| settingsExtra() != s_SettingsExtra) {
			netLobbySendSettings();
		}

		hostTeamSend();
	}

	return 0;
}

static struct menuitem s_RoomSettingsItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"New settings un-ready everyone.\n", 0, NULL },
	NETLOBBY_SETUP_ITEMS,
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Apply\n", 0, handlerSettingsApply },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetRoomSettingsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Room Settings", s_RoomSettingsItems, dialogRoomSettings, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * The GAME LOBBY over whatever is up (netLobbyMenuAfterMatch): after a
 * match from the room, in place of the Combat Simulator or Perfect Menu
 */
void netLobbyMenuPushRoom(void)
{
	if (isCurrent(&g_NetRoomMenuDialog)) {
		return;
	}

	if (g_Menus[g_MpPlayerNum].curdialog) {
		menuPushDialog(&g_NetRoomMenuDialog);
	} else {
		menuPushRootDialog(&g_NetRoomMenuDialog, MENUROOT_MPSETUP);
	}
}

/**
 * Back from a room's match (netLobbyMenuAfterMatch): the Game Lobby over the
 * Online Game page, the way the room was entered, so Leave lands on that
 * page and Back from it where it was opened from - the Perfect Menu for
 * Perfect Dark's rooms, the mode's Combat Simulator for a GoldenEye or ROM
 * hack room (F3 2026-10-09: Leave after a match landed on Perfect Dark's
 * Combat Simulator, the root menutick.c puts up after every match).
 * menutick.c's return is the Combat Simulator alone as the MENUROOT_MPSETUP
 * root: Perfect Dark's is swapped for the page, a mode's keeps it under the
 * page. A client whose match ended under it is on the Perfect Menu
 * (netMainMenuTick): the page opens from there as its row opens it, and the
 * Game Lobby goes over it once it is up (netLobbyMenuFrame). Anything else
 * (an Advanced Setup's pages) keeps the Game Lobby over what is up.
 */
static s32 s_RoomOverOnline = 0; // the Game Lobby to go over the Online Game page once it is up

void netLobbyMenuPushRoomAfterMatch(void)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];
	struct menudialogdef *top = menu->curdialog ? menu->curdialog->definition : NULL;

	if (top == &g_NetRoomMenuDialog || s_RoomOverOnline) {
		return;
	}

	if (g_MenuData.root == MENUROOT_MPSETUP && menu->depth == 1 && top == &g_CombatSimulatorMenuDialog) {
		if (g_GexPlusMode) {
			menuPushDialog(&g_NetOnlineMenuDialog);
		} else {
			func0f0f3704(&g_NetOnlineMenuDialog);
		}

		netLobbyMenuPushRoom();
		return;
	}

	if (g_MenuData.root == MENUROOT_MAINMENU && menu->depth == 1 && top == &g_CiMenuViaPcMenuDialog) {
		g_Vars.mpsetupmenu = MPSETUPMENU_GENERAL;
		s_RoomOverOnline = 1;
		func0f0f820c(g_GexPlusMode ? &g_CombatSimulatorMenuDialog : &g_NetOnlineMenuDialog, MENUROOT_MPSETUP);
		func0f0f8300();
		return;
	}

	netLobbyMenuPushRoom();
}

// Once per loop (netPump): the client's way back from the Perfect Menu
void netLobbyMenuFrame(void)
{
	struct menudialogdef *top;

	if (!s_RoomOverOnline) {
		return;
	}

	if (!netLobbyInRoom()) {
		s_RoomOverOnline = 0;
		return;
	}

	if (g_MenuData.root != MENUROOT_MPSETUP || !g_Menus[g_MpPlayerNum].curdialog) {
		return; // the Perfect Menu still closing
	}

	top = g_Menus[g_MpPlayerNum].curdialog->definition;

	if (top == &g_CombatSimulatorMenuDialog && g_GexPlusMode) {
		menuPushDialog(&g_NetOnlineMenuDialog);
		return;
	}

	s_RoomOverOnline = 0;
	netLobbyMenuPushRoom();
}

// The Briefing Room over whatever is up (the lobby test's screenshots)
void netLobbyMenuPushBriefing(void)
{
	if (isCurrent(&g_NetBriefingMenuDialog)) {
		return;
	}

	if (g_Menus[g_MpPlayerNum].curdialog) {
		menuPushDialog(&g_NetBriefingMenuDialog);
	} else {
		menuPushRootDialog(&g_NetBriefingMenuDialog, MENUROOT_MPSETUP);
	}
}

// The lobby test's screenshots: is that page the one on screen?
s32 netLobbyMenuPageUp(s32 room)
{
	return isCurrent(room ? &g_NetRoomMenuDialog : &g_NetBriefingMenuDialog);
}
