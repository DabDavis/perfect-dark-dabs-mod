#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "game/menu.h"
#include "game/game_1531a0.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "lib/vi.h"
#include "game/lang.h"
#include "modloader.h"
#include "net/net.h"
#include "net/netlobby.h"

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
 *                                         Arena, Weapons, Limits, Simulants
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
#define ROSTER_HEIGHT 44 // the header and three rows (a room of up to six; rosterFit grows it)
#define LABEL_WIDTH 262  // a small-font label's line inside the dialog, in its own font's units (labelWidth; ~2.2 px a unit at 640x480, the line from x 32 to the edge at ~612); it does not widen the dialog
#define ROSTER_COLW 88   // three columns in the dialog's ~266 units
#define CHAT_LINES 3

static char s_RoomPassword[NETLOBBY_MAXPASSWORD + 1];
static char s_JoinPassword[NETLOBBY_MAXPASSWORD + 1];
static char s_ChatLine[NETLOBBY_MAXCHATTEXT + 1];
static struct netlobbycreate s_Create = { "", "", 4 };
static s32 s_SelectedRoom = -1;
static char s_JoinId[9];
static s32 s_WaitingForRoom = 0; // a join or create is in flight from these pages
static char s_Status[200];
static char s_BriefingNote[160]; // why the focused room cannot be joined

extern struct menudialogdef g_NetBriefingMenuDialog;
extern struct menudialogdef g_NetCreateMenuDialog;
extern struct menudialogdef g_NetRoomMenuDialog;
static void rosterFit(void);
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

static Gfx *drawCell(Gfx *gdl, struct menuitemrenderdata *rd, s32 x, const char *text, s32 maxchars, u32 colour)
{
	return drawCellAt(gdl, rd, x, 1, text, maxchars, colour);
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

static char *textAccount(struct menuitem *item)
{
	static char text[96];

	if (!netLobbyAvailable()) {
		snprintf(text, sizeof(text), "%s", "No account: make one in Ghost Trials > Account.\n");
	} else if (netLobbySignedIn()) {
		snprintf(text, sizeof(text), "Signed in as %s\n", netLobbyAccount());
	} else {
		snprintf(text, sizeof(text), "Account: %s\n", netLobbyAccount());
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
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
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
 * The Combat Simulator's own row for it (setup.c): greyed, with a hint,
 * until there is an account to sign in with, and in the GoldenEye mode,
 * whose arenas net games do not carry yet
 */
char *netLobbyMenuTextOnline(struct menuitem *item)
{
	if (g_GexPlusMode) {
		return "Online Game (Perfect Dark only)\n";
	}

	return netLobbyAvailable() ? "Online Game\n" : "Online Game (sign in first)\n";
}

MenuItemHandlerResult netLobbyMenuHandlerOnline(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !netLobbyAvailable() || g_GexPlusMode;
	}

	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetOnlineMenuDialog);
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
			rosterFit();
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
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_CUSTOMRENDER, 304, ROOMROWH * 5, handlerRoomList },
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

		if (!s_Create.name[0]) {
			snprintf(s_Create.name, sizeof(s_Create.name), "%.12s's Room", netLobbyAccount());
		}

		// the room's host plays: player 1 of the setup
		g_MpSetup.chrslots |= 1;
	}

	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		listClamp(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[5], ROOMROWH);
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		if (s_WaitingForRoom && netLobbyInRoom()) {
			s_WaitingForRoom = 0;
			rosterFit();
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
static MenuItemHandlerResult handlerGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 2;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 1 ? "Co-op Mission" : "Combat Simulator");
	case MENUOP_SET:
		g_NetCoopSetup.on = data->dropdown.value == 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_NetCoopSetup.on ? 1 : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerMission(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = SOLOSTAGEINDEX_WAR + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)langGet(g_SoloStages[data->dropdown.value <= SOLOSTAGEINDEX_WAR ? data->dropdown.value : 0].name3);
	case MENUOP_SET:
		g_NetCoopSetup.stageindex = data->dropdown.value <= SOLOSTAGEINDEX_WAR ? data->dropdown.value : 0;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_NetCoopSetup.stageindex;
		break;
	}

	return 0;
}

