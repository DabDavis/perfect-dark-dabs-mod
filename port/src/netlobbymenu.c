#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
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
#include "game/file.h"
#include "game/tex.h"
#include "lib/mtx.h"
#include "game/modeldef.h"
#include "game/texdecompress.h"
#include "game/savebuffer.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "lib/model.h"
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
#include "input.h"
#include "menuimage.h"
#include "xblamesh.h"
#include "config.h"
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
extern struct menudialogdef g_MpPlayerSetupViaQuickGoMenuDialog;
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

static u32 headerColour(u32 colour)
{
	return 0xffc04000 | (colour & 0xff);
}

static u32 dimColour(u32 colour)
{
	return 0x80808000 | (colour & 0xff);
}

/*
 * Windows: the online pages as windows in two columns (2026-10-11, after
 * the owner's mock-up of the Game Lobby). Each page is one real dialog -
 * the buttons, or the list and its buttons - in one column, so the pad, the
 * keys and the mouse work it as any page, and windows drawn around it in
 * its own chrome and colours, after it (netLobbyMenuRenderPanels(), from
 * menuRenderDialog()), never focused. s_LobbyPages at the end of the file
 * says which page does what.
 *
 * The menu is 320x220 units drawn at that shape in the middle of the
 * window. The left column is held to the window's left edge and the right
 * column to its right (G_ASPECT_LEFT_EXT and RIGHT_EXT, as Ghost Trials'
 * plaques), each the wider by the screen beside the frame, so on a
 * widescreen display the windows fill its width. The frame is wider than
 * 4:3, so on a 4:3 window that "pillar" is negative and the columns are the
 * narrower (lobbyPillar). Neither column's coordinates leave 0-320 in its
 * own side's space, which is as far as text goes (text0f15568c() drops a
 * glyph past viGetWidth()). WIDE slows the offset past 16:9, as for the
 * plaques: an ultrawide display has the windows in from its edges.
 */

#define LOBBY_MARGIN  8  // from the screen's sides
#define LOBBY_GAP     6  // between the columns
#define LOBBY_VGAP    4  // between a column's windows
#define LOBBY_INSET   4  // a window's text from its sides
#define LOBBY_XSROW   lobbyRow(8)  // a line of the small font (HandelGothicXs)
#define LOBBY_SMROW   lobbyRow(10) // a line of HandelGothicSm
#define LOBBY_MDROW   lobbyRow(14) // a line of HandelGothicMd
#define LOBBY_LEFTPCT 60 // the left column's share of the width

#define LOBBYFONT_XS 0
#define LOBBYFONT_SM 1
#define LOBBYFONT_MD 2

/**
 * The text size (the owner, 2026-10-11: "letter glyphs are large"). The menu
 * is 320x220 units at every window size, and its fonts are as many units tall
 * as their glyphs are texels, so a line of HandelGothicSm is a 22nd of the
 * screen whatever the resolution. The windows' text is drawn smaller than its
 * glyphs instead (gDPSetRectScaleEXT: the renderer spreads a glyph's texels
 * over a smaller rectangle - the game's own font, and the XBLA release's
 * sharper one where XBLA Font is on), and the windows' lines and cards are as
 * tall as the smaller text (lobbyRow). Net.LobbyTextScale, percent; 100 is the
 * fonts' own size. The header and the footer keep the full size.
 */
static s32 s_LobbyTextScale = 75;
static s32 s_LobbyFullSize = 0; // while the header and the footer are drawn

static s32 lobbyPct(void)
{
	return s_LobbyFullSize ? 100 : s_LobbyTextScale < 50 ? 50 : s_LobbyTextScale > 100 ? 100 : s_LobbyTextScale;
}

static s32 lobbyRow(s32 h)
{
	return (h * lobbyPct() + 99) / 100;
}

s32 netLobbyMenuGetTextScale(void)
{
	return s_LobbyTextScale < 50 ? 50 : s_LobbyTextScale > 100 ? 100 : s_LobbyTextScale;
}

void netLobbyMenuSetTextScale(s32 pct)
{
	s_LobbyTextScale = pct < 50 ? 50 : pct > 100 ? 100 : pct;
}

static const struct lobbypage *lobbyPageOf(struct menudialogdef *def);
static Gfx *lobbyChatLines(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2,
		const struct netlobbychat *chat, s32 nchat, const char *empty, const char *status);

/**
 * The online pages' own dialogs draw their stock items - Create Room's
 * setup, the Game Lobby's buttons, Name Entry's Next and Back - at the
 * windows' text size too (menu.c: menuCalculateItemSize(), dialogRender()).
 * Not the lists drawn here, whose rows are sized and scaled already (the
 * Briefing Room's cards, Name Entry's box), and not the stock pages pushed
 * over them (an arena's list, the weapons), which are the game's own.
 */
s32 netLobbyMenuItemPct(struct menudialog *dialog, struct menuitem *item)
{
	if (dialog == NULL || dialog->definition == NULL || lobbyPageOf(dialog->definition) == NULL) {
		return 100;
	}

	if (item && item->type == MENUITEMTYPE_LIST && (item->flags & MENUITEMFLAG_LIST_CUSTOMRENDER)) {
		return 100;
	}

	return lobbyPct();
}

struct lobbylayout {
	s32 pillar;    // the screen past the menu's 320-unit frame on each side, in menu units (negative at 4:3)
	s32 headery1;  // the header bar across the top (the logo, the page's title)
	s32 headery2;
	s32 top;       // the windows, between the header and the footer
	s32 bottom;
	s32 footery;   // the footer's line of hints
	s32 lx1;       // the left column, in its (left-held) space
	s32 lx2;
	s32 rx1;       // the right column, in its (right-held) space
	s32 rx2;
};

#define LOBBY_HEADERH 26 // the header bar
#define LOBBY_FOOTERH 11 // the footer's line

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
 * The two columns: LOBBY_MARGIN in from the screen's sides at any aspect (a
 * 4:3 window has dialogs about as far in: the frame's 20-unit margin less
 * the 13 of it off the screen), the height the menu's own
 * (menuFindAvailableSize). Each page splits its columns into windows.
 */
static void lobbyLayout(struct lobbylayout *l)
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

	// the header bar, the windows, the footer's hints (the owner's WWE
	// SmackDown! vs. Raw reference, 2026-10-11: a bar with the logo and
	// the page's name across the top, the buttons' hints along the foot)
	l->headery1 = ymin;
	l->headery2 = ymin + LOBBY_HEADERH;
	l->top = l->headery2 + LOBBY_VGAP;
	l->footery = ymax - LOBBY_FOOTERH + 2;
	l->bottom = ymax - LOBBY_FOOTERH;

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
}

/**
 * A page's buttons as a box at the foot of the right column, as tall as
 * its rows (at most 3/5 of the column): the dialog's y and height. The box
 * starts LINEHEIGHT below its y (no title bar).
 */
static void lobbyButtonsAtFoot(const struct lobbylayout *l, s32 contentheight, s32 *y, s32 *h)
{
	*h = contentheight;

	if (*h > (l->bottom - l->top) * 3 / 5) {
		*h = (l->bottom - l->top) * 3 / 5;
	}

	*y = l->bottom - *h;
}

static struct fontchar *lobbyChars(s32 font)
{
	return font == LOBBYFONT_MD ? g_CharsHandelGothicMd : font == LOBBYFONT_SM ? g_CharsHandelGothicSm : g_CharsHandelGothicXs;
}

static struct font *lobbyFont(s32 font)
{
	return font == LOBBYFONT_MD ? g_FontHandelGothicMd : font == LOBBYFONT_SM ? g_FontHandelGothicSm : g_FontHandelGothicXs;
}

// as drawn at the font's own size
static s32 lobbyTextWidthFull(const char *text, s32 font)
{
	s32 w = 0;
	s32 h = 0;

	textMeasure(&h, &w, (char *)text, lobbyChars(font), lobbyFont(font), 0);

	return w;
}

static s32 lobbyTextWidth(const char *text, s32 font)
{
	return (lobbyTextWidthFull(text, font) * lobbyPct() + 99) / 100;
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

	if (lobbyPct() != 100) {
		// The line laid out at full size and drawn scaled about its start.
		// A glyph past the edge of the 320 units is dropped before it is
		// drawn (text0f15568c()), so a line whose full size would cross it -
		// one held to a window's right side - is scaled about its end
		// instead: laid out leftwards from there, its end where it is.
		const f32 scale = lobbyPct() / 100.0f;
		const s32 full = lobbyTextWidthFull(buf, font);
		f32 ox = x;

		if (x + full > viGetWidth()) {
			ox = x + full * scale;
			x = (s32)(ox + 0.5f) - full;
		}

		gDPSetRectScaleEXT(gdl++, scale, ox, y);
		gdl = textRenderProjected(gdl, &x, &y, buf, lobbyChars(font), lobbyFont(font), colour, viGetWidth(), viGetHeight(), 0, 0);
		gDPSetRectScaleEXT(gdl++, 1.0f, 0, 0);

		return gdl;
	}

	return textRenderProjected(gdl, &x, &y, buf, lobbyChars(font), lobbyFont(font), colour, viGetWidth(), viGetHeight(), 0, 0);
}

/**
 * A line at the windows' text size longer than the 320 units the text
 * renderer keeps its glyphs inside (text0f15568c() drops one laid out past
 * them, before the scale): drawn in pieces, split at spaces, each scaled
 * about the point that puts it where the last one ended. That point is a
 * signed 12 bits of quarter units in G_SETRECTSCALE_EXT (-512 to 511), so a
 * piece far along is laid out further right, and the line ends where the
 * furthest point reaches (about 364 units at 75%). Cut to width (as drawn) first; at 100% only what is inside
 * the 320 units.
 */
static Gfx *lobbyTextLong(Gfx *gdl, s32 x, s32 y, const char *text, s32 font, u32 colour, s32 width)
{
	char buf[NETLOBBY_MAXCHATTEXT + 64];
	const s32 pct = lobbyPct();
	const f32 k = pct / 100.0f;
	const f32 omax = 508.0f;
	const s32 lmax = SCREEN_WIDTH_LO - 4;
	f32 sx = x;
	char *p;

	if (pct == 100) {
		return lobbyText(gdl, x, y, text, font, colour, MIN(width, lmax - x));
	}

	// a piece ending at lmax laid out, scaled about omax, ends here
	width = MIN(width, (s32)(omax * (1.0f - k) + lmax * k) - x);

	snprintf(buf, sizeof(buf), "%s", text);
	lobbyFit(buf, sizeof(buf), width, font);
	p = buf;

	while (*p) {
		char piece[sizeof(buf)];
		// as far left as the point allows (under omax), and not left of x
		const s32 lx0 = MAX(x, (s32)((sx - omax * (1.0f - k)) / k + 1.0f));
		s32 n = strlen(p);
		s32 lx = lx0;
		s32 ly = y;
		s32 w;

		if (lmax - lx0 < 8) {
			break;
		}

		// as much as fits laid out from there, ended after a space where
		// there is one
		snprintf(piece, sizeof(piece), "%s", p);

		while (n > 1 && lobbyTextWidthFull(piece, font) > lmax - lx0) {
			piece[--n] = '\0';
		}

		if (n < (s32)strlen(p)) {
			s32 cut = n;

			while (cut > 0 && piece[cut - 1] != ' ') {
				cut--;
			}

			if (cut > 0) {
				n = cut;
				piece[n] = '\0';
			}
		}

		w = lobbyTextWidthFull(piece, font);
		gDPSetRectScaleEXT(gdl++, k, (sx - k * lx0) / (1.0f - k), y);
		gdl = textRenderProjected(gdl, &lx, &ly, piece, lobbyChars(font), lobbyFont(font), colour, viGetWidth(), viGetHeight(), 0, 0);
		sx += w * k;
		p += n;
	}

	gDPSetRectScaleEXT(gdl++, 1.0f, 0, 0);

	return gdl;
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

/**
 * What a page's windows are drawn with: its dialog (the colours, the text
 * effects, its state), whether the dialog is open far enough for text, how
 * far it has grown open, and how far it has slid from its place
 */
struct lobbydraw {
	struct menudialog *dialog;
	s32 withtext;
	f32 grow;
	s32 dx;
};

typedef Gfx *(*lobbycontent)(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2);

#define LOBBY_MAXSETTINGS 8

// a line of a window's settings: "Arena: Skedar"
struct lobbysetting {
	const char *label;
	char value[48];
};

// drawn the same in more than one page's windows (the sections below)
static const char *roomMode(const struct netlobbyroomsum *r);
static const char *ruleValueOf(const struct netlobbyroom *room, const char *key);
static Gfx *lobbyTitleLine(Gfx *gdl, const char *name, const char *tail, s32 tx, s32 *y, s32 inner, u32 colour);
static Gfx *lobbySettingsBlock(Gfx *gdl, struct menudialog *dialog, const struct netlobbyroom *room, s32 tx, s32 y, s32 inner, s32 maxrows, s32 *rows);
static Gfx *lobbyPairsBlock(Gfx *gdl, struct menudialog *dialog, const struct lobbysetting *settings, s32 nsettings,
		s32 tx, s32 y, s32 inner, s32 maxrows, s32 *rows);
/**
 * A setting's width as lobbyPairsBlock draws it: the label with its ": "
 * and the value apart, each rounded at the text size (measured whole, the
 * pair came out a unit narrower than its two parts and a short value was
 * cut to "..": F3 20261011-214715, "Simulants: ..")
 */
static s32 lobbyPairWidth(const struct lobbysetting *setting)
{
	char buf[128];

	snprintf(buf, sizeof(buf), "%s: ", setting->label);

	return lobbyTextWidth(buf, LOBBYFONT_XS) + lobbyTextWidth(setting->value, LOBBYFONT_XS);
}

static s32 lobbyPairsCols(const struct lobbysetting *settings, s32 nsettings, s32 inner, s32 maxrows);
static Gfx *lobbyModsBlock(Gfx *gdl, struct menudialog *dialog, const struct netcontentrow *rows, s32 nrows,
		s32 tx, s32 y, s32 inner, s32 fit, s32 empties);

// The width to wrap at for a window's inner width: the font's own measure
// (textWrapN()), the text drawn at the windows' size
static s32 lobbyWrapWidth(s32 inner)
{
	return (inner - 8) * 100 / lobbyPct();
}

// the lines text takes wrapped to a window's width in the small font
static s32 lobbyParagraphLines(const char *text, s32 inner)
{
	char in[512];
	char out[640];
	s32 n = 0;
	char *p;

	snprintf(in, sizeof(in), "%s\n", text);
	textWrapN(lobbyWrapWidth(inner), in, out, sizeof(out), g_CharsHandelGothicXs, g_FontHandelGothicXs);

	for (p = out; *p; p++) {
		n += *p == '\n';
	}

	return n > 0 ? n : 1;
}

/**
 * A status window's height for its text (the panels' bottom windows): the
 * lines it wraps to at this width, and a gap between paragraphs
 */
static s32 lobbyStatusHeight(const struct lobbylayout *l, const char *a, const char *b)
{
	const s32 inner = l->rx2 - l->rx1 - 2 * LOBBY_INSET;
	const s32 lines = lobbyParagraphLines(a, inner) + (b ? lobbyParagraphLines(b, inner) : 0);

	return LINEHEIGHT + 3 + lines * LOBBY_XSROW + (b ? 2 : 0) + 5;
}

/**
 * Text wrapped to a window's width in the small font, at most maxlines of
 * it, from *y on (moved past it)
 */
static Gfx *lobbyParagraph(Gfx *gdl, const char *text, s32 tx, s32 *y, s32 inner, u32 colour, s32 maxlines)
{
	char in[512];
	char out[640];
	char *p = out;
	s32 n = 0;

	snprintf(in, sizeof(in), "%s\n", text);
	textWrapN(lobbyWrapWidth(inner), in, out, sizeof(out), g_CharsHandelGothicXs, g_FontHandelGothicXs);

	while (*p && n < maxlines) {
		char *end = strchr(p, '\n');

		if (end) {
			*end = '\0';
		}

		gdl = lobbyText(gdl, tx, *y, p, LOBBYFONT_XS, colour, inner);
		*y += LOBBY_XSROW;
		n++;

		if (!end) {
			break;
		}

		p = end + 1;
	}

	return gdl;
}

/**
 * One window of a page, in the space of the side it is held to (lobbyAlign
 * first): its frame grown open with the dialog and slid with it, its title,
 * and once the dialog is open its content
 */
static Gfx *lobbyPanel(Gfx *gdl, const struct lobbydraw *d, s32 x1, s32 y1, s32 x2, s32 y2, const char *title, lobbycontent content)
{
	x1 += d->dx;
	x2 += d->dx;
	y2 = y1 + (s32)((y2 - y1) * d->grow);

	gdl = lobbyWindow(gdl, d->dialog, x1, y1, x2, y2, (char *)title, d->withtext);

	if (d->withtext && content) {
		gdl = text0f153628(gdl);
		textSetWaveColours(g_MenuWave2Colours[d->dialog->type].item_unfocused, g_MenuWave1Colours[d->dialog->type].item_unfocused);
		lobbyWindowBlend(d->dialog, x1, y1);
		gdl = content(gdl, d->dialog, x1, y1, x2, y2);
		gdl = text0f153780(gdl);
	}

	return gdl;
}

/*
 * The header and the footer (the owner's WWE SmackDown! vs. Raw reference,
 * 2026-10-11: a bar across the top with a logo badge and the page's name,
 * the buttons' hints along the foot).
 */

PD_CONSTRUCTOR static void lobbyConfigInit(void)
{
	configRegisterInt("Net.LobbyTextScale", &s_LobbyTextScale, 50, 100);
}

/**
 * The header's logo: the boot screen's last frame (title.c's
 * titleRenderPdLogo()), Perfect Dark's emblem with PERFECT over DARK across
 * it - the owner's choice over the port's icon and "PD / Online", and drawn
 * from the ROM rather than shipped. The ROM's own two models in 3D, through
 * the menus' model renderer the way the Ghost Trials nameplate draws its
 * head (ghostmenu.c), each with memory and a viewport of its own: the menus'
 * are the pages' character previews'.
 *
 * Both are the title's morphs at their end, which is the shape under their
 * second toggle as it stands (titleRenderPdLogoModel() at frac 1 draws the
 * first toggle's lists through the second's vertices; the words' end, arg2
 * false, is the second toggle itself): the first toggle off, the second on.
 * The emblem is lit flat, as the title's front face is once the other sides
 * have darkened (those are edge on or behind it here), the words by the light
 * where the title's comes to rest, and both reflect as under the title's
 * LookAt, a camera on the z axis.
 *
 * The title's camera is 46 degrees high at 4000 units, the emblem at scale
 * 0.35 and the words 1000 units nearer at 0.308, grown 5% by the end: the
 * words' projected scale is 1.23 times the emblem's, and the emblem is 340 of
 * 720 lines, 4580 of its model's units tall. Here both stand at the menu
 * models' depth, the emblem as tall as the rectangle, and the depth is
 * cleared between the two, which is what the title's 1000 units are for: the
 * words in front.
 */
#define LOBBY_LOGO_ASPECT  2.376f  // the boot screen's logo, 808 x 340 at 720p
#define LOBBY_LOGO_EMBLEMH 4580.0f // the emblem's height in its model's units
#define LOBBY_LOGO_WORDS   1.232f  // the words' projected scale over the emblem's
#define LOBBY_LOGO_TEXMEM  0x40000 // each model's textures, after its file

extern u8 g_ViBackIndex; // vi.c

static struct menumodel s_LogoModels[2];
static Vp s_LogoViewport[2]; // vi.c's double buffer: the frame's back index
static LookAt s_LogoLookAt;
static s32 s_LogoLookSet = 0;
static const u16 s_LogoFiles[2] = { FILE_PNLOGO, FILE_PPDTWO };

static struct modelpartvisibility s_LogoParts[] = {
	{ MODELPART_LOGO_0000, false },
	{ MODELPART_LOGO_0001, true },
	{ 255, false },
};

static Lights1 s_LogoLightFlat = gdSPDefLights1(0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
static Lights1 s_LogoLightWords = gdSPDefLights1(0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x39, 0x71);

static struct menumodel *lobbyLogoModel(s32 i)
{
	struct menumodel *m = &s_LogoModels[i];

	if (m->allocstart == NULL) {
		const u32 len = ALIGN64(fileGetInflatedSize(s_LogoFiles[i], LOADTYPE_MODEL)) + 0x4000 + LOBBY_LOGO_TEXMEM;
		// 64-byte aligned, as the model loader assumes (ghostmenu.c)
		u8 *mem = sysMemAlloc(len + 64);

		if (mem == NULL) {
			return NULL;
		}

		menuResetModel(m, len, false);
		m->allocstart = (u8 *)ALIGN64((uintptr_t)mem);
		m->alloclen = len;
	}

	return m;
}

// The title's lights and LookAt, set in front of the visible side's lists;
// the morph's first shape (Nintendo's cube, for the emblem) never drawn
static void lobbyLogoLight(struct menumodel *m, s32 words)
{
	static const u8 parts[] = { MODELPART_LOGO_0003, MODELPART_LOGO_0005, MODELPART_LOGO_0007, MODELPART_LOGO_0009 };
	static const u8 hidden[] = { MODELPART_LOGO_FRONTSIDE, MODELPART_LOGO_RIGHTSIDE, MODELPART_LOGO_BACKSIDE, MODELPART_LOGO_LEFTSIDE };
	s32 i;

	for (i = 0; i < ARRAYCOUNT(hidden); i++) {
		struct modelnode *node = modelGetPart(m->bodymodeldef, hidden[i]);
		Gfx *dl;

		if (node == NULL || (node->type & 0xff) != MODELNODETYPE_DL) {
			continue;
		}

		dl = gfxAllocate(sizeof(Gfx));
		gSPEndDisplayList(dl);
		((struct modelrwdata_dl *)modelGetNodeRwData(&m->bodymodel, node))->gdl = dl;
	}

	for (i = 0; i < ARRAYCOUNT(parts); i++) {
		struct modelnode *node = modelGetPart(m->bodymodeldef, parts[i]);
		struct modelrwdata_dl *rwdata;
		Gfx *start;
		Gfx *dl;

		if (node == NULL || (node->type & 0xff) != MODELNODETYPE_DL) {
			continue;
		}

		rwdata = modelGetNodeRwData(&m->bodymodel, node);
		start = dl = gfxAllocate(12 * sizeof(Gfx));

		// lit, as the title draws them: the lists' colours are normals,
		// and menuRenderModel()'s setup turns lighting off
		gSPSetGeometryMode(dl++, G_LIGHTING);

		// The lists blend the morph's two textures, Nintendo's cube's and
		// Perfect Dark's, by the primitive colour's alpha, which the title
		// sets for each model (titleRenderPdLogoModel()). These are the
		// lists of the morph's second toggle, whose alpha at the end is 256
		// less the cap: 16 for the emblem (capped at 240), 1 for the words
		// (255). Left to the menu's, the emblem came up in Nintendo's green
		// and blue whenever the last colour set had some alpha (an open
		// dropdown's).
		gDPSetPrimColor(dl++, 0, 0, 0x00, 0x00, 0x00, words ? 1 : 16);

		if (words) {
			gSPSetLights1(dl++, s_LogoLightWords);
		} else {
			gSPSetLights1(dl++, s_LogoLightFlat);
		}

		gSPLookAt(dl++, &s_LogoLookAt);
		gSPBranchList(dl++, node->rodata->dl.opagdl);

		rwdata->gdl = start;
	}
}

// The model in its own memory, loaded as menuRenderModel() loads a file
static s32 lobbyLogoLoad(struct menumodel *m, u16 filenum)
{
	struct texpool texpool;
	u32 filelen;

	if (m->bodymodeldef != NULL) {
		return 1;
	}

	filelen = ALIGN64(fileGetInflatedSize(filenum, LOADTYPE_MODEL)) + 0x4000;
	videoFreeCachedTextures(m->allocstart, m->allocstart + m->alloclen);
	texInitPool(&texpool, m->allocstart + filelen, m->alloclen - filelen);
	m->bodymodeldef = modeldefLoad(filenum, m->allocstart, filelen, &texpool);

	if (m->bodymodeldef == NULL) {
		return 0;
	}

	modelAllocateRwData(m->bodymodeldef);
	modelInit(&m->bodymodel, m->bodymodeldef, m->rwdata, true);
	animInit(&m->bodyanim);
	m->bodymodel.rwdatalen = ARRAYCOUNT(m->rwdata);
	m->bodymodel.anim = &m->bodyanim;

	return 1;
}

// One model, 100 units in front of the eye at `scale`, as menuRenderModel()
// draws a menu's model through the projection the caller has set
static Gfx *lobbyLogoDrawModel(Gfx *gdl, struct menumodel *m, s32 words, f32 scale)
{
	struct modelrenderdata renderdata = {NULL, true, 3};
	struct coord pos = { 0.0f, 0.0f, -100.0f };
	struct modelnode *node;
	Mtxf *matrices;
	s32 i;

	// the morph's end: its second toggle's shape (see above)
	if ((node = modelGetPart(m->bodymodeldef, MODELPART_LOGO_0000)) != NULL) {
		((union modelrwdata *)modelGetNodeRwData(&m->bodymodel, node))->toggle.visible = false;
	}

	if ((node = modelGetPart(m->bodymodeldef, MODELPART_LOGO_0001)) != NULL) {
		((union modelrwdata *)modelGetNodeRwData(&m->bodymodel, node))->toggle.visible = true;
	}

	lobbyLogoLight(m, words);

	matrices = gfxAllocate(m->bodymodeldef->nummatrices * sizeof(Mtxf));

	for (i = 0; i < m->bodymodeldef->nummatrices; i++) {
		mtx4LoadIdentity(&matrices[i]);
	}

	m->bodymodel.matrices = matrices;
	mtx4LoadTranslation(&pos, &m->mtx);
	mtx00015f04(scale, &m->mtx);
	mtx4Copy(&m->mtx, matrices);
	renderdata.unk00 = &m->mtx;
	renderdata.unk10 = m->bodymodel.matrices;
	modelSetMatricesWithAnim(&renderdata, &m->bodymodel);

	gSPDisplayList(gdl++, var80061380);
	gSPDisplayList(gdl++, var800613a0);
	gdl = menugfx0f0e2348(gdl);
	gSPSetGeometryMode(gdl++, G_ZBUFFER);

	renderdata.unk30 = 1;
	renderdata.envcolour = 0xffffffff;
	renderdata.fogcolour = 0xffffffff;
	renderdata.gdl = gdl;
	renderdata.zbufferenabled = true;
	modelRender(&renderdata, &m->bodymodel);
	gdl = renderdata.gdl;

	// the matrices into the RSP's form where they lie
	mtx00016760();

	for (i = 0; i < m->bodymodeldef->nummatrices; i++) {
		Mtxf tmp;

		mtx4Copy((Mtxf *)((uintptr_t)m->bodymodel.matrices + i * sizeof(Mtxf)), &tmp);
		mtxF2L(&tmp, &m->bodymodel.matrices[i]);
	}

	mtx00016784();

	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	return gdl;
}

/**
 * The logo in x1-y2 of the left-held space: the two models drawn by hand,
 * through a viewport and a projection of its own, a depth cleared for each
 * and the menus' own 2D projection set again after. Nothing of the view's,
 * the camera's or the menus' state is touched. menuRenderModel(), first used
 * here, sets the view to its rectangle and its projection as the camera's;
 * put back, the eye piece came up small in the logo's box or not at all,
 * and building the player's projection again rewrote the player's viewport,
 * which in the XBLA look drew the eye piece's mesh as a blue bar down the
 * screen and mapped the pointer through it (F3 20261011-203600).
 *
 * The vertices take a side's offset as a share of the viewport rather than
 * of the screen, so the models are drawn with the centre held and the
 * rectangle moved into the centre's frame by hand; the scissor is set with
 * no side held, in the stretched frame (one set with the centre held is
 * clamped at 0, and the logo stands in the 16:9 pillar).
 */
static Gfx *lobbyLogo(Gfx *gdl, const struct lobbylayout *l, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const s32 sx1 = g_MenuScissorX1;
	const s32 sy1 = g_MenuScissorY1;
	const s32 sx2 = g_MenuScissorX2;
	const s32 sy2 = g_MenuScissorY2;
	const f32 fovy = g_Vars.currentplayer->fovy;
	// the view's height at the models' depth, 100 in front
	const f32 viewh = 2.0f * tanf(fovy * (M_PI / 360.0f)) * 100.0f;
	// the rectangle in the centre's frame, squeezed into the stretched frame
	// as menuRenderModel() squeezes a dialog's for its viewport
	const f32 ratio = SCREEN_ASPECT / videoGetAspect();
	const f32 cx1 = SCREEN_WIDTH_LO / 2 + (x1 - l->pillar - SCREEN_WIDTH_LO / 2) * ratio;
	const f32 cx2 = SCREEN_WIDTH_LO / 2 + (x2 - l->pillar - SCREEN_WIDTH_LO / 2) * ratio;
	Vp *vp = &s_LogoViewport[g_ViBackIndex];
	Mtx *proj;
	Mtxf projf;
	u16 perspnorm;
	s32 i;

	if (!s_LogoLookSet) {
		static Mtx lookmtx;

		guLookAtReflect(&lookmtx, &s_LogoLookAt, 0.0f, 0.0f, 4000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
		s_LogoLookSet = 1;
	}

	// the scissor, with no side held
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	g_MenuScissorX1 = (s32)floorf(cx1);
	g_MenuScissorX2 = (s32)ceilf(cx2);
	g_MenuScissorY1 = y1;
	g_MenuScissorY2 = y2;
	gdl = menuApplyScissor(gdl);

	// the viewport and the projection, with the centre held
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	vp->vp.vscale[0] = (s16)((cx2 - cx1) * g_ScaleX * 2);
	vp->vp.vtrans[0] = (s16)((cx2 - cx1) * g_ScaleX * 2 + cx1 * g_ScaleX * 4);
	vp->vp.vscale[1] = (y2 - y1) * 2;
	vp->vp.vtrans[1] = (y2 - y1) * 2 + y1 * 4;
	vp->vp.vscale[2] = 511;
	vp->vp.vtrans[2] = 511;
	vp->vp.vscale[3] = 0;
	vp->vp.vtrans[3] = 0;
	gSPViewport(gdl++, OS_K0_TO_PHYSICAL(vp));

	proj = gfxAllocateMatrix();
	guPerspectiveF(projf.m, &perspnorm, fovy, (cx2 - cx1) / (f32)(y2 - y1), 10, 300, 1);
	*(Mtxf *)proj = projf;
	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(proj), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION | CAM_PROJ_MTX_FLAGS);
	gSPPerspNormalize(gdl++, perspnorm);

	for (i = 0; i < 2; i++) {
		struct menumodel *m = lobbyLogoModel(i);

		if (m == NULL) {
			continue;
		}

		// The N64's models whatever the look: in the XBLA look the release
		// has a mesh for the emblem's file, 4J's cube, drawn on the morph's
		// second toggle - the one shown here. The title keeps the release's
		// meshes off its logo the same way.
		xblaMeshSetBypass(true);

		if (lobbyLogoLoad(m, s_LogoFiles[i])) {
			// a depth of its own: the words over the emblem
			gDPClearDepthEXT(gdl++);
			gdl = lobbyLogoDrawModel(gdl, m, i == 1, viewh / LOBBY_LOGO_EMBLEMH * (i == 1 ? LOBBY_LOGO_WORDS : 1.0f));
		}

		xblaMeshSetBypass(false);
	}

	// The player's viewport and the menus' 2D projection again, as after a
	// dialog's model (menuRenderModel()), and the state it leaves
	gSPViewport(gdl++, OS_K0_TO_PHYSICAL(viGetCurrentPlayerViewport()));
	gdl = func0f0d479c(gdl);

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineMode(gdl++, G_CC_MODULATEI, G_CC_MODULATEI);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	texSelect(&gdl, NULL, 2, 0, 2, 1, NULL);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	texSelect(&gdl, NULL, 2, 0, 2, 1, NULL);
	gSPDisplayList(gdl++, var800613a0);

	g_MenuScissorX1 = sx1;
	g_MenuScissorY1 = sy1;
	g_MenuScissorX2 = sx2;
	g_MenuScissorY2 = sy2;

	return menuApplyScissor(gdl);
}

// The header's bar from x1 to x2: the dialogs' title bar, taller, its rails
// along the top and the foot
static Gfx *lobbyHeaderBar(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const u32 border1 = MIXCOLOUR(dialog, dialog_border1);
	const u32 titlebg = MIXCOLOUR(dialog, dialog_titlebg);
	const u32 border2 = MIXCOLOUR(dialog, dialog_border2);

	gdl = menugfxRenderGradient(gdl, x1, y1, x2, y2, border1, titlebg, border2);
	gdl = menugfxDrawShimmer(gdl, x1, y1, x2, y1 + 1, (border1 & 0xff) >> 1, 1, 40, 0);
	gdl = menugfxDrawShimmer(gdl, x1, y2 - 1, x2, y2, (border1 & 0xff) >> 1, 0, 40, 1);

	return gdl;
}

/**
 * The header across the whole screen - the emblem and the page's name on
 * the left, what the page is about (the room, the account) on the right -
 * and the footer's hints. The bar is one piece stretched across the window
 * (two halves held to the sides met in a seam); the emblem and the name are
 * held left, the information right.
 */
static Gfx *lobbyChromeDraw(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l, const char *title, const char *info,
		const char *hints, const char *footmsg)
{
	struct menudialog *dialog = d->dialog;
	const u32 alpha = (MIXCOLOUR(dialog, dialog_titlefg)) & 0xff;
	const u32 titlefg = MIXCOLOUR(dialog, dialog_titlefg);
	const s32 h = l->headery2 - l->headery1;
	const s32 y1 = l->headery1;
	const s32 y2 = l->headery1 + (s32)(h * d->grow);
	char buf[96];
	s32 titlex;
	s32 titleend;

	// the bar in one piece, with no side held: the 320 units then stretch
	// across the whole window, which a gradient does not mind (text would;
	// there is none in it). Its ends where the columns' outer edges are:
	// LOBBY_MARGIN in from the screen's sides, or past 16:9 from where WIDE
	// holds the columns in.
	{
		const f32 window = videoGetAspect();
		const f32 native = (f32)videoGetNativeWidth() / (f32)videoGetNativeHeight();
		const f32 wtrue = SCREEN_WIDTH_LO * window / native;
		const f32 held = (wtrue - (SCREEN_WIDTH_LO + 2 * l->pillar)) / 2 + LOBBY_MARGIN;
		const s32 bx1 = (s32)(held * SCREEN_WIDTH_LO / wtrue);

		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
		g_MenuScissorX1 = viGetViewLeft() / g_ScaleX;
		g_MenuScissorY1 = viGetViewTop();
		g_MenuScissorX2 = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
		g_MenuScissorY2 = viGetViewTop() + viGetViewHeight();
		gdl = menuApplyScissor(gdl);
		gdl = lobbyHeaderBar(gdl, dialog, bx1, y1, SCREEN_WIDTH_LO - bx1, y2);
	}

	if (!d->withtext) {
		return gdl;
	}

	// the left: the boot screen's logo over the bar's end, the page's name
	// after it
	{
		const s32 lh = h + 2;
		const s32 lw = (s32)(lh * LOBBY_LOGO_ASPECT);
		const s32 lx = LOBBY_MARGIN + 4;

		gdl = lobbyLogo(gdl, l, lx, y1 - 1, lx + lw, y1 - 1 + lh);
		titlex = lx + lw + 8;
	}

	gdl = lobbyAlign(gdl, G_ASPECT_LEFT_EXT);
	gdl = text0f153628(gdl);
	textSetWaveColours(g_MenuWave2Colours[dialog->type].dialog_titlefg, g_MenuWave1Colours[dialog->type].dialog_titlefg);

	gdl = lobbyText(gdl, titlex + 1, y1 + (h - LOBBY_MDROW) / 2 + 2, title, LOBBYFONT_MD, titlefg & 0xff, 0);
	gdl = lobbyText(gdl, titlex, y1 + (h - LOBBY_MDROW) / 2 + 1, title, LOBBYFONT_MD, titlefg, 0);
	titleend = titlex + lobbyTextWidth(title, LOBBYFONT_MD);

	// the footer's hints, at the foot of the left column; the page's message
	// in their place while it has one, in the notices' gold at the windows'
	// text size (lobbyTextLong()), as wide as the 320 units at any aspect (a
	// left-held glyph much past them was cut at 16:9)
	if (footmsg && footmsg[0]) {
		s_LobbyFullSize = 0;
		gdl = lobbyTextLong(gdl, LOBBY_MARGIN + 4, l->footery, footmsg, LOBBYFONT_XS, lobbyAlpha(0xffd25a00, alpha),
				SCREEN_WIDTH_LO - 2 * LOBBY_MARGIN - 4);
		s_LobbyFullSize = 1;
	} else if (hints && hints[0]) {
		gdl = lobbyText(gdl, LOBBY_MARGIN + 4, l->footery, hints, LOBBYFONT_XS, lobbyAlpha(0xc0c0c000, alpha), 0);
	}

	gdl = text0f153780(gdl);

	// the right: the page's information, as far right as the bar goes and
	// cut where it would run into the name (in the screen's own measure:
	// the right half is 2 pillars on)
	if (info && info[0]) {
		const s32 right = SCREEN_WIDTH_LO - LOBBY_MARGIN - 6;
		const s32 room = right + 2 * l->pillar - (titleend + 12);

		snprintf(buf, sizeof(buf), "%s", info);

		if (room > 20) {
			// what is cut is the part before the last gap ("Dabba's Room   10/12"
			// keeps its count at 4:3, the name shortened)
			char *tail = NULL;
			char *gap;

			for (gap = strstr(buf, "   "); gap; gap = strstr(gap + 3, "   ")) {
				tail = gap;
			}

			if (tail && lobbyTextWidth(buf, LOBBYFONT_SM) > room) {
				char last[48];
				const s32 lastw = lobbyTextWidth(tail, LOBBYFONT_SM);

				snprintf(last, sizeof(last), "%s", tail);
				*tail = '\0';

				if (room - lastw > 20) {
					lobbyFit(buf, sizeof(buf), room - lastw, LOBBYFONT_SM);
					strncat(buf, last, sizeof(buf) - strlen(buf) - 1);
				} else {
					snprintf(buf, sizeof(buf), "%s", last + 3);
				}
			}

			gdl = lobbyAlign(gdl, G_ASPECT_RIGHT_EXT);
			gdl = text0f153628(gdl);
			lobbyFit(buf, sizeof(buf), room, LOBBYFONT_SM);
			gdl = lobbyTextRight(gdl, right, y1 + (h - LOBBY_SMROW) / 2 + 1, buf, LOBBYFONT_SM, titlefg);
			gdl = text0f153780(gdl);
		}
	}

	return gdl;
}

// the header and the footer at the fonts' own size
static Gfx *lobbyChrome(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l, const char *title, const char *info,
		const char *hints, const char *footmsg)
{
	s_LobbyFullSize = 1;
	gdl = lobbyChromeDraw(gdl, d, l, title, info, hints, footmsg);
	s_LobbyFullSize = 0;

	return gdl;
}

/*
 * Online Game
 */

extern struct menudialogdef g_GhostAccountMenuDialog;

/*
 * Name Entry: the page Online Game opens on (the owner's WWE SmackDown! vs.
 * Raw reference, 2026-10-11: "the name screen, a screen before the main
 * online screen"). The name this game plays online as in a box of its own
 * and Next: the box opens the account page (ghostmenu.c, shared with Ghost
 * Trials), where a name and PIN are typed and one Sign In signs in or, for a
 * new name, makes the account; Next goes on to the Briefing Room once signed
 * in (signing in first when it is not). The page used to be a hub of four
 * buttons with the rooms beside them; the Briefing Room is that now.
 */

#define NAMEBOX_W 190
#define NAMEBOX_H 44

static s32 s_NameNext = 0; // Next was pressed before the sign-in finished: on to the rooms when it has

static MenuItemHandlerResult handlerNameBox(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menuitemrenderdata *rd;
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	Gfx *gdl;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)"";
	case MENUOP_GETOPTIONHEIGHT:
		data->list.value = NAMEBOX_H;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = 0;
		break;
	case MENUOP_GETOPTGROUPCOUNT:
		data->list.value = 0;
		break;
	case MENUOP_GETOPTGROUPTEXT:
		return 0;
	case MENUOP_GETGROUPSTARTINDEX:
		data->list.groupstartindex = 0;
		break;
	case MENUOP_SET:
		netLobbyClearMessage();
		s_NameNext = 0;
		menuPushDialog(&g_GhostAccountMenuDialog);
		break;
	case MENUOP_RENDER:
		gdl = data->type19.gdl;
		rd = data->type19.renderdata2;

		if (dialog) {
			const s32 focused = dialog->focuseditem == item;
			const u32 alpha = rd->colour & 0xff;
			const s32 x1 = rd->x + 10;
			const s32 x2 = rd->x + rd->width - 10;
			const s32 y1 = rd->y + 13;
			const s32 y2 = rd->y + NAMEBOX_H - 3;
			const char *name = netLobbyAccount();
			const char *prompt = "Please enter your name.";
			const u32 edge = focused ? lobbyAlpha(0xff303000, alpha) : lobbyAlpha(0x2060a000, alpha);
			s32 w;

			// the box, its edge red where the cursor is (the reference's)
			gdl = textSetPrimColour(gdl, lobbyAlpha(0x00000000, alpha * 3 / 5));
			gDPFillRectangleScaled(gdl++, x1, y1, x2, y2);
			gdl = text0f153838(gdl);
			gdl = menugfxDrawDialogBorderLine(gdl, x1, y1, x2, y1 + 1, edge, edge);
			gdl = menugfxDrawDialogBorderLine(gdl, x1, y2 - 1, x2, y2, edge, edge);
			gdl = menugfxDrawDialogBorderLine(gdl, x1, y1, x1 + 1, y2, edge, edge);
			gdl = menugfxDrawDialogBorderLine(gdl, x2 - 1, y1, x2, y2, edge, edge);

			gdl = text0f153628(gdl);
			w = lobbyTextWidth(prompt, LOBBYFONT_SM);
			gdl = lobbyText(gdl, rd->x + (rd->width - w) / 2, rd->y + 1, prompt, LOBBYFONT_SM, lobbyAlpha(0xc0c0ff00, alpha), 0);

			if (name && name[0]) {
				char buf[64];

				snprintf(buf, sizeof(buf), "%s", name);
				lobbyFit(buf, sizeof(buf), x2 - x1 - 8, LOBBYFONT_MD);
				w = lobbyTextWidth(buf, LOBBYFONT_MD);
				gdl = lobbyText(gdl, (x1 + x2 - w) / 2, (y1 + y2 - LOBBY_MDROW) / 2 + 1, buf, LOBBYFONT_MD, lobbyAlpha(0xffffff00, alpha), 0);
			} else {
				w = lobbyTextWidth("Your name", LOBBYFONT_MD);
				gdl = lobbyText(gdl, (x1 + x2 - w) / 2, (y1 + y2 - LOBBY_MDROW) / 2 + 1, "Your name", LOBBYFONT_MD, dimColour(alpha), 0);
			}

			gdl = text0f153780(gdl);
		}

		return (uintptr_t)gdl;
	}

	return 0;
}