static MenuItemHandlerResult handlerDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const u16 names[3] = { L_OPTIONS_251, L_OPTIONS_252, L_OPTIONS_253 }; // "Agent", "Special Agent", "Perfect Agent"

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetCoopSetup.on;
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

#define NETLOBBY_SETUP_ITEMS \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Game", 0, handlerGame }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_019, (uintptr_t)&textScenario, handlerSetupScenario }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_020, (uintptr_t)&textArena, handlerSetupArena }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_023, 0, handlerSetupWeapons }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_024, 0, handlerSetupLimits }, \
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_MPMENU_025, 0, handlerSetupSimulants }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Mission", 0, handlerMission }, \
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, handlerDifficulty }, \
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Radar", 0, handlerCoopRadar }, \
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Friendly Fire", 0, handlerCoopFriendlyFire }

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
 */

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

/**
 * A member's way to the host and its ping: LAN, DIR (the host reachable),
 * NAT (punched through both NATs), RLY (through the lobby's relay), as the
 * member measured it; nothing on the host's own row (the settings line
 * above the roster says "Host: name")
 */
static void rosterNet(const struct netlobbymember *m, char *out, s32 size)
{
	const char *tag = "";

	// the host's row has no path: the line above the roster names it
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
		snprintf(out, size, "NO PATH");
		return;
	} else {
		snprintf(out, size, "...");
		return;
	}

	if (m->ping >= 0) {
		snprintf(out, size, "%s %d", tag, m->ping > 999 ? 999 : m->ping);
	} else {
		snprintf(out, size, "%s", tag);
	}
}

static s32 rosterRows(void)
{
	s32 rows = 1;
	s32 c;

	for (c = 0; c < 3; c++) {
		s32 r = 0;

		while (rosterCell(c, r)) {
			r++;
		}

		if (r > rows) {
			rows = r;
		}
	}

	return rows;
}

static MenuItemHandlerResult handlerRoster(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menuitemrenderdata *rd;
	Gfx *gdl;
	char buf[48];
	u32 colour;
	s32 index;
	s32 c;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// shown, never focused: the buttons below are what the cursor is for
		return 1;
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = rosterRows() + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)"";
	case MENUOP_GETOPTIONHEIGHT:
		data->list.value = ROWH;
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
	case MENUOP_RENDER:
		gdl = data->type19.gdl;
		rd = data->type19.renderdata2;
		index = (s32)data->type19.unk04;
		// a disabled item is drawn dimmed: the roster keeps a bright colour
		colour = 0x60f0f000 | (rd->colour & 0xff);
		gdl = text0f153628(gdl);

		if (index == 0) {
			static const char *heads[3] = { "TEAM 1:", "TEAM 2:", "SPECTATORS:" };

			for (c = 0; c < 3; c++) {
				s32 n = 0;

				while (rosterCell(c, n)) {
					n++;
				}

				snprintf(buf, sizeof(buf), "%s %d", heads[c], n);
				gdl = drawCell(gdl, rd, 4 + c * ROSTER_COLW, buf, 16, headerColour(colour));
			}
		} else {
			for (c = 0; c < 3; c++) {
				const struct netlobbymember *m = rosterCell(c, index - 1);

				if (m) {
					const u32 col = m->ready || m->host ? colour : dimColour(colour);

					char tag[16];
					s32 tagw;

					// the path and ping at the column's right, the name cut to what is left
					rosterNet(m, tag, sizeof(tag));
					tagw = cellWidth(tag);
					snprintf(buf, sizeof(buf), "%s%s", m->ready || m->host ? "* " : "  ", m->user);
					cellFitDots(buf, sizeof(buf), ROSTER_COLW - 6 - tagw - (tagw ? 4 : 0));
					gdl = drawCell(gdl, rd, 4 + c * ROSTER_COLW, buf, 20, col);
					gdl = drawCell(gdl, rd, 4 + c * ROSTER_COLW + ROSTER_COLW - 6 - tagw, tag, 15, dimColour(colour));
				}
			}
		}

		gdl = text0f153780(gdl);
		return (uintptr_t)gdl;
	}

	return 0;
}