static MenuItemHandlerResult handlerNameNext(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return s_NameNext;
	}

	if (operation == MENUOP_SET) {
		netLobbyClearMessage();

		if (netLobbySignedIn()) {
			menuPushDialog(&g_NetBriefingMenuDialog);
		} else if (netLobbyAvailable()) {
			// signed in first; dialogOnline goes on when it is in
			s_NameNext = 1;
			netLobbySignInAgain();
		} else {
			menuPushDialog(&g_GhostAccountMenuDialog);
		}
	}

	return 0;
}

static MenuDialogHandlerResult dialogOnline(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		netLobbyClearMessage();
		netLobbyWarm();
		netLobbySignInAgain();
		s_NameNext = 0;
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

		if (s_NameNext && netLobbySignedIn()) {
			s_NameNext = 0;
			menuPushDialog(&g_NetBriefingMenuDialog);
		} else if (s_NameNext && !netLobbyBusy() && netLobbyMessage()[0]) {
			// it did not sign in: why is under the box
			s_NameNext = 0;
		}
	}

	return 0;
}

// The name box, Next and Back: the dialog, in the middle of the screen
static struct menuitem s_OnlineItems[] = {
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_CUSTOMRENDER | MENUITEMFLAG_LIST_LEAVEATENDS, NAMEBOX_W, NAMEBOX_H, handlerNameBox },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)"Next\n", 0, handlerNameNext },
	// the player's own character and controls (Reverse Pitch), which go to
	// the host as this player's (netWriteSlotCfg: player 1's): Combat
	// Simulator's Player Setup (F3 20261011-225920: "y invert" online)
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_SELECTABLE_OPENSDIALOG, (uintptr_t)"Player Setup...\n", 0, (void *)&g_MpPlayerSetupViaQuickGoMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetOnlineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Online Game",
	s_OnlineItems,
	dialogOnline,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLETITLEBAR,
	NULL,
};

// In the middle of the screen, a little over the middle of the windows' room
static void lobbyNamePlace(struct menudialog *dialog, const struct lobbylayout *l)
{
	const s32 w = NAMEBOX_W + 16;
	const s32 h = dialog->contentheight;

	dialog->dstx = (SCREEN_WIDTH_LO - w) / 2;
	dialog->dsty = (l->top + l->bottom - h) / 2 - 12;
	dialog->dstwidth = w;
	dialog->dstheight = h;
}

/**
 * Under the box: what the lobby says (signing in, a wrong PIN), else what a
 * new player needs to know, centred under the dialog
 */
static Gfx *lobbyNamePanels(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l)
{
	struct menudialog *dialog = d->dialog;
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const char *text;
	char in[320];
	char out[400];
	char *p = out;
	s32 y = dialog->y + dialog->height + 6;
	s32 n = 0;

	if (!d->withtext) {
		return gdl;
	}

	if (netLobbyBusy()) {
		text = "Signing in...";
	} else if (netLobbyMessage()[0]) {
		text = netLobbyMessage();
	} else if (netLobbySignedIn()) {
		text = "Signed in. Next goes to the Briefing Room.";
	} else {
		text = "Select the box to type your name and PIN: a new name makes your account.";
	}

	gdl = lobbyAlignRestore(gdl);
	gdl = text0f153628(gdl);
	snprintf(in, sizeof(in), "%s\n", text);
	textWrapN(NAMEBOX_W + 40, in, out, sizeof(out), g_CharsHandelGothicXs, g_FontHandelGothicXs);

	while (*p && n < 4) {
		char *end = strchr(p, '\n');
		s32 w;

		if (end) {
			*end = '\0';
		}

		w = lobbyTextWidth(p, LOBBYFONT_XS);
		gdl = lobbyText(gdl, (SCREEN_WIDTH_LO - w) / 2, y, p, LOBBYFONT_XS, lobbyAlpha(0xffffff00, alpha), 0);
		y += LOBBY_XSROW;
		n++;

		if (!end) {
			break;
		}

		p = end + 1;
	}

	gdl = text0f153780(gdl);

	return gdl;
}

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
 * The room list as cards (the owner's WWE SmackDown! vs. Raw reference,
 * 2026-10-11): a room's state and mode on its first line, its name on the
 * second, its host, count and ping on the third; then a Create Room card
 * and a Refresh card, as the reference ends its list with Create Game. The
 * rest of a room (its arena, password, mods, why this game cannot join it)
 * is the Room window's on the right, for the card the cursor is on.
 */
#define ROOMROWH (2 * LOBBY_SMROW + LOBBY_XSROW + 5) // a card: the state, the name, the host

static u64 s_BriefingListAt = 0; // when the Briefing Room last asked for the list (us; every 10 s)

static void lobbyRect(Gfx **gdlp, s32 x1, s32 y1, s32 x2, s32 y2, u32 fill, u32 edge)
{
	Gfx *gdl = *gdlp;

	gdl = textSetPrimColour(gdl, fill);
	gDPFillRectangleScaled(gdl++, x1, y1, x2, y2);
	gdl = text0f153838(gdl);
	gdl = menugfxDrawDialogBorderLine(gdl, x1, y1, x2, y1 + 1, edge, edge);
	gdl = menugfxDrawDialogBorderLine(gdl, x1, y2 - 1, x2, y2, edge, edge);
	gdl = menugfxDrawDialogBorderLine(gdl, x1, y1, x1 + 1, y2, edge, edge);
	gdl = menugfxDrawDialogBorderLine(gdl, x2 - 1, y1, x2, y2, edge, edge);

	*gdlp = gdl;
}