static char *textRoomTitle(struct menuitem *item)
{
	static char text[128];
	const struct netlobbyroom *room = netLobbyGetRoom();

	if (!room->valid) {
		return "Waiting for the room...\n";
	}

	// the arena and mode are on the line below: a 32-character name with
	// them ran off the dialog's right edge
	snprintf(text, sizeof(text), "%s  (%d/%d)\n", room->sum.name, room->sum.humans, room->sum.maxhumans);

	return text;
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

static char *textRoomSettings(struct menuitem *item)
{
	static char texts[3][160];
	char *text = texts[item->param % 3];
	const struct netlobbyroom *room = netLobbyGetRoom();
	const char *time = ruleValue("time_limit");
	const char *score = ruleValue("score_limit");
	const char *weapons = ruleValue("weapons");

	// one line each (item->param): a label that grows a line after the
	// dialog is laid out pushes the rows above it under the title bar
	if (item->param == 2) {
		// a label does not widen the dialog (CLAUDE-notes/text-rendering.md):
		// each value cut to what is left of the line, the host's name kept
		char arena[64];
		char mode[48];
		char host[48];
		char tail[128];

		snprintf(host, sizeof(host), "%s", room->sum.host[0] ? room->sum.host : "-");
		labelFitDots(host, sizeof(host), 96);
		snprintf(mode, sizeof(mode), "%s", room->sum.scenario[0] ? room->sum.scenario : "-");
		labelFitDots(mode, sizeof(mode), 64);
		snprintf(tail, sizeof(tail), "   Mode: %s   Host: %s", mode, host);
		snprintf(arena, sizeof(arena), "%s", room->sum.stage[0] ? room->sum.stage : "-");
		labelFitDots(arena, sizeof(arena), LABEL_WIDTH - labelWidth("Arena: ") - labelWidth(tail));
		snprintf(text, sizeof(texts[0]), "Arena: %s%s\n", arena, tail);
	} else if (item->param == 1) {
		snprintf(text, sizeof(texts[0]), "Weapons: %s\n", weapons ? weapons : "-");
	} else {
		snprintf(text, sizeof(texts[0]), "Time: %s%s  Score: %s  Simulants: %d\n",
				time && strcmp(time, "0") ? time : "none", time && strcmp(time, "0") ? " min" : "",
				score && strcmp(score, "0") ? score : "none", room->sum.sims);
	}

	return text;
}

static char *textChatLine(struct menuitem *item)
{
	static char text[CHAT_LINES][160];
	const struct netlobbyroom *room = netLobbyGetRoom();
	const s32 line = item->param;
	const s32 first = room->nchat > CHAT_LINES ? room->nchat - CHAT_LINES : 0;
	const s32 i = first + line;

	if (i < room->nchat) {
		snprintf(text[line], sizeof(text[0]), "%s: %s\n", room->chat[i].user, room->chat[i].text);
	} else if (line == 0 && room->nchat == 0) {
		snprintf(text[line], sizeof(text[0]), "%s", "(no chat yet)\n");
	} else {
		snprintf(text[line], sizeof(text[0]), "%s", " \n");
	}

	return text[line];
}

static char *textRoomStatus(struct menuitem *item)
{
	const struct netlobbyroom *room = netLobbyGetRoom();

	switch (netLobbyLaunchState()) {
	case 1:
		snprintf(s_Status, sizeof(s_Status), "Launching in %d...\n", (netLobbyCountdownMs() + 999) / 1000);
		break;
	case 2:
		snprintf(s_Status, sizeof(s_Status), "%s", netLobbyIsHost() ? "Launched: waiting for the players to connect...\n" : "Launched: connecting to the host...\n");
		break;
	case 3:
		snprintf(s_Status, sizeof(s_Status), "%s", "In the match.\n");
		break;
	default:
		if (netLobbyMessage()[0]) {
			snprintf(s_Status, sizeof(s_Status), "%s\n", netLobbyMessage());
		} else if (netLobbyIsHost()) {
			snprintf(s_Status, sizeof(s_Status), "%s", "LAUNCH when everyone is ready (* = ready).\n");
		} else {
			snprintf(s_Status, sizeof(s_Status), "%s", netLobbyMyReady() ? "Ready: waiting for the host to launch.\n" : "Select READY when you are.\n");
		}
		break;
	}

	return s_Status;
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
		netLobbyChat(s_ChatLine);
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

static MenuItemHandlerResult handlerLeave(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netLobbyLeave();
		menuPopDialog();
	}

	return 0;
}

/**
 * A list keeps its focused row in the middle of its box; the roster is
 * never focused, so its rows would start half way down. Its scroll is held
 * at the top instead (the box's half height is where row 0 sits flush).
 */
static void rosterPinTop(struct menudialog *dialog, struct menuitem *item)
{
	struct menu *menu = &g_Menus[g_MpPlayerNum];
	s32 col;
	s32 j;

	for (col = dialog->colstart; col < dialog->colstart + dialog->numcols; col++) {
		for (j = 0; j < menu->cols[col].numrows; j++) {
			const s32 row = menu->cols[col].rowstart + j;

			if (&dialog->definition->items[menu->rows[row].itemindex] == item && menu->rows[row].blockindex != -1) {
				union menuitemdata *d = (union menuitemdata *)&menu->blocks[menu->rows[row].blockindex];
				const s32 view = d->list.viewheight > 0 ? d->list.viewheight : ROSTER_HEIGHT;

				d->list.curoffsety = d->list.targetoffsety = (view / 2) / ROWH * ROWH;
				return;
			}
		}
	}
}

static MenuDialogHandlerResult dialogRoom(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		netLobbyClearMessage();
		s_Status[0] = '\0';
	}

	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		rosterPinTop(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[4]);
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		// the seat went (kicked, the room closed): back to where it was
		// opened from, the reason on the status line there
		if (!netLobbyInRoom()) {
			menuPopDialog();
		}
	}

	return 0;
}