static MenuItemHandlerResult handlerRoomList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menuitemrenderdata *rd;
	const struct netlobbyroomsum *r;
	const s32 n = netLobbyNumRooms();
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	Gfx *gdl;
	s32 index;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = n + 2;
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
		// the Room window shows the room the cursor is on; a reason from
		// the last card picked goes with the cursor
		index = (s32)data->list.value;
		s_SelectedRoom = index < n ? index : -1;
		netLobbyClearMessage();
		s_BriefingNote[0] = '\0';
		break;
	case MENUOP_SET:
		index = (s32)data->list.value;

		if (index == n) {
			netLobbyClearMessage();
			menuPushDialog(&g_NetCreateMenuDialog);
			break;
		}

		if (index > n) {
			s_BriefingNote[0] = '\0';
			netLobbyClearMessage();
			netLobbyRefresh();
			s_BriefingListAt = sysGetMicroseconds();
			break;
		}

		s_SelectedRoom = index;
		r = netLobbyRoomAt(index);

		if (r == NULL) {
			netLobbyRefresh();
		} else if (r->compat != NETLOBBY_COMPAT_OK) {
			// the reason, on the status window
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

		{
			const u32 alpha = rd->colour & 0xff;
			const s32 focused = rd->unk10 && dialog && dialog->focuseditem == item;
			const s32 x1 = rd->x + 2;
			const s32 x2 = rd->x + rd->width - 8;
			const s32 y1 = rd->y + 1;
			const s32 y2 = rd->y + ROOMROWH - 2;
			const s32 inner = x2 - x1 - 10;
			const s32 tx = x1 + 5;
			const u32 edge = focused ? lobbyAlpha(0xff303000, alpha) : lobbyAlpha(0x2060a000, alpha);
			char buf[96];
			char count[48];
			s32 w;

			lobbyRect(&gdl, x1, y1, x2, y2, lobbyAlpha(0x00000000, alpha / 2), edge);
			gdl = text0f153628(gdl);

			if (index >= n) {
				// Create Room, Refresh: the reference's Create Game card
				const char *label = index == n ? "Create Room" : "Refresh List";
				const s32 font = index == n ? LOBBYFONT_MD : LOBBYFONT_SM;

				w = lobbyTextWidth(label, font);
				gdl = lobbyText(gdl, (x1 + x2 - w) / 2, (y1 + y2 - (font == LOBBYFONT_MD ? LOBBY_MDROW : LOBBY_SMROW)) / 2 + 1,
						label, font, lobbyAlpha(0xffffff00, alpha), 0);
			} else if ((r = netLobbyRoomAt(index)) != NULL) {
				const s32 can = r->compat == NETLOBBY_COMPAT_OK;
				const char *state = !can ? "Can't join" : strcmp(r->state, "launched") == 0 ? "In a match"
					: strcmp(r->state, "countdown") == 0 ? "Starting" : "Open";
				const u32 statecol = !can ? lobbyAlpha(0xffff6000, alpha) : lobbyAlpha(0x60a0ff00, alpha);
				const u32 namecol = can ? lobbyAlpha(0xffffff00, alpha) : dimColour(alpha);
				char ping[16];
				s32 bw;

				// the state and the mode, the mode in a badge at the right
				gdl = lobbyText(gdl, tx, y1 + 2, state, LOBBYFONT_SM, statecol, inner / 2);

				snprintf(buf, sizeof(buf), "%s", roomMode(r));
				lobbyFit(buf, sizeof(buf), inner / 2, LOBBYFONT_XS);
				bw = lobbyTextWidth(buf, LOBBYFONT_XS) + lobbyRow(8);
				gdl = text0f153780(gdl);
				lobbyRect(&gdl, x2 - 4 - bw, y1 + 2, x2 - 4, y1 + 3 + LOBBY_XSROW, lobbyAlpha(0x20208000, alpha), lobbyAlpha(0x8080ff00, alpha));
				gdl = text0f153628(gdl);
				gdl = lobbyText(gdl, x2 - 4 - bw + lobbyRow(4), y1 + 3, buf, LOBBYFONT_XS, lobbyAlpha(0xffffff00, alpha), 0);

				// the name
				snprintf(buf, sizeof(buf), "%s%s", r->locked ? "[pw] " : "", r->name);
				gdl = lobbyText(gdl, tx, y1 + 2 + LOBBY_SMROW, buf, LOBBYFONT_SM, namecol, inner);

				// the host, how full, the ping
				roomPing(r, ping, sizeof(ping));
				snprintf(count, sizeof(count), "%d/%d   %s ms", r->humans, r->maxhumans, ping);
				w = lobbyTextWidth(count, LOBBYFONT_XS);
				snprintf(buf, sizeof(buf), "Host: %s", r->host);
				gdl = lobbyText(gdl, tx, y1 + 2 + 2 * LOBBY_SMROW, buf, LOBBYFONT_XS, dimColour(alpha), inner - w - 6);
				gdl = lobbyText(gdl, tx + inner - w, y1 + 2 + 2 * LOBBY_SMROW, count, LOBBYFONT_XS, dimColour(alpha), 0);
			}

			gdl = text0f153780(gdl);
		}

		return (uintptr_t)gdl;
	}

	return 0;
}

/**
 * Where the Briefing Room stands: the lobby busy or what it last said, why
 * the last room picked cannot be joined, or how to use it. No newline.
 */
/**
 * The Briefing Room's message, in the footer in the hints' place until the
 * cursor moves (which clears both): a refused join (the lobby's words, or why
 * this game cannot join the room picked), the lobby out of reach, the room
 * closed under this player. The Main Menu Chat is players' lines alone (the
 * owner, 2026-10-11: "only show actual player chat, not the Rooms: 0 etc.").
 */
static const char *briefingFootMessage(void)
{
	if (netLobbyMessage()[0]) {
		return netLobbyMessage();
	}

	return s_BriefingNote[0] ? s_BriefingNote : NULL;
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

#define BRIEFING_LIST 0 // s_BriefingItems' room list

// The cards: the dialog, the whole of the left column; a card joins its
// room, the last two create one and ask for the list again; Back is the
// menus' own back
static struct menuitem s_BriefingItems[] = {
	{ MENUITEMTYPE_LIST, 0, MENUITEMFLAG_LIST_CUSTOMRENDER, 304, 33 * 5, handlerRoomList }, // sized each frame
	{ MENUITEMTYPE_END },
};

/**
 * The list as tall as the column leaves under the buttons and as wide as
 * the column (laid out every tick, so a window resized while it is up
 * keeps it whole): its rows a whole number of lines
 */
static void briefingListFit(struct menudialog *dialog)
{
	struct lobbylayout l;
	s32 h;
	s32 i;

	lobbyLayout(&l);
	h = l.bottom - l.top - 12;

	for (i = 0; i < BRIEFING_LIST; i++) {
		s16 w16;
		s16 h16;

		menuCalculateItemSize(&s_BriefingItems[i], &w16, &h16, dialog);
		h -= h16;
	}

	h = h / ROOMROWH * ROOMROWH;
	s_BriefingItems[BRIEFING_LIST].param3 = h < ROOMROWH * 2 ? ROOMROWH * 2 : h;
	s_BriefingItems[BRIEFING_LIST].param2 = l.lx2 - l.lx1 - 4;
}

static MenuDialogHandlerResult dialogBriefing(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN) {
		s_BriefingNote[0] = '\0';
		s_SelectedRoom = -1;
		netLobbyClearMessage();
		netLobbyRefresh();
		s_BriefingListAt = sysGetMicroseconds();
	}

	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		briefingListFit(g_Menus[g_MpPlayerNum].curdialog);
		listClamp(g_Menus[g_MpPlayerNum].curdialog, &dialogdef->items[BRIEFING_LIST], ROOMROWH);
	}

	if (operation == MENUOP_TICK && isCurrent(dialogdef)) {
		netLobbyTick();

		// the main menu's chat: asked for while the page is up, and its
		// line opens with Space or the chat key (nethud.c)
		netLobbyMainChatWant();
		netHudMainChatFrame();

		if (s_WaitingForRoom && netLobbyInRoom()) {
			s_WaitingForRoom = 0;
			menuPushDialog(&g_NetRoomMenuDialog);
		} else if (s_WaitingForRoom && !netLobbyBusy()) {
			s_WaitingForRoom = 0;
		}

		// the list asked for again every 10 s while the page is up: it is
		// the main page now, where a player waits for a room to open
		if (!s_WaitingForRoom && !netLobbyBusy() && sysGetMicroseconds() - s_BriefingListAt > 10000000) {
			s_BriefingListAt = sysGetMicroseconds();
			netLobbyRefresh();
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

struct menudialogdef g_NetBriefingMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Rooms",
	s_BriefingItems,
	dialogBriefing,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * The room the cursor is on (or the first this game can join): its name,
 * why this game cannot join it, its host, arena, mode and counts, and what
 * it plays with and where this machine stands with each
 */
// The room the Briefing Room's Room window shows: the one under the cursor,
// else the one Join would join, else the first in the list
static const struct netlobbyroomsum *briefingShownRoom(void)
{
	const struct netlobbyroomsum *r = netLobbyRoomAt(s_SelectedRoom);

	if (r == NULL) {
		r = joinTarget();
	}

	if (r == NULL) {
		r = netLobbyRoomAt(0);
	}

	return r;
}

// Its details, most important first (a narrow window shows the first)
static s32 briefingPairs(const struct netlobbyroomsum *r, struct lobbysetting *pairs)
{
	s32 n = 0;

#define PAIR(lbl, ...) do { pairs[n].label = lbl; snprintf(pairs[n].value, sizeof(pairs[n].value), __VA_ARGS__); n++; } while (0)
	PAIR("Arena", "%s", r->stage[0] ? r->stage : "-");
	PAIR("Mode", "%s", roomMode(r));
	PAIR("Players", "%d/%d", r->humans, r->maxhumans);
	PAIR("Host", "%s", r->host);
	PAIR("Password", "%s", r->locked ? "Yes" : "No");
	PAIR("Status", "%s", strcmp(r->state, "launched") == 0 ? "In a match" : strcmp(r->state, "countdown") == 0 ? "Starting" : "Open");
	PAIR("Simulants", "%d", r->sims);
	PAIR("Watching", "%d/%d", r->spectators, r->maxspectators);
#undef PAIR

	return n;
}

/**
 * The Room window as tall as what it shows of the room (lobbyBriefingRoomWindow()
 * line for line): the main menu's chat under it has the rest of the column.
 * At 4:3 the details are a column of eight and the window takes most of it.
 */
static s32 briefingRoomHeight(const struct lobbylayout *l)
{
	const struct netlobbyroomsum *r = briefingShownRoom();
	const s32 inner = l->rx2 - l->rx1 - 2 * LOBBY_INSET;
	struct lobbysetting pairs[LOBBY_MAXSETTINGS];
	struct netcontentrow rows[8];
	s32 h = LINEHEIGHT + 3;
	s32 n;
	s32 nrows;

	if (r == NULL) {
		return h + 3 * LOBBY_XSROW + 6;
	}

	h += lobbyTextWidth(r->name, LOBBYFONT_MD) <= inner ? LOBBY_MDROW : LOBBY_SMROW + 2;

	if (r->compat != NETLOBBY_COMPAT_OK) {
		h += lobbyParagraphLines(netLobbyCompatText(r->compat), inner) * LOBBY_XSROW;
	}

	n = briefingPairs(r, pairs);
	h += (lobbyPairsCols(pairs, n, inner, n) == 2 ? (n + 1) / 2 : n) * LOBBY_XSROW + 4;

	nrows = netContentLobbyRows(r->mod, r->ge, NULL, rows, ARRAYCOUNT(rows));
	h += LOBBY_SMROW + 1 + (nrows > 0 ? nrows : 1) * LOBBY_SMROW;

	return h + 5;
}

static Gfx *lobbyBriefingRoomWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const struct netlobbyroomsum *r = briefingShownRoom();
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	struct lobbysetting pairs[LOBBY_MAXSETTINGS];
	struct netcontentrow rows[8];
	s32 y = y1 + LINEHEIGHT + 3;
	s32 nrows;
	s32 n = 0;
	s32 lines;

	if (r == NULL) {
		gdl = lobbyParagraph(gdl, netLobbyBusy() ? "Asking the lobby..." : "No rooms open now. Create one: everyone sees it here.",
				tx, &y, inner, normal, 3);
		return gdl;
	}

	gdl = lobbyTitleLine(gdl, r->name, "", tx, &y, inner, normal);

	if (r->compat != NETLOBBY_COMPAT_OK) {
		gdl = lobbyParagraph(gdl, netLobbyCompatText(r->compat), tx, &y, inner, lobbyAlpha(0xffff6000, alpha), 3);
	}

	n = briefingPairs(r, pairs);

	// what it plays with: its mod, its conversion (its map's mod is in its
	// rules, which the list does not carry); one column for the rest where
	// the window has the lines for it with the mods under it
	nrows = netContentLobbyRows(r->mod, r->ge, NULL, rows, ARRAYCOUNT(rows));

	gdl = lobbyPairsBlock(gdl, dialog, pairs, n, tx, y, inner,
			(y2 - 3 - y - 4 - (LOBBY_SMROW + 1) - (nrows > 0 ? nrows : 1) * LOBBY_SMROW) / LOBBY_XSROW, &lines);
	y += lines * LOBBY_XSROW + 4;

	if (y + LOBBY_SMROW + 1 + LOBBY_XSROW > y2 - 2) {
		return gdl;
	}

	gdl = lobbyText(gdl, tx, y, "Mods", LOBBYFONT_SM, headerColour(alpha), inner);
	y += LOBBY_SMROW + 1;

	if (nrows == 0) {
		gdl = lobbyText(gdl, tx, y, "None", LOBBYFONT_XS, dimColour(alpha), inner);
	} else {
		gdl = lobbyModsBlock(gdl, dialog, rows, nrows, tx, y, inner, (y2 - 2 - y) / LOBBY_SMROW, 0);
	}

	return gdl;
}

/**
 * The main menu's chat (the owner, 2026-10-11: "the small bottom right
 * window that says Lobby, let's make that a Main Menu Chat"): everyone
 * signed in, from pdlobbyd's GET /chat, drawn as the Game Lobby draws its
 * room's (lobbyChatLines()). The chat's lines and the line being typed, and
 * nothing else: the page's message is the footer's (briefingFootMessage()).
 */
static Gfx *lobbyBriefingChatWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	static struct netlobbychat chat[NETLOBBY_MAXCHAT];
	const s32 n = netLobbyMainChatAvailable() > 0 ? netLobbyMainChatLines(chat, NETLOBBY_MAXCHAT) : 0;

	return lobbyChatLines(gdl, dialog, x1, y1, x2, y2, chat, n, NULL, NULL);
}

// The list and its buttons: the whole of the left column
static void lobbyBriefingPlace(struct menudialog *dialog, const struct lobbylayout *l)
{
	dialog->dstx = l->lx1;
	dialog->dsty = l->top;
	dialog->dstwidth = l->lx2 - l->lx1;
	dialog->dstheight = l->bottom - l->top;
}

static Gfx *lobbyBriefingPanels(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l)
{
	// the room's details as tall as they are, the chat the rest, the chat
	// never less than a third of the column
	const s32 column = l->bottom - l->top;
	s32 roomh = briefingRoomHeight(l);
	s32 chaty1;

	if (roomh > column * 2 / 3 - LOBBY_VGAP) {
		roomh = column * 2 / 3 - LOBBY_VGAP;
	}

	chaty1 = l->top + roomh + LOBBY_VGAP;

	gdl = lobbyAlign(gdl, G_ASPECT_RIGHT_EXT);
	gdl = lobbyPanel(gdl, d, l->rx1, l->top, l->rx2, chaty1 - LOBBY_VGAP, "Room", lobbyBriefingRoomWindow);
	gdl = lobbyPanel(gdl, d, l->rx1, chaty1, l->rx2, l->bottom, "Main Menu Chat", lobbyBriefingChatWindow);

	return gdl;
}

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

	return 3 + n;
}