static struct menuitem s_RoomItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&textRoomTitle, 0, NULL },
	{ MENUITEMTYPE_LABEL, 2, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textRoomSettings, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textRoomSettings, 0, NULL },
	{ MENUITEMTYPE_LABEL, 1, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textRoomSettings, 0, NULL },
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_CUSTOMRENDER, 304, ROSTER_HEIGHT, handlerRoster },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textChatLine, 0, NULL },
	{ MENUITEMTYPE_LABEL, 1, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textChatLine, 0, NULL },
	{ MENUITEMTYPE_LABEL, 2, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textChatLine, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)&textRoomStatus, 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textReady, 0, handlerReady },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Change Team\n", 0, handlerTeam },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Chat...\n", 0, handlerChat },
	{ MENUITEMTYPE_SELECTABLE, 1, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Kick...\n", 0, handlerHostOnly },
	{ MENUITEMTYPE_SELECTABLE, 2, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Settings...\n", 0, handlerHostOnly },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Leave\n", 0, handlerLeave },
	{ MENUITEMTYPE_END },
};

/**
 * Phase 8: the roster box as tall as the room needs, set before the Game
 * Lobby is pushed (a dialog lays its items out then): three rows for a room
 * of up to six, as before, and up to six for twelve in two team columns. A
 * box taller than three rows makes the dialog scroll, so only a big room
 * pays for it.
 */
static void rosterFit(void)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	s32 rows = room && room->valid ? (room->sum.maxhumans + 1) / 2 : 3;

	rows = rows < 3 ? 3 : rows > 6 ? 6 : rows;
	s_RoomItems[4].param3 = rows == 3 ? ROSTER_HEIGHT : ROWH * (rows + 1);
}

struct menudialogdef g_NetRoomMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Game Lobby",
	s_RoomItems,
	dialogRoom,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK,
	NULL,
};

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
	{ MENUITEMTYPE_LIST, 0, 0, 160, 66, handlerKickList },
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
	}

	if (operation == MENUOP_CLOSE) {
		if (memcmp(&s_SettingsSetup, &g_MpSetup, sizeof(s_SettingsSetup)) != 0
				|| memcmp(s_SettingsBots, g_BotConfigsArray, sizeof(s_SettingsBots)) != 0
				|| memcmp(&s_SettingsCoop, &g_NetCoopSetup, sizeof(s_SettingsCoop)) != 0) {
			netLobbySendSettings();
		}
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

	rosterFit();

	if (g_Menus[g_MpPlayerNum].curdialog) {
		menuPushDialog(&g_NetRoomMenuDialog);
	} else {
		menuPushRootDialog(&g_NetRoomMenuDialog, MENUROOT_MPSETUP);
	}
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