static MenuItemHandlerResult handlerGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char text[48];
	char tag[16];
	s32 n;

	if (g_GexPlusMode) {
		g_NetCoopSetup.on = 0;
		g_NetCoopSetup.campaign = 0;
		g_NetCoopSetup.anti = 0;
	}

	// 0 Combat Simulator, 1 Co-op Mission, 2 Counter-Op Mission (protocol
	// 32: the members' room teams their sides), then each set's campaign
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

		if (data->dropdown.value == 2) {
			return (intptr_t)"Counter-Op Mission";
		}

		if (!netCoopGameTag(data->dropdown.value - 3, tag, sizeof(tag))) {
			tag[0] = '\0';
		}

		snprintf(text, sizeof(text), "%s Campaign", netCoopGameName(tag));
		return (intptr_t)text;
	case MENUOP_SET:
		g_NetCoopSetup.on = data->dropdown.value >= 1 && !g_GexPlusMode;
		g_NetCoopSetup.anti = data->dropdown.value == 2 && !g_GexPlusMode;
		g_NetCoopSetup.campaign = data->dropdown.value >= 3 && !g_GexPlusMode;

		if (g_NetCoopSetup.campaign) {
			if (!netCoopGameTag(data->dropdown.value - 3, tag, sizeof(tag))) {
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
					data->dropdown.value = 3 + n;
				}
			}
		} else if (g_NetCoopSetup.on && g_NetCoopSetup.anti) {
			data->dropdown.value = 2;
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

// The room's setup: the dialog, the whole of the left column (its preview
// and what the lobby says are windows: lobbyCreatePanels)
static struct menuitem s_CreateItems[] = {
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
	(uintptr_t)"Room Setup",
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
 * focused (the windows above: "Windows").
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

static const char *ruleValueOf(const struct netlobbyroom *room, const char *key)
{
	s32 i;

	for (i = 0; i < room->nrules; i++) {
		if (strcmp(room->rules[i].key, key) == 0) {
			return room->rules[i].value;
		}
	}

	return NULL;
}

static const char *ruleValue(const char *key)
{
	return ruleValueOf(netLobbyGetRoom(), key);
}

/**
 * A room plays as teams: its host's setup has them (the Teams option, or
 * Capture the Case, which turns them on at its start), as the room's
 * "teams" rule says (netlobby.c). A co-op room has none. A host older than
 * the rule sends none: no teams then.
 */
static s32 lobbyCounterOpOf(const struct netlobbyroom *room)
{
	const char *anti = ruleValueOf(room, "counterop");

	return anti && strcmp(anti, "0") != 0 && strcmp(anti, "false") != 0;
}

/**
 * A counter-op room (protocol 32, netcoop.c): its two sides are the room's
 * two teams, 0 the agents and 1 the counter-ops - grouped and named so on
 * the roster, picked with Change Side
 */
static s32 lobbyCounterOp(void)
{
	return lobbyCounterOpOf(netLobbyGetRoom());
}

static s32 lobbyTeamsOnOf(const struct netlobbyroom *room)
{
	const char *teams = ruleValueOf(room, "teams");
	const char *key = ruleValueOf(room, "stage_key");

	if (lobbyCounterOpOf(room)) {
		return 1;
	}

	if (key && (strncmp(key, "mission:", 8) == 0 || strncmp(key, "campaign:", 9) == 0)) {
		return 0;
	}

	return teams && strcmp(teams, "0") != 0 && strcmp(teams, "false") != 0;
}

static s32 lobbyTeamsOn(void)
{
	return lobbyTeamsOnOf(netLobbyGetRoom());
}

/**
 * A member's team as the roster groups it: a counter-op room has two sides,
 * team 1 the counter-ops and any other the agents (a host's profile team
 * may be another colour; it plays an agent, netcoop.c)
 */
static s32 lobbyTeamOf(const struct netlobbymember *m)
{
	if (lobbyCounterOp()) {
		return (m->team & 7) == 1 ? 1 : 0;
	}

	return m->team & 7;
}

/**
 * A team's colour for a name on the menu: the radar's (g_TeamColours),
 * lightened as the name over a player's head is (nethud.c) so it reads on
 * the menu's blue, and darker while the player is not ready
 */
static u32 lobbyTeamColour(s32 team, s32 ready, u32 alpha)
{
	const u32 c = g_TeamColours[team & 7];
	u32 r = c >> 24 & 0xff;
	u32 g = c >> 16 & 0xff;
	u32 b = c >> 8 & 0xff;

	r += (255 - r) * 2 / 5;
	g += (255 - g) * 2 / 5;
	b += (255 - b) * 2 / 5;

	if (!ready) {
		r = r * 3 / 4;
		g = g * 3 / 4;
		b = b * 3 / 4;
	}

	return r << 24 | g << 16 | b << 8 | (alpha & 0xff);
}

/**
 * The players (not the spectators) in the order the roster lists them: the
 * room's own order, or in a team match team by team, the room's order
 * within each team. The number of them.
 */
static s32 rosterOrder(const struct netlobbymember **order, s32 teams)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	s32 n = 0;
	s32 t;
	s32 i;

	for (t = 0; t < (teams ? 8 : 1); t++) {
		for (i = 0; i < room->nmembers && n < NETLOBBY_MAXMEMBERS; i++) {
			const struct netlobbymember *m = &room->members[i];

			if (!m->spectator && (!teams || lobbyTeamOf(m) == t)) {
				order[n++] = m;
			}
		}
	}

	return n;
}

// every name whole in its column in that font, its tag as rosterCellTag leaves it
static s32 rosterNamesFit(const struct netlobbymember **order, s32 n, s32 colw, s32 font)
{
	s32 i;

	for (i = 0; i < n; i++) {
		char tag[16];
		const s32 tagw = rosterCellTag(order[i], colw, font, tag, sizeof(tag));

		if (lobbyTextWidth(order[i]->user, font) > colw - 3 - rosterMarkWidth(font) - tagw) {
			return 0;
		}
	}

	return 1;
}

/**
 * The roster's header: in a team match each team's colour name and count
 * in its colour ("Red 3  Yellow 2"), else the number of players
 */
static Gfx *lobbyRosterHeader(Gfx *gdl, struct menudialog *dialog, s32 x, s32 y, s32 width,
		const struct netlobbymember **order, s32 n, s32 teams)
{
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	char buf[48];
	s32 count[8] = { 0 };
	s32 t;
	s32 i;

	if (!teams) {
		snprintf(buf, sizeof(buf), "Players: %d", n);
		return lobbyText(gdl, x, y, buf, LOBBYFONT_SM, headerColour(alpha), width);
	}

	for (i = 0; i < n; i++) {
		count[lobbyTeamOf(order[i])]++;
	}

	for (t = 0; t < 8; t++) {
		char name[24];
		char *nl;
		s32 w;

		if (count[t] == 0) {
			continue;
		}

		// the colour's own name ("Red"): a team's name is each machine's
		// own to change (the boss file), the colour is the same everywhere;
		// a counter-op room's sides by their names
		if (lobbyCounterOp() && t <= 1) {
			snprintf(name, sizeof(name), "%s", t == 0 ? "Agents" : "Counter-Ops");
		} else {
			snprintf(name, sizeof(name), "%s", langGet(L_OPTIONS_008 + t));
		}
		nl = strchr(name, '\n');

		if (nl) {
			*nl = '\0';
		}

		snprintf(buf, sizeof(buf), "%s %d", name, count[t]);
		w = lobbyTextWidth(buf, LOBBYFONT_SM);

		if (w > width) {
			break;
		}

		gdl = lobbyText(gdl, x, y, buf, LOBBYFONT_SM, lobbyTeamColour(t, 1, alpha), 0);
		x += w + 10;
		width -= w + 10;
	}

	return gdl;
}


/**
 * The room's settings, a label and a value each: a Combat Simulator room's
 * arena, mode, host, limits, simulants and weapons; a co-op room's mission
 * (or campaign), difficulty, host, radar and friendly fire. The mods are in
 * the window under the room.
 */
static s32 lobbySettingsOf(const struct netlobbyroom *room, struct lobbysetting *out)
{
	const char *time = ruleValueOf(room, "time_limit");
	const char *score = ruleValueOf(room, "score_limit");
	const char *weapons = ruleValueOf(room, "weapons");
	const char *key = ruleValueOf(room, "stage_key");
	s32 n = 0;

#define SETTING(lbl, ...) do { out[n].label = lbl; snprintf(out[n].value, sizeof(out[n].value), __VA_ARGS__); n++; } while (0)

	if (key && (strncmp(key, "mission:", 8) == 0 || strncmp(key, "campaign:", 9) == 0)) {
		// a co-op room (netcoop.c): its mission, difficulty and options,
		// none of the Combat Simulator's limits, weapons or simulants
		const s32 campaign = key[0] == 'c';
		const char *radar = ruleValueOf(room, "radar");
		const char *ff = ruleValueOf(room, "friendly_fire");

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

		if (lobbyTeamsOnOf(room)) {
			SETTING("Teams", "%s", "On");
		}
	}

#undef SETTING

	return n;
}

/**
 * A window's first line: a name and its tail (a count) in the biggest font
 * they fit, the name cut to leave the tail whole; *y moves past it
 */
static Gfx *lobbyTitleLine(Gfx *gdl, const char *name, const char *tail, s32 tx, s32 *y, s32 inner, u32 colour)
{
	char buf[128];
	s32 font;

	snprintf(buf, sizeof(buf), "%s%s", name, tail);
	font = lobbyTextWidth(buf, LOBBYFONT_MD) <= inner ? LOBBYFONT_MD : LOBBYFONT_SM;

	if (font == LOBBYFONT_SM && lobbyTextWidth(buf, font) > inner) {
		snprintf(buf, sizeof(buf), "%s", name);
		lobbyFit(buf, sizeof(buf), inner - lobbyTextWidth(tail, font), font);
		strncat(buf, tail, sizeof(buf) - strlen(buf) - 1);
	}

	gdl = lobbyText(gdl, tx, *y, buf, font, colour, inner);
	*y += font == LOBBYFONT_MD ? LOBBY_MDROW : LOBBY_SMROW + 2;

	return gdl;
}

/**
 * Settings ("Arena: Skedar") at x, y in a window inner wide: two columns
 * where they fit, the first half (the long values) on the left. *rows is
 * the lines it took.
 */
/**
 * The settings' columns: two (one on a window too narrow for them: at 4:3 a
 * column of seven left a full room's roster no rows; the width at the
 * windows' text size). The right column's values are short (the limits, the
 * simulants): it is as wide as they are, the left one has the rest. One
 * column, a line each (as the mock-up has them), where the window has the
 * lines for it and two columns would cut a value.
 */
static s32 lobbyPairsCols(const struct lobbysetting *settings, s32 nsettings, s32 inner, s32 maxrows)
{
	s32 cols = inner >= lobbyRow(140) ? 2 : 1;
	s32 i;

	if (cols == 2 && nsettings <= maxrows) {
		s32 right = 0;

		for (i = (nsettings + 1) / 2; i < nsettings; i++) {
			if (lobbyPairWidth(&settings[i]) > right) {
				right = lobbyPairWidth(&settings[i]);
			}
		}

		for (i = 0; i < (nsettings + 1) / 2; i++) {
			if (lobbyPairWidth(&settings[i]) > inner - 6 - (right < inner / 2 ? right : inner / 2)) {
				cols = 1;
			}
		}
	}

	return cols;
}

static Gfx *lobbyPairsBlock(Gfx *gdl, struct menudialog *dialog, const struct lobbysetting *settings, s32 nsettings,
		s32 tx, s32 y, s32 inner, s32 maxrows, s32 *rows)
{
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 label = MIXCOLOUR(dialog, listgroup_headerfg);
	char buf[128];
	s32 nleft;
	s32 cols;
	s32 colw;
	s32 colw2;
	s32 col2x;
	s32 i;

	cols = lobbyPairsCols(settings, nsettings, inner, maxrows);

	// one column with fewer lines than the settings (a narrow window): the
	// first ones, which the caller puts first because they matter most;
	// none where the window has no line left
	if (cols == 1 && nsettings > maxrows) {
		nsettings = maxrows > 0 ? maxrows : 0;
	}

	nleft = cols == 2 ? (nsettings + 1) / 2 : nsettings;
	colw2 = 0;

	for (i = nleft; i < nsettings; i++) {
		s32 w;

		w = lobbyPairWidth(&settings[i]);

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

			w = lobbyPairWidth(&settings[i]);

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

	*rows = nleft;

	return gdl;
}

// A room's settings (lobbySettingsOf) the same way
static Gfx *lobbySettingsBlock(Gfx *gdl, struct menudialog *dialog, const struct netlobbyroom *room, s32 tx, s32 y, s32 inner, s32 maxrows, s32 *rows)
{
	struct lobbysetting settings[LOBBY_MAXSETTINGS];
	const s32 n = lobbySettingsOf(room, settings);

	return lobbyPairsBlock(gdl, dialog, settings, n, tx, y, inner, maxrows, rows);
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
	const u32 bright = lobbyAlpha(0x60f0f000, alpha);
	const u32 dim = dimColour(alpha);
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	char buf[128];
	s32 y = y1 + LINEHEIGHT + 3;
	s32 i;

	if (!room->valid) {
		return lobbyText(gdl, tx, y, "Waiting for the room...", LOBBYFONT_SM, normal, inner);
	}

	// the room's name and count are the header's (lobbyInfoRoom)
	// two columns: the roster needs the rows (a window too narrow for two
	// has four lines of them)
	gdl = lobbySettingsBlock(gdl, dialog, room, tx, y, inner, 4, &i);
	y += i * LOBBY_XSROW + 4;

	// the players: one list (the owner, 2026-10-11: no team columns), in a
	// team match grouped by team, each name in its team's colour; the
	// spectators on a line of their own at the foot
	{
		const struct netlobbymember *order[NETLOBBY_MAXMEMBERS];
		s32 cellcol[NETLOBBY_MAXMEMBERS];
		s32 cellrow[NETLOBBY_MAXMEMBERS];
		const s32 teams = lobbyTeamsOn();
		const s32 nplayers = rosterOrder(order, teams);
		const s32 specs = rosterCount(2);
		s32 avail;
		s32 fit = 1;
		s32 ncols = 1;
		s32 colw = inner;
		s32 rowh = LOBBY_SMROW;
		s32 nfont = LOBBYFONT_XS;
		s32 f;

		gdl = lobbyRosterHeader(gdl, dialog, tx, y, inner, order, nplayers, teams);
		y += LOBBY_SMROW + 1;
		avail = y2 - 3 - y - (specs > 0 ? LOBBY_XSROW : 0);

		// the bigger font while every name is whole in it, in as few
		// columns as the rows allow (three at most, never one wide one:
		// the pings stay near their names)
		for (f = LOBBYFONT_SM; f >= LOBBYFONT_XS; f--) {
			rowh = f == LOBBYFONT_SM ? LOBBY_SMROW : LOBBY_XSROW;
			fit = avail / rowh;

			if (fit < 1) {
				fit = 1;
			}

			ncols = (nplayers + fit - 1) / fit;

			if (ncols < 1) {
				ncols = 1;
			}

			if (ncols > 3) {
				ncols = 3;
			}

			colw = inner / (ncols < 2 ? 2 : ncols);
			nfont = f;

			if (f == LOBBYFONT_XS || (nplayers <= fit * 3 && rosterNamesFit(order, nplayers, colw, f))) {
				break;
			}
		}

		// down each column, the columns even; in a team match a team that
		// would wrap from the foot of a column to the next starts that
		// column instead, where it fits whole in it (kept together, not a
		// column per team: two small teams share one)
		if (!teams && nplayers <= fit * ncols) {
			fit = (nplayers + ncols - 1) / ncols;
		}

		{
			s32 col = 0;
			s32 row = 0;

			for (i = 0; i < nplayers; i++) {
				if (teams && (i == 0 || lobbyTeamOf(order[i]) != lobbyTeamOf(order[i - 1]))) {
					s32 g = 1;

					while (i + g < nplayers && lobbyTeamOf(order[i + g]) == lobbyTeamOf(order[i])) {
						g++;
					}

					if (row > 0 && row + g > fit && g <= fit && col + 1 < 3) {
						col++;
						row = 0;
					}
				}

				if (row >= fit) {
					col++;
					row = 0;
				}

				cellcol[i] = col;
				cellrow[i] = row++;
			}

			// the columns that takes, as many as the window has
			if (nplayers > 0 && cellcol[nplayers - 1] + 1 > ncols) {
				ncols = cellcol[nplayers - 1] + 1 > 3 ? 3 : cellcol[nplayers - 1] + 1;
				colw = inner / (ncols < 2 ? 2 : ncols);
			}
		}

		for (i = 0; i < nplayers; i++) {
			const struct netlobbymember *m = order[i];
			const s32 cx = tx + cellcol[i] * colw;
			const s32 cy = y + cellrow[i] * rowh;
			const u32 col = teams ? lobbyTeamColour(lobbyTeamOf(m), m->ready || m->host, alpha) : m->ready || m->host ? bright : dim;
			char tag[16];
			s32 tagw;

			// past the last column: the last place says how many more
			if (cellcol[i] >= ncols || (i + 1 < nplayers && cellcol[i + 1] >= ncols && cellrow[i] == fit - 1)) {
				snprintf(buf, sizeof(buf), "+%d more", nplayers - i);
				gdl = lobbyText(gdl, cellcol[i] >= ncols ? tx + (ncols - 1) * colw : cx,
						cellcol[i] >= ncols ? y + (fit - 1) * rowh : cy, buf, nfont, dim, colw - 4);
				break;
			}

			// the ready mark, the name, the path and ping at the column's
			// right; the name cut to what is left
			tagw = rosterCellTag(m, colw, nfont, tag, sizeof(tag));

			if (m->ready || m->host) {
				gdl = lobbyText(gdl, cx, cy, "*", nfont, col, 0);
			}

			gdl = lobbyText(gdl, cx + rosterMarkWidth(nfont), cy, m->user, nfont, col, colw - 3 - rosterMarkWidth(nfont) - tagw);

			if (tagw) {
				gdl = lobbyTextRight(gdl, cx + colw - 3, cy + (nfont == LOBBYFONT_SM ? 2 : 0), tag, LOBBYFONT_XS, dim);
			}
		}

		if (specs > 0) {
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
/**
 * Content rows (netContentLobbyRows) at x, y: the name, and its state at
 * the right in its colour (on its way orange, to come from the host
 * yellow). As many rows as fit; with empties, the rows left over say
 * <empty>, as the Game Lobby's mock-up has them.
 */
static Gfx *lobbyModsBlock(Gfx *gdl, struct menudialog *dialog, const struct netcontentrow *rows, s32 nrows,
		s32 tx, s32 y, s32 inner, s32 fit, s32 empties)
{
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 disabled = MIXCOLOUR(dialog, item_disabled);
	char state[48];
	s32 i;

	for (i = 0; i < fit; i++, y += LOBBY_SMROW) {
		u32 colour;
		s32 sw;

		if (i >= nrows) {
			if (!empties) {
				break;
			}

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

static Gfx *lobbyModsWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	struct netcontentrow rows[8];
	s32 nrows = 0;

	if (room->valid) {
		nrows = netContentLobbyRows(room->sum.mod, room->sum.ge, ruleValue("stage_key"), rows, ARRAYCOUNT(rows));
	}

	return lobbyModsBlock(gdl, dialog, rows, nrows, x1 + LOBBY_INSET, y1 + LINEHEIGHT + 3, x2 - x1 - 2 * LOBBY_INSET,
			(y2 - 2 - (y1 + LINEHEIGHT + 3)) / LOBBY_SMROW, 1);
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

	return lobbyChatLines(gdl, dialog, x1, y1, x2, y2, room->chat, room->nchat,
			"No chat yet: press Space to talk.", netLobbyMenuStatusText());
}

/**
 * A chat window's lines: who said what, wrapped, the newest at the foot;
 * `empty` when nobody has, then the page's status, then the line being
 * typed (nethud.c) - as many of the last as fit
 */
static Gfx *lobbyChatLines(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2,
		const struct netlobbychat *chat, s32 nchat, const char *empty, const char *status)
{
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 label = MIXCOLOUR(dialog, listgroup_headerfg);
	const u32 white = MIXCOLOUR(dialog, item_focused_inner);
	// the lobby's own lines (pdlobbyd's notices): the match's chat's notice colour
	const u32 notice = 0xffd25a00 | alpha;
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	static struct lobbychatline lines[LOBBY_MAXCHATLINES];
	char text[sizeof(lines[0].text) + 128];
	s32 nlines = 0;
	s32 fit = (y2 - 2 - (y1 + LINEHEIGHT + 3)) / LOBBY_XSROW;
	s32 first;
	s32 y;
	s32 i;

	for (i = 0; i < nchat; i++) {
		snprintf(text, sizeof(text), "%s: %s", chat[i].user, chat[i].text);

		if (chat[i].system) {
			// the server's: all of it in the one colour
			nlines = lobbyChatWrap(lines, nlines, text, 0, notice, inner);
		} else {
			nlines = lobbyChatWrap(lines, nlines, text, strlen(chat[i].user) + 2, normal, inner);
		}
	}

	if (nchat == 0 && empty && empty[0]) {
		nlines = lobbyChatWrap(lines, nlines, empty, 0, dimColour(alpha), inner);
	}

	if (status && status[0]) {
		nlines = lobbyChatWrap(lines, nlines, status, 0, white, inner);
	}

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

/**
 * Change Team in a team match: Red, Yellow, watching (a member), Red. With
 * no teams the roster shows none, so the row is Spectate or Play, and a
 * host (who cannot watch its own room) has none.
 */
static char *textTeam(struct menuitem *item)
{
	if (lobbyCounterOp()) {
		return "Change Side\n";
	}

	if (lobbyTeamsOn()) {
		return "Change Team\n";
	}

	return netLobbyMyTeam() < 0 ? "Play\n" : "Spectate\n";
}

static MenuItemHandlerResult handlerTeam(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !lobbyTeamsOn() && netLobbyIsHost();
	}

	if (operation == MENUOP_CHECKDISABLED) {
		return !roomIsOpen();
	}

	if (operation == MENUOP_SET) {
		if (lobbyTeamsOn()) {
			netLobbyCycleTeam();
		} else {
			netLobbySetSpectator(netLobbyMyTeam() >= 0);
		}
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
		} else if (lobbyCounterOp() && !netLobbySidesOk()) {
			// a counter-op mission is played by both sides
			netLobbySetMessage("A counter-op mission needs an agent and a counter-op: Change Side.");
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

static MenuItemHandlerResult handlerRejoin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !netLobbyCanRejoin();
	}

	if (operation == MENUOP_SET) {
		netLobbyRejoin();
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
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back Into the Match\n", 0, handlerRejoin },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textReady, 0, handlerReady },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&textTeam, 0, handlerTeam },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Chat\n", 0, handlerChat },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG, (uintptr_t)"Player Setup...\n", 0, (void *)&g_MpPlayerSetupViaQuickGoMenuDialog },
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

/**
 * The mods window: as many rows as the room plays with, two to four (the
 * rows left over say <empty>, as the mock-up has them); the roster has the
 * rest of the column
 */
static s32 lobbyModsHeight(void)
{
	const struct netlobbyroom *room = netLobbyGetRoom();
	struct netcontentrow rows[8];
	s32 n = room->valid ? netContentLobbyRows(room->sum.mod, room->sum.ge, ruleValue("stage_key"), rows, ARRAYCOUNT(rows)) : 0;

	n = n < 2 ? 2 : n > 4 ? 4 : n;

	return LINEHEIGHT + 3 + n * LOBBY_SMROW + 2;
}

/**
 * The Game Lobby's buttons at the foot of the right column, as wide as the
 * column and as tall as their rows (dialogCalculateContentSize, every tick:
 * the host's rows come and go)
 */
static void lobbyRoomPlace(struct menudialog *dialog, const struct lobbylayout *l)
{
	s32 y;
	s32 h;

	lobbyButtonsAtFoot(l, dialog->contentheight, &y, &h);

	dialog->dstx = l->rx1;
	dialog->dsty = y;
	dialog->dstwidth = l->rx2 - l->rx1;
	dialog->dstheight = h;
}

/**
 * The windows around the Game Lobby's buttons: the room and its mods in the
 * left column, the chat in the right over the buttons, whose box the
 * chat's foot follows as it grows and shrinks
 */
static Gfx *lobbyRoomPanels(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l)
{
	const s32 modsy1 = l->bottom - lobbyModsHeight();

	gdl = lobbyAlign(gdl, G_ASPECT_LEFT_EXT);
	gdl = lobbyPanel(gdl, d, l->lx1, l->top, l->lx2, modsy1 - LOBBY_VGAP, "Room", lobbyRoomWindow);
	gdl = lobbyPanel(gdl, d, l->lx1, modsy1, l->lx2, l->bottom, "Mods", lobbyModsWindow);

	gdl = lobbyAlign(gdl, G_ASPECT_RIGHT_EXT);
	gdl = lobbyPanel(gdl, d, l->rx1, l->top, l->rx2, d->dialog->y + LINEHEIGHT - LOBBY_VGAP, "Lobby Chat", lobbyChatWindow);

	return gdl;
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
	NETLOBBY_SETUP_ITEMS,
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Apply\n", 0, handlerSettingsApply },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetRoomSettingsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Room Setup", s_RoomSettingsItems, dialogRoomSettings, MENUDIALOGFLAG_LITERAL_TEXT, NULL,
};

/**
 * Create Room's and Room Settings' preview: the room as the lobby will list
 * it from the setup as it is now (netLobbyPreviewRoom: the same JSON the
 * create and a settings change send), so the Briefing Room and the Game
 * Lobby show what this does
 */
static Gfx *lobbyPreviewWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	static struct netlobbyroom preview;
	const struct netlobbyroom *room = netLobbyGetRoom();
	const s32 settings = dialog->definition == &g_NetRoomSettingsMenuDialog;
	const u32 normal = MIXCOLOUR(dialog, item_unfocused);
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	struct netcontentrow rows[8];
	char tail[32];
	s32 y = y1 + LINEHEIGHT + 3;
	s32 lines;
	s32 nrows;

	netLobbyPreviewRoom(&preview);
	snprintf(preview.sum.host, sizeof(preview.sum.host), "%s", netLobbyAccount());

	if (settings && room->valid) {
		snprintf(tail, sizeof(tail), " (%d/%d)", room->sum.humans, room->sum.maxhumans);
		gdl = lobbyTitleLine(gdl, room->sum.name, tail, tx, &y, inner, normal);
	} else {
		snprintf(tail, sizeof(tail), " (1/%d)", s_Create.maxhumans);
		gdl = lobbyTitleLine(gdl, s_Create.name, tail, tx, &y, inner, normal);
		gdl = lobbyText(gdl, tx, y, s_RoomPassword[0] ? "Password: set" : "Password: none", LOBBYFONT_XS, dimColour(alpha), inner);
		y += LOBBY_XSROW + 2;
	}

	gdl = lobbySettingsBlock(gdl, dialog, &preview, tx, y, inner, LOBBY_MAXSETTINGS, &lines);
	y += lines * LOBBY_XSROW + 4;

	nrows = netContentLobbyRows(preview.sum.mod, preview.sum.ge, ruleValueOf(&preview, "stage_key"), rows, ARRAYCOUNT(rows));

	gdl = lobbyText(gdl, tx, y, "Mods", LOBBYFONT_SM, headerColour(alpha), inner);
	y += LOBBY_SMROW + 1;

	if (nrows == 0) {
		gdl = lobbyText(gdl, tx, y, "None", LOBBYFONT_XS, dimColour(alpha), inner);
	} else {
		gdl = lobbyModsBlock(gdl, dialog, rows, nrows, tx, y, inner, (y2 - 2 - y) / LOBBY_SMROW, 0);
	}

	return gdl;
}

/**
 * What the lobby says to a create ("" nothing), and what the page's
 * button does
 */
static const char *previewStatusText(void)
{
	return netLobbyBusy() ? "Asking the lobby..." : netLobbyMessage();
}

static const char *previewHint(struct menudialog *dialog)
{
	return dialog->definition == &g_NetRoomSettingsMenuDialog ? "New settings un-ready everyone."
		: "Create opens the room: it shows in everyone's Briefing Room.";
}

static Gfx *lobbyPreviewStatusWindow(Gfx *gdl, struct menudialog *dialog, s32 x1, s32 y1, s32 x2, s32 y2)
{
	const u32 white = MIXCOLOUR(dialog, item_focused_inner);
	const u32 alpha = (MIXCOLOUR(dialog, item_unfocused)) & 0xff;
	const s32 inner = x2 - x1 - 2 * LOBBY_INSET;
	const s32 tx = x1 + LOBBY_INSET;
	s32 y = y1 + LINEHEIGHT + 3;

	if (previewStatusText()[0]) {
		gdl = lobbyParagraph(gdl, previewStatusText(), tx, &y, inner, white, 8);
		y += 2;
	}

	gdl = lobbyParagraph(gdl, previewHint(dialog), tx, &y, inner, dimColour(alpha), 8);

	return gdl;
}

// The setup's rows: the whole of the left column (more than fit scroll)
static void lobbyPreviewPlace(struct menudialog *dialog, const struct lobbylayout *l)
{
	dialog->dstx = l->lx1;
	dialog->dsty = l->top;
	dialog->dstwidth = l->lx2 - l->lx1;
	dialog->dstheight = l->bottom - l->top;
}

static Gfx *lobbyPreviewPanels(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l)
{
	const s32 statusy1 = l->bottom - lobbyStatusHeight(l, previewHint(d->dialog), previewStatusText()[0] ? previewStatusText() : NULL);

	gdl = lobbyAlign(gdl, G_ASPECT_RIGHT_EXT);
	gdl = lobbyPanel(gdl, d, l->rx1, l->top, l->rx2, statusy1 - LOBBY_VGAP,
			d->dialog->definition == &g_NetRoomSettingsMenuDialog ? "Room" : "Room Preview", lobbyPreviewWindow);
	gdl = lobbyPanel(gdl, d, l->rx1, statusy1, l->rx2, l->bottom, "Lobby", lobbyPreviewStatusWindow);

	return gdl;
}

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

		// the Briefing Room between them: Leave lands on the rooms, as it
		// does for a room joined from them (Online Game is Name Entry now)
		menuPushDialog(&g_NetBriefingMenuDialog);
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

	if (top == &g_NetOnlineMenuDialog) {
		menuPushDialog(&g_NetBriefingMenuDialog);
	}

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

/**
 * The lobby test (netlobbytest.sh rematch, --net-test-chat-at-launch): the
 * Game Lobby's Chat opened as its row opens it, over the Game Lobby, by a
 * keyboard player (typing at once: menuitemKeyboardInit). Whether it types.
 */
s32 netLobbyMenuPushChat(void)
{
	netLobbyMenuPushRoom();

	if (!isCurrent(&s_ChatDialog)) {
		menuPushDialog(&s_ChatDialog);
	}

	return inputIsTextInputActive();
}

// The lobby test's screenshots: is that page the one on screen?
s32 netLobbyMenuPageUp(s32 room)
{
	return isCurrent(room ? &g_NetRoomMenuDialog : &g_NetBriefingMenuDialog);
}

/*
 * The pages in windows ("Windows" above), and what menu.c asks of each
 * dialog: its side, its place, its windows
 */

struct lobbypage {
	struct menudialogdef *def;
	s32 side; // G_ASPECT_LEFT_EXT or RIGHT_EXT: the column the dialog is in; 0 the middle
	void (*place)(struct menudialog *dialog, const struct lobbylayout *l);
	Gfx *(*panels)(Gfx *gdl, const struct lobbydraw *d, const struct lobbylayout *l);
	const char *title;                      // the header's name for the page
	void (*info)(char *out, s32 size);      // what the header's right says (NULL nothing)
	const char *hints;                      // the footer's
	const char *(*foot)(void);              // a message for the footer in the hints' place (NULL none)
};

// The header's right: who this machine plays as, and how many rooms are open
static void lobbyInfoRooms(char *out, s32 size)
{
	const s32 n = netLobbyNumRooms();

	snprintf(out, size, "%d room%s   %s", n, n == 1 ? "" : "s", netLobbySignedIn() ? netLobbyAccount() : "");
}

static void lobbyInfoAccount(char *out, s32 size)
{
	snprintf(out, size, "%s", netLobbySignedIn() ? netLobbyAccount() : "");
}

// the room's name and how full it is
static void lobbyInfoRoom(char *out, s32 size)
{
	const struct netlobbyroom *room = netLobbyGetRoom();

	if (room->valid) {
		snprintf(out, size, "%s   %d/%d", room->sum.name, room->sum.humans, room->sum.maxhumans);
	} else {
		out[0] = '\0';
	}
}

#define LOBBY_HINTS_SELECT "A / Enter: Select     B / Esc: Back"

static const struct lobbypage s_LobbyPages[] = {
	{ &g_NetOnlineMenuDialog, 0, lobbyNamePlace, lobbyNamePanels, "NAME ENTRY", NULL, LOBBY_HINTS_SELECT },
	{ &g_NetBriefingMenuDialog, G_ASPECT_LEFT_EXT, lobbyBriefingPlace, lobbyBriefingPanels, "BRIEFING ROOM", lobbyInfoRooms,
		"A / Enter: Join     Space or T: Chat     B / Esc: Back", briefingFootMessage },
	{ &g_NetCreateMenuDialog, G_ASPECT_LEFT_EXT, lobbyPreviewPlace, lobbyPreviewPanels, "CREATE ROOM", lobbyInfoAccount, LOBBY_HINTS_SELECT },
	{ &g_NetRoomSettingsMenuDialog, G_ASPECT_LEFT_EXT, lobbyPreviewPlace, lobbyPreviewPanels, "ROOM SETTINGS", lobbyInfoRoom, LOBBY_HINTS_SELECT },
	{ &g_NetRoomMenuDialog, G_ASPECT_RIGHT_EXT, lobbyRoomPlace, lobbyRoomPanels, "GAME LOBBY", lobbyInfoRoom,
		"A / Enter: Select     Space or T: Chat" },
};

static const struct lobbypage *lobbyPageOf(struct menudialogdef *def)
{
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(s_LobbyPages); i++) {
		if (s_LobbyPages[i].def == def) {
			return &s_LobbyPages[i];
		}
	}

	return NULL;
}

s32 netLobbyMenuDialogAlign(struct menudialogdef *def)
{
	const struct lobbypage *page = lobbyPageOf(def);

	return page ? page->side : 0;
}

s32 netLobbyMenuDialogAlignOffset(struct menudialogdef *def)
{
	const struct lobbypage *page = lobbyPageOf(def);

	if (!page) {
		return 0;
	}

	return page->side == G_ASPECT_RIGHT_EXT ? lobbyPillar() : -lobbyPillar();
}

void netLobbyMenuPlaceDialog(struct menudialog *dialog)
{
	const struct lobbypage *page = lobbyPageOf(dialog->definition);
	struct lobbylayout l;

	if (page && page->place) {
		lobbyLayout(&l);
		page->place(dialog, &l);
	}
}

/**
 * A page's windows, after its dialog (menuRenderDialog): they slide with
 * the dialog, grow open with it and take its text effects; while it grows
 * open they are frames
 */
void *netLobbyMenuRenderPanels(void *gdlptr, struct menudialog *dialog)
{
	const struct lobbypage *page = lobbyPageOf(dialog->definition);
	Gfx *gdl = gdlptr;
	struct lobbylayout l;
	struct lobbydraw d;

	if (!page || !page->panels) {
		return gdl;
	}

	lobbyLayout(&l);

	d.dialog = dialog;
	d.withtext = dialog->state != MENUDIALOGSTATE_PREOPEN;
	d.grow = 1.0f;
	d.dx = page->side == G_ASPECT_LEFT_EXT ? dialog->x - l.lx1 : page->side == G_ASPECT_RIGHT_EXT ? dialog->x - l.rx1 : 0;

	if (dialog->state == MENUDIALOGSTATE_PREOPEN) {
		// as dialogRender() grows the dialog open
		d.grow = 1.0f - g_MenuData.unk010;
		d.grow = 1.0f - d.grow * d.grow;
	}

	gSPClearGeometryMode(gdl++, G_ZBUFFER);

	{
		char info[96];

		info[0] = '\0';

		if (page->info) {
			page->info(info, sizeof(info));
		}

		gdl = lobbyChrome(gdl, &d, &l, page->title, info, page->hints, page->foot ? page->foot() : NULL);
	}

	gdl = page->panels(gdl, &d, &l);

	return lobbyAlignRestore(gdl);
}
